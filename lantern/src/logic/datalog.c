#include "logic/datalog.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/list.h"

typedef struct datalog_tuple datalog_tuple_t;

typedef struct {
  core_datalog_relation_id_t relation;
  datalog_tuple_t *tuple;
} datalog_premise_t;
core_list_decl(datalog_premise_t);

struct datalog_tuple {
  core_datalog_symbol_t *values;
  int32_t arity;
  int32_t rule;
  datalog_premise_t *premises;
  size_t premise_count;
  bool recent;
  bool pending;
};
typedef datalog_tuple_t *datalog_tuple_ptr_t;
core_list_decl(datalog_tuple_ptr_t);
core_list_decl(core_datalog_rule_t);
core_list_decl(core_datalog_goal_t);
typedef core_string_t *datalog_string_ptr_t;
core_list_decl(datalog_string_ptr_t);

typedef struct {
  size_t *data;
  size_t len;
  size_t cap;
} datalog_offset_list_t;

typedef struct {
  core_datalog_relation_id_t id;
  core_string_t name;
  int32_t arity;
  int32_t stratum;
  core_list_t(datalog_tuple_ptr_t) tuples;
  core_dict_t tuple_set;
  core_dict_t *column_indexes;
  bool indexes_dirty;
} datalog_relation_t;
typedef datalog_relation_t *datalog_relation_ptr_t;
core_list_decl(datalog_relation_ptr_t);

typedef struct {
  core_datalog_symbol_t variable;
  core_datalog_symbol_t value;
} datalog_binding_t;
core_list_decl(datalog_binding_t);

struct core_datalog_engine {
  core_list_t(datalog_relation_ptr_t) relations;
  core_list_t(core_datalog_rule_t) rules;
  core_list_t(datalog_string_ptr_t) constants;
  core_list_t(datalog_string_ptr_t) variables;
  core_dict_t relation_names;
  core_dict_t constant_names;
  core_dict_t variable_names;
  core_datalog_symbol_t next_variable;
  int64_t iterations;
  int64_t fact_count;
  bool provenance;
  core_list_t(datalog_premise_t) trail;
};

static core_hash_value_t tuple_hash(void *key) {
  const datalog_tuple_t *tuple = key;
  return core_hash_raw(tuple->values,
                       (size_t)tuple->arity * sizeof(*tuple->values));
}

static bool tuple_eq(const void *left, const void *right) {
  const datalog_tuple_t *a = left;
  const datalog_tuple_t *b = right;
  return a->arity == b->arity &&
         !memcmp(a->values, b->values, (size_t)a->arity * sizeof(*a->values));
}

struct core_datalog_rule_builder {
  core_datalog_literal_t head;
  core_list_t(core_datalog_goal_t) body;
  bool has_head;
};

core_datalog_rule_builder_t *core_datalog_rule_builder_new(void) {
  core_datalog_rule_builder_t *builder =
      core_alloc(core_datalog_rule_builder_t);
  memset(builder, 0, sizeof(*builder));
  builder->body = core_list_new(core_datalog_goal_t);
  return builder;
}

bool core_datalog_rule_builder_head(core_datalog_rule_builder_t *builder,
                                    core_datalog_relation_id_t relation,
                                    int32_t arity,
                                    const core_datalog_symbol_t *arguments) {
  if (!builder || arity < 0 || (arity && !arguments))
    return false;
  core_free(builder->head.arguments);
  builder->head = (core_datalog_literal_t){
      .relation = relation,
      .arity = arity,
      .arguments =
          core_alloc_array(core_datalog_symbol_t, arity ? (size_t)arity : 1),
  };
  if (arity)
    memcpy(builder->head.arguments, arguments,
           (size_t)arity * sizeof(*arguments));
  builder->has_head = true;
  return true;
}

bool core_datalog_rule_builder_add_literal(
    core_datalog_rule_builder_t *builder, core_datalog_relation_id_t relation,
    int32_t arity, const core_datalog_symbol_t *arguments, bool negated) {
  if (!builder || arity < 0 || (arity && !arguments))
    return false;
  core_datalog_literal_t literal = {
      .relation = relation,
      .arity = arity,
      .arguments =
          core_alloc_array(core_datalog_symbol_t, arity ? (size_t)arity : 1),
      .negated = negated,
  };
  if (arity)
    memcpy(literal.arguments, arguments, (size_t)arity * sizeof(*arguments));
  core_list_push(builder->body,
                 core_variant_make_1(core_datalog_goal_t, literal));
  return true;
}

bool core_datalog_rule_builder_add_builtin(core_datalog_rule_builder_t *builder,
                                           core_datalog_builtin_t builtin) {
  if (!builder)
    return false;
  core_list_push(builder->body,
                 core_variant_make_2(core_datalog_goal_t, builtin));
  return true;
}

core_datalog_rule_t
core_datalog_rule_builder_finish(core_datalog_rule_builder_t *builder) {
  if (!builder || !builder->has_head)
    return (core_datalog_rule_t){0};
  core_datalog_rule_t rule = {
      .head = builder->head,
      .body = builder->body.data,
      .body_count = builder->body.len,
  };
  builder->head.arguments = nullptr;
  builder->body = (core_list_t(core_datalog_goal_t)){0};
  builder->has_head = false;
  core_datalog_rule_builder_free(builder);
  return rule;
}

void core_datalog_rule_builder_free(core_datalog_rule_builder_t *builder) {
  if (!builder)
    return;
  core_free(builder->head.arguments);
  for (size_t i = 0; i < builder->body.len; i++) {
    core_datalog_goal_t goal = builder->body.data[i];
    if (core_variant_is(goal, 1))
      core_free(core_variant_get_1(goal).arguments);
    else if (core_variant_is(goal, 2)) {
      core_datalog_builtin_t builtin = core_variant_get_2(goal);
      core_datalog_expr_free(builtin.left);
      core_datalog_expr_free(builtin.right);
    }
  }
  core_list_free(builder->body);
  core_free(builder);
}

static bool valid_symbol(core_datalog_symbol_t symbol) {
  return core_datalog_is_variable(symbol) || symbol >= 0;
}

core_datalog_engine_t *core_datalog_engine_new(void) {
  core_datalog_engine_t *engine = core_alloc(core_datalog_engine_t);
  memset(engine, 0, sizeof(*engine));
  engine->relations = core_list_new(datalog_relation_ptr_t);
  engine->rules = core_list_new(core_datalog_rule_t);
  engine->constants = core_list_new(datalog_string_ptr_t);
  engine->variables = core_list_new(datalog_string_ptr_t);
  engine->next_variable = CORE_DATALOG_VARIABLE_BASE;
  engine->trail = core_list_new(datalog_premise_t);
  core_dict_init_string(&engine->relation_names);
  core_dict_init_string(&engine->constant_names);
  core_dict_init_string(&engine->variable_names);
  return engine;
}

static void expression_free(core_datalog_expr_t *expression) {
  core_datalog_expr_free(expression);
}

void core_datalog_rule_free(core_datalog_rule_t *rule) {
  if (!rule)
    return;
  core_free(rule->head.arguments);
  for (size_t i = 0; i < rule->body_count; i++) {
    core_datalog_goal_t goal = rule->body[i];
    if (core_variant_is(goal, 1)) {
      core_free(core_variant_get_1(goal).arguments);
    } else if (core_variant_is(goal, 2)) {
      core_datalog_builtin_t builtin = core_variant_get_2(goal);
      expression_free(builtin.left);
      expression_free(builtin.right);
    }
  }
  core_free(rule->body);
  *rule = (core_datalog_rule_t){0};
}

void core_datalog_engine_free(core_datalog_engine_t *engine) {
  if (!engine)
    return;
  for (size_t i = 0; i < engine->relations.len; i++) {
    datalog_relation_t *relation = engine->relations.data[i];
    for (size_t j = 0; j < relation->tuples.len; j++) {
      core_free(relation->tuples.data[j]->values);
      core_free(relation->tuples.data[j]->premises);
      core_free(relation->tuples.data[j]);
    }
    core_list_free(relation->tuples);
    core_dict_free(&relation->tuple_set);
    for (int32_t col = 0; col < relation->arity; col++) {
      core_dict_t *index = &relation->column_indexes[col];
      core_dict_foreach(index, entry) core_free(entry->value);
      core_dict_free(index);
    }
    core_free(relation->column_indexes);
    core_free(relation->name.data);
    core_free(relation);
  }
  for (size_t i = 0; i < engine->rules.len; i++)
    core_datalog_rule_free(&engine->rules.data[i]);
  for (size_t i = 0; i < engine->constants.len; i++) {
    core_free(engine->constants.data[i]->data);
    core_free(engine->constants.data[i]);
  }
  for (size_t i = 0; i < engine->variables.len; i++) {
    core_free(engine->variables.data[i]->data);
    core_free(engine->variables.data[i]);
  }
  core_dict_free(&engine->relation_names);
  core_dict_free(&engine->constant_names);
  core_dict_free(&engine->variable_names);
  core_list_free(engine->relations);
  core_list_free(engine->rules);
  core_list_free(engine->constants);
  core_list_free(engine->variables);
  core_list_free(engine->trail);
  core_free(engine);
}

core_datalog_relation_id_t core_datalog_relation(core_datalog_engine_t *engine,
                                                 core_string_t name,
                                                 int32_t arity) {
  if (!engine || !name.data || arity < 0)
    return -1;
  bool found = false;
  void *value = core_dict_get(&engine->relation_names, &name, &found);
  if (found) {
    datalog_relation_t *existing = engine->relations.data[(size_t)value];
    return existing->arity == arity ? existing->id : -1;
  }
  datalog_relation_t *relation = core_alloc(datalog_relation_t);
  memset(relation, 0, sizeof(*relation));
  relation->id = (core_datalog_relation_id_t)engine->relations.len;
  relation->name = core_string_dup(name);
  relation->arity = arity;
  relation->tuples = core_list_new(datalog_tuple_ptr_t);
  core_dict_init(&relation->tuple_set, tuple_hash, tuple_eq);
  relation->column_indexes =
      arity ? core_alloc_array(core_dict_t, (size_t)arity) : nullptr;
  relation->indexes_dirty = true;
  for (int32_t i = 0; i < arity; i++)
    core_dict_init(&relation->column_indexes[i], core_hash_word, nullptr);
  core_list_push(engine->relations, relation);
  core_dict_put(&engine->relation_names, &relation->name,
                (uintptr_t)relation->id);
  return relation->id;
}

core_option_t(core_datalog_relation_id_t)
    core_datalog_find_relation(core_datalog_engine_t *engine,
                               core_string_t name) {
  if (!engine || !name.data)
    return core_option_none(core_datalog_relation_id_t);
  bool found = false;
  void *value = core_dict_get(&engine->relation_names, &name, &found);
  if (!found)
    return core_option_none(core_datalog_relation_id_t);
  return core_option_set(core_datalog_relation_id_t,
                         (core_datalog_relation_id_t)(uintptr_t)value);
}

core_string_t core_datalog_relation_name(core_datalog_engine_t *engine,
                                         core_datalog_relation_id_t relation) {
  if (!engine || relation < 0 || (size_t)relation >= engine->relations.len)
    return (core_string_t){0};
  return engine->relations.data[relation]->name;
}

core_option_t(int32_t)
    core_datalog_relation_arity(core_datalog_engine_t *engine,
                                core_datalog_relation_id_t relation) {
  if (!engine || relation < 0 || (size_t)relation >= engine->relations.len)
    return core_option_none(int32_t);
  return core_option_set(int32_t, engine->relations.data[relation]->arity);
}

static core_datalog_symbol_t
intern_name(core_list_t(datalog_string_ptr_t) * names, core_dict_t *index,
            core_string_t name) {
  bool found = false;
  void *value = core_dict_get(index, &name, &found);
  if (found)
    return (core_datalog_symbol_t)(uintptr_t)value;
  core_string_t *owned = core_dict_string_key_copy(name);
  core_datalog_symbol_t id = (core_datalog_symbol_t)names->len;
  core_list_push(*names, owned);
  core_dict_put(index, owned, (uintptr_t)id);
  return id;
}

core_datalog_symbol_t core_datalog_constant(core_datalog_engine_t *engine,
                                            core_string_t name) {
  return engine && name.data
             ? intern_name(&engine->constants, &engine->constant_names, name)
             : CORE_DATALOG_SYMBOL_INVALID;
}

core_datalog_symbol_t core_datalog_variable(core_datalog_engine_t *engine,
                                            core_string_t name) {
  if (!engine || !name.data)
    return CORE_DATALOG_SYMBOL_INVALID;
  bool found = false;
  void *value = core_dict_get(&engine->variable_names, &name, &found);
  if (found)
    return (core_datalog_symbol_t)(uintptr_t)value;
  core_string_t *owned = core_dict_string_key_copy(name);
  core_datalog_symbol_t id = engine->next_variable--;
  core_list_push(engine->variables, owned);
  core_dict_put(&engine->variable_names, owned, (uintptr_t)id);
  return id;
}

core_datalog_symbol_t core_datalog_integer(core_datalog_engine_t *engine,
                                           int64_t value) {
  if (!engine)
    return CORE_DATALOG_SYMBOL_INVALID;
  int needed = snprintf(nullptr, 0, "%lld", (long long)value);
  char *text = core_alloc_array(char, (size_t)needed + 1);
  snprintf(text, (size_t)needed + 1, "%lld", (long long)value);
  core_datalog_symbol_t symbol =
      core_datalog_constant(engine, core_string_borrow(text, (size_t)needed));
  core_free(text);
  return symbol;
}

core_string_t core_datalog_symbol_name(const core_datalog_engine_t *engine,
                                       core_datalog_symbol_t symbol) {
  if (!engine)
    return (core_string_t){0};
  if (core_datalog_is_variable(symbol)) {
    size_t ix = (size_t)(CORE_DATALOG_VARIABLE_BASE - symbol);
    if (ix < engine->variables.len)
      return *engine->variables.data[ix];
    return (core_string_t){0};
  }
  if (symbol < 0 || (uint64_t)symbol >= engine->constants.len)
    return (core_string_t){0};
  return *engine->constants.data[symbol];
}

core_datalog_int_result_t
core_datalog_symbol_integer(const core_datalog_engine_t *engine,
                            core_datalog_symbol_t symbol) {
  core_string_t name = core_datalog_symbol_name(engine, symbol);
  if (!name.data || !name.u8_bytes)
    return core_result_err(core_datalog_int_result_t, EINVAL);
  int64_t value = 0;
  size_t consumed = 0;
  if (!core_string_parse_i64(name, 10, &value, &consumed) ||
      consumed != name.u8_bytes)
    return core_result_err(core_datalog_int_result_t, EINVAL);
  return core_result_ok(core_datalog_int_result_t, value);
}

static datalog_tuple_t *find_tuple(datalog_relation_t *relation,
                                   const core_datalog_symbol_t *arguments) {
  datalog_tuple_t probe = {
      .values = (core_datalog_symbol_t *)arguments,
      .arity = relation->arity,
  };
  bool found = false;
  void *existing = core_dict_get(&relation->tuple_set, &probe, &found);
  return found ? existing : nullptr;
}

static datalog_tuple_t *insert_tuple(datalog_relation_t *relation,
                                     const core_datalog_symbol_t *arguments) {
  if (find_tuple(relation, arguments))
    return nullptr;
  int32_t arity = relation->arity;
  datalog_tuple_t *tuple = core_alloc(datalog_tuple_t);
  *tuple = (datalog_tuple_t){
      .values =
          core_alloc_array(core_datalog_symbol_t, arity ? (size_t)arity : 1),
      .arity = arity,
      .rule = -1,
  };
  if (arity)
    memcpy(tuple->values, arguments, (size_t)arity * sizeof(*arguments));
  core_dict_put(&relation->tuple_set, tuple, tuple);
  core_list_push(relation->tuples, tuple);
  relation->indexes_dirty = true;
  return tuple;
}

bool core_datalog_add_fact(core_datalog_engine_t *engine,
                           core_datalog_relation_id_t id, int32_t arity,
                           const core_datalog_symbol_t *arguments) {
  if (!engine || id < 0 || (size_t)id >= engine->relations.len || arity < 0 ||
      (arity && !arguments))
    return false;
  datalog_relation_t *relation = engine->relations.data[id];
  if (arity != relation->arity)
    return false;
  for (int32_t i = 0; i < arity; i++)
    if (!valid_symbol(arguments[i]) || core_datalog_is_variable(arguments[i]))
      return false;
  if (!insert_tuple(relation, arguments))
    return false;
  engine->fact_count++;
  return true;
}

bool core_datalog_add_rule(core_datalog_engine_t *engine,
                           core_datalog_rule_t rule) {
  if (!engine || rule.head.relation < 0 ||
      (size_t)rule.head.relation >= engine->relations.len ||
      rule.head.arity != engine->relations.data[rule.head.relation]->arity ||
      (rule.head.arity && !rule.head.arguments) ||
      (rule.body_count && !rule.body))
    return false;
  for (size_t i = 0; i < rule.body_count; i++) {
    if (core_variant_is(rule.body[i], 2))
      continue;
    if (!core_variant_is(rule.body[i], 1))
      return false;
    core_datalog_literal_t literal = core_variant_get_1(rule.body[i]);
    if (literal.relation < 0 ||
        (size_t)literal.relation >= engine->relations.len ||
        literal.arity != engine->relations.data[literal.relation]->arity ||
        (literal.arity && !literal.arguments))
      return false;
    for (int32_t a = 0; a < literal.arity; a++)
      if (!valid_symbol(literal.arguments[a]))
        return false;
  }
  for (int32_t i = 0; i < rule.head.arity; i++)
    if (!valid_symbol(rule.head.arguments[i]))
      return false;
  core_list_push(engine->rules, rule);
  return true;
}

static bool stratify(core_datalog_engine_t *engine) {
  size_t n = engine->relations.len;
  for (size_t pass = 0; pass <= n; pass++) {
    bool changed = false;
    for (size_t r = 0; r < engine->rules.len; r++) {
      core_datalog_rule_t *rule = &engine->rules.data[r];
      int32_t required = 0;
      for (size_t b = 0; b < rule->body_count; b++) {
        core_datalog_goal_t goal = rule->body[b];
        if (!core_variant_is(goal, 1))
          continue;
        core_datalog_literal_t literal = core_variant_get_1(goal);
        int32_t level = engine->relations.data[literal.relation]->stratum;
        if (literal.negated)
          level++;
        if (level > required)
          required = level;
      }
      datalog_relation_t *head = engine->relations.data[rule->head.relation];
      if (head->stratum < required) {
        head->stratum = required;
        changed = true;
      }
    }
    if (!changed) {
      for (size_t r = 0; r < engine->rules.len; r++)
        engine->rules.data[r].stratum =
            engine->relations.data[engine->rules.data[r].head.relation]
                ->stratum;
      return true;
    }
    if (pass == n)
      return false;
  }
  return false;
}

static core_datalog_symbol_t binding_get(core_list_t(datalog_binding_t) *
                                             bindings,
                                         core_datalog_symbol_t variable) {
  for (size_t i = 0; i < bindings->len; i++)
    if (bindings->data[i].variable == variable)
      return bindings->data[i].value;
  return CORE_DATALOG_SYMBOL_INVALID;
}

static void binding_set(core_list_t(datalog_binding_t) * bindings,
                        core_datalog_symbol_t variable,
                        core_datalog_symbol_t value) {
  core_list_push(*bindings, ((datalog_binding_t){variable, value}));
}

static bool eval_integer(core_datalog_engine_t *engine,
                         core_datalog_expr_t *expression,
                         core_list_t(datalog_binding_t) * bindings,
                         int64_t *result) {
  int64_t left, right;
  if (!expression)
    return false;
  switch (expression->kind) {
  case CORE_DATALOG_EXPR_INTEGER:
    *result = expression->value.integer;
    return true;
  case CORE_DATALOG_EXPR_SYMBOL: {
    core_datalog_symbol_t symbol = expression->value.symbol;
    if (core_datalog_is_variable(symbol)) {
      symbol = binding_get(bindings, symbol);
      if (symbol == CORE_DATALOG_SYMBOL_INVALID)
        return false;
    }
    core_datalog_int_result_t converted =
        core_datalog_symbol_integer(engine, symbol);
    if (!converted.is_ok)
      return false;
    *result = converted.result.value;
    return true;
  }
  case CORE_DATALOG_EXPR_NEGATE:
    if (!eval_integer(engine, expression->value.operand, bindings, &left) ||
        left == INT64_MIN)
      return false;
    *result = -left;
    return true;
  default:
    if (!eval_integer(engine, expression->value.binary.left, bindings, &left) ||
        !eval_integer(engine, expression->value.binary.right, bindings, &right))
      return false;
    switch (expression->kind) {
    case CORE_DATALOG_EXPR_ADD:
      return !__builtin_add_overflow(left, right, result);
    case CORE_DATALOG_EXPR_SUBTRACT:
      return !__builtin_sub_overflow(left, right, result);
    case CORE_DATALOG_EXPR_MULTIPLY:
      return !__builtin_mul_overflow(left, right, result);
    case CORE_DATALOG_EXPR_DIVIDE:
      if (!right || (left == INT64_MIN && right == -1))
        return false;
      *result = left / right;
      return true;
    case CORE_DATALOG_EXPR_MODULO:
      if (!right || (left == INT64_MIN && right == -1))
        return false;
      *result = left % right;
      return true;
    default:
      return false;
    }
  }
}

static bool match_literal(const core_datalog_symbol_t *tuple,
                          const core_datalog_literal_t *literal,
                          core_list_t(datalog_binding_t) * bindings) {
  size_t mark = bindings->len;
  for (int32_t i = 0; i < literal->arity; i++) {
    core_datalog_symbol_t arg = literal->arguments[i];
    if (!core_datalog_is_variable(arg)) {
      if (arg != tuple[i]) {
        bindings->len = mark;
        return false;
      }
      continue;
    }
    core_datalog_symbol_t bound = binding_get(bindings, arg);
    if (bound != CORE_DATALOG_SYMBOL_INVALID) {
      if (bound != tuple[i]) {
        bindings->len = mark;
        return false;
      }
    } else
      binding_set(bindings, arg, tuple[i]);
  }
  return true;
}

static void relation_rebuild_indexes(datalog_relation_t *relation) {
  if (!relation->indexes_dirty)
    return;
  for (int32_t col = 0; col < relation->arity; col++) {
    core_dict_t *index = &relation->column_indexes[col];
    core_dict_foreach(index, entry) core_free(entry->value);
    core_dict_free(index);
    core_dict_init(index, core_hash_word, nullptr);
  }
  for (size_t offset = 0; offset < relation->tuples.len; offset++) {
    datalog_tuple_t *tuple = relation->tuples.data[offset];
    if (tuple->pending)
      continue;
    for (int32_t col = 0; col < relation->arity; col++) {
      void *key = (void *)(uintptr_t)tuple->values[col];
      bool found = false;
      datalog_offset_list_t *offsets =
          core_dict_get(&relation->column_indexes[col], key, &found);
      if (!found) {
        offsets = core_alloc(datalog_offset_list_t);
        *offsets = (datalog_offset_list_t){0};
        core_dict_put(&relation->column_indexes[col], key, (uintptr_t)offsets);
      }
      if (offsets->len == offsets->cap) {
        offsets->cap = offsets->cap ? offsets->cap * 2 : 4;
        offsets->data =
            core_realloc(offsets->data, offsets->cap * sizeof(*offsets->data));
      }
      offsets->data[offsets->len++] = offset;
    }
  }
  relation->indexes_dirty = false;
}

static void evaluate_body(core_datalog_engine_t *engine,
                          core_datalog_rule_t *rule, size_t body_index,
                          core_list_t(datalog_binding_t) * bindings,
                          bool has_recent, bool first_iteration,
                          datalog_relation_t *head);

static void derive_head(core_datalog_engine_t *engine,
                        core_datalog_rule_t *rule,
                        core_list_t(datalog_binding_t) * bindings,
                        bool has_recent, bool first_iteration,
                        datalog_relation_t *head) {
  if (!first_iteration && !has_recent)
    return;
  core_datalog_symbol_t *values = core_alloc_array(
      core_datalog_symbol_t, head->arity ? (size_t)head->arity : 1);
  for (int32_t i = 0; i < rule->head.arity; i++) {
    core_datalog_symbol_t value = rule->head.arguments[i];
    if (core_datalog_is_variable(value)) {
      value = binding_get(bindings, value);
      if (value == CORE_DATALOG_SYMBOL_INVALID) {
        core_free(values);
        return;
      }
    }
    values[i] = value;
  }
  datalog_tuple_t *tuple = insert_tuple(head, values);
  core_free(values);
  if (!tuple)
    return;
  tuple->pending = true;
  if (!engine->provenance)
    return;
  tuple->rule = (int32_t)(rule - engine->rules.data);
  tuple->premise_count = engine->trail.len;
  tuple->premises = core_alloc_array(datalog_premise_t,
                                     engine->trail.len ? engine->trail.len : 1);
  memcpy(tuple->premises, engine->trail.data,
         engine->trail.len * sizeof(*tuple->premises));
}

static bool eval_builtin(core_datalog_engine_t *engine,
                         core_datalog_builtin_t *builtin,
                         core_list_t(datalog_binding_t) * bindings,
                         bool *did_bind) {
  int64_t left, right;
  if (builtin->kind == CORE_DATALOG_BUILTIN_IS) {
    if (!builtin->left || builtin->left->kind != CORE_DATALOG_EXPR_SYMBOL ||
        !core_datalog_is_variable(builtin->left->value.symbol) ||
        !eval_integer(engine, builtin->right, bindings, &right))
      return false;
    core_datalog_symbol_t variable = builtin->left->value.symbol;
    core_datalog_symbol_t existing = binding_get(bindings, variable);
    core_datalog_symbol_t value = core_datalog_integer(engine, right);
    if (existing != CORE_DATALOG_SYMBOL_INVALID)
      return existing == value;
    binding_set(bindings, variable, value);
    *did_bind = true;
    return true;
  }
  if (!eval_integer(engine, builtin->left, bindings, &left) ||
      !eval_integer(engine, builtin->right, bindings, &right))
    return false;
  switch (builtin->kind) {
  case CORE_DATALOG_BUILTIN_LESS:
    return left < right;
  case CORE_DATALOG_BUILTIN_GREATER:
    return left > right;
  case CORE_DATALOG_BUILTIN_LESS_EQUAL:
    return left <= right;
  case CORE_DATALOG_BUILTIN_GREATER_EQUAL:
    return left >= right;
  case CORE_DATALOG_BUILTIN_EQUAL:
    return left == right;
  case CORE_DATALOG_BUILTIN_NOT_EQUAL:
    return left != right;
  default:
    return false;
  }
}

static void evaluate_body(core_datalog_engine_t *engine,
                          core_datalog_rule_t *rule, size_t body_index,
                          core_list_t(datalog_binding_t) * bindings,
                          bool has_recent, bool first_iteration,
                          datalog_relation_t *head) {
  if (body_index == rule->body_count) {
    derive_head(engine, rule, bindings, has_recent, first_iteration, head);
    return;
  }
  core_datalog_goal_t goal = rule->body[body_index];
  if (core_variant_is(goal, 2)) {
    core_datalog_builtin_t builtin = core_variant_get_2(goal);
    size_t mark = bindings->len;
    bool did_bind = false;
    if (eval_builtin(engine, &builtin, bindings, &did_bind))
      evaluate_body(engine, rule, body_index + 1, bindings, has_recent,
                    first_iteration, head);
    bindings->len = mark;
    return;
  }
  core_datalog_literal_t literal = core_variant_get_1(goal);
  datalog_relation_t *relation = engine->relations.data[literal.relation];
  if (literal.negated) {
    bool matched = false;
    core_datalog_symbol_t ground[literal.arity ? literal.arity : 1];
    bool bound = true;
    for (int32_t a = 0; bound && a < literal.arity; a++) {
      ground[a] = core_datalog_is_variable(literal.arguments[a])
                      ? binding_get(bindings, literal.arguments[a])
                      : literal.arguments[a];
      bound = ground[a] != CORE_DATALOG_SYMBOL_INVALID;
    }
    if (bound) {
      datalog_tuple_t *tuple = find_tuple(relation, ground);
      if (!tuple || tuple->pending)
        evaluate_body(engine, rule, body_index + 1, bindings, has_recent,
                      first_iteration, head);
      return;
    }
    for (size_t i = 0; i < relation->tuples.len; i++) {
      datalog_tuple_t *tuple = relation->tuples.data[i];
      if (tuple->pending)
        continue;
      size_t mark = bindings->len;
      if (match_literal(tuple->values, &literal, bindings))
        matched = true;
      bindings->len = mark;
      if (matched)
        break;
    }
    if (!matched)
      evaluate_body(engine, rule, body_index + 1, bindings, has_recent,
                    first_iteration, head);
    return;
  }
  int32_t bound_col = -1;
  core_datalog_symbol_t bound_value = CORE_DATALOG_SYMBOL_INVALID;
  for (int32_t i = 0; i < literal.arity; i++) {
    core_datalog_symbol_t arg = literal.arguments[i];
    if (core_datalog_is_variable(arg)) {
      arg = binding_get(bindings, arg);
      if (arg == CORE_DATALOG_SYMBOL_INVALID)
        continue;
    }
    bound_col = i;
    bound_value = arg;
    break;
  }
  datalog_offset_list_t *candidates = nullptr;
  if (bound_col >= 0) {
    relation_rebuild_indexes(relation);
    bool found = false;
    candidates = core_dict_get(&relation->column_indexes[bound_col],
                               (void *)(uintptr_t)bound_value, &found);
    if (!found)
      return;
  }
  size_t candidate_count = candidates ? candidates->len : relation->tuples.len;
  for (size_t i = 0; i < candidate_count; i++) {
    size_t offset = candidates ? candidates->data[i] : i;
    datalog_tuple_t *tuple = relation->tuples.data[offset];
    if (tuple->pending)
      continue;
    size_t mark = bindings->len;
    if (match_literal(tuple->values, &literal, bindings)) {
      if (engine->provenance)
        core_list_push(engine->trail,
                       ((datalog_premise_t){literal.relation, tuple}));
      evaluate_body(engine, rule, body_index + 1, bindings,
                    has_recent || tuple->recent, first_iteration, head);
      if (engine->provenance)
        engine->trail.len--;
    }
    bindings->len = mark;
  }
}

bool core_datalog_run(core_datalog_engine_t *engine) {
  if (!engine || !stratify(engine))
    return false;
  engine->iterations = 0;
  for (size_t i = 0; i < engine->relations.len; i++)
    for (size_t t = 0; t < engine->relations.data[i]->tuples.len; t++)
      engine->relations.data[i]->tuples.data[t]->recent = true;
  int32_t max_stratum = 0;
  for (size_t i = 0; i < engine->relations.len; i++)
    if (engine->relations.data[i]->stratum > max_stratum)
      max_stratum = engine->relations.data[i]->stratum;
  for (int32_t stratum = 0; stratum <= max_stratum; stratum++) {
    bool first_iteration = true;
    for (;;) {
      engine->iterations++;
      for (size_t r = 0; r < engine->rules.len; r++) {
        core_datalog_rule_t *rule = &engine->rules.data[r];
        if (rule->stratum != stratum)
          continue;
        core_list_t(datalog_binding_t) bindings =
            core_list_new(datalog_binding_t);
        datalog_relation_t *head = engine->relations.data[rule->head.relation];
        evaluate_body(engine, rule, 0, &bindings, false, first_iteration, head);
        core_list_free(bindings);
      }
      first_iteration = false;
      bool changed = false;
      for (size_t r = 0; r < engine->relations.len; r++) {
        datalog_relation_t *relation = engine->relations.data[r];
        bool promoted = false;
        for (size_t t = 0; t < relation->tuples.len; t++) {
          datalog_tuple_t *tuple = relation->tuples.data[t];
          tuple->recent = tuple->pending;
          if (tuple->pending) {
            changed = true;
            promoted = true;
          }
          tuple->pending = false;
        }
        if (promoted)
          relation->indexes_dirty = true;
      }
      if (!changed)
        break;
    }
  }
  engine->fact_count = 0;
  for (size_t r = 0; r < engine->relations.len; r++)
    engine->fact_count += (int64_t)engine->relations.data[r]->tuples.len;
  return true;
}

void core_datalog_set_provenance(core_datalog_engine_t *engine, bool enabled) {
  if (engine)
    engine->provenance = enabled;
}

bool core_datalog_contains(const core_datalog_engine_t *engine,
                           core_datalog_relation_id_t relation,
                           const core_datalog_symbol_t *arguments) {
  if (!engine || relation < 0 || (size_t)relation >= engine->relations.len)
    return false;
  datalog_tuple_t *tuple =
      find_tuple(engine->relations.data[relation], arguments);
  return tuple && !tuple->pending;
}

static bool explain_tuple(const core_datalog_engine_t *engine,
                          core_datalog_relation_id_t relation,
                          const datalog_tuple_t *tuple, int32_t depth,
                          core_datalog_explain_fn visit, void *context) {
  if (!visit(relation, tuple->values, tuple->arity, tuple->rule, depth,
             context))
    return false;
  for (size_t p = 0; p < tuple->premise_count; p++)
    if (!explain_tuple(engine, tuple->premises[p].relation,
                       tuple->premises[p].tuple, depth + 1, visit, context))
      return false;
  return true;
}

bool core_datalog_explain(const core_datalog_engine_t *engine,
                          core_datalog_relation_id_t relation,
                          const core_datalog_symbol_t *arguments,
                          core_datalog_explain_fn visit, void *context) {
  if (!engine || !visit || relation < 0 ||
      (size_t)relation >= engine->relations.len)
    return false;
  datalog_tuple_t *tuple =
      find_tuple(engine->relations.data[relation], arguments);
  if (!tuple || tuple->pending)
    return false;
  explain_tuple(engine, relation, tuple, 0, visit, context);
  return true;
}

void core_datalog_query(const core_datalog_engine_t *engine,
                        core_datalog_relation_id_t id,
                        core_datalog_query_fn callback, void *context) {
  if (!engine || !callback || id < 0 || (size_t)id >= engine->relations.len)
    return;
  datalog_relation_t *relation = engine->relations.data[id];
  for (size_t i = 0; i < relation->tuples.len; i++)
    if (!callback(relation->tuples.data[i]->values, relation->arity, context))
      return;
}

size_t core_datalog_count(core_datalog_engine_t *engine,
                          core_datalog_relation_id_t id) {
  if (!engine || id < 0 || (size_t)id >= engine->relations.len)
    return 0;
  return engine->relations.data[id]->tuples.len;
}

size_t core_datalog_count_by_name(core_datalog_engine_t *engine,
                                  core_string_t name) {
  core_option_t(core_datalog_relation_id_t) relation =
      core_datalog_find_relation(engine, name);
  return relation.has_value ? core_datalog_count(engine, relation.value) : 0;
}

int32_t core_datalog_relation_count(const core_datalog_engine_t *engine) {
  return engine ? (int32_t)engine->relations.len : 0;
}

int64_t core_datalog_iterations(const core_datalog_engine_t *engine) {
  return engine ? engine->iterations : 0;
}

int64_t core_datalog_total_facts(const core_datalog_engine_t *engine) {
  return engine ? engine->fact_count : 0;
}

core_datalog_expr_t *core_datalog_expr_symbol(core_datalog_symbol_t symbol) {
  core_datalog_expr_t *expression = core_alloc(core_datalog_expr_t);
  *expression = (core_datalog_expr_t){.kind = CORE_DATALOG_EXPR_SYMBOL,
                                      .value.symbol = symbol};
  return expression;
}

core_datalog_expr_t *core_datalog_expr_integer(int64_t value) {
  core_datalog_expr_t *expression = core_alloc(core_datalog_expr_t);
  *expression = (core_datalog_expr_t){.kind = CORE_DATALOG_EXPR_INTEGER,
                                      .value.integer = value};
  return expression;
}

core_datalog_expr_t *core_datalog_expr_binary(core_datalog_expr_kind_t kind,
                                              core_datalog_expr_t *left,
                                              core_datalog_expr_t *right) {
  core_datalog_expr_t *expression = core_alloc(core_datalog_expr_t);
  *expression = (core_datalog_expr_t){
      .kind = kind, .value.binary = {.left = left, .right = right}};
  return expression;
}

core_datalog_expr_t *core_datalog_expr_negate(core_datalog_expr_t *operand) {
  core_datalog_expr_t *expression = core_alloc(core_datalog_expr_t);
  *expression = (core_datalog_expr_t){.kind = CORE_DATALOG_EXPR_NEGATE,
                                      .value.operand = operand};
  return expression;
}

void core_datalog_expr_free(core_datalog_expr_t *expression) {
  if (!expression)
    return;
  switch (expression->kind) {
  case CORE_DATALOG_EXPR_ADD:
  case CORE_DATALOG_EXPR_SUBTRACT:
  case CORE_DATALOG_EXPR_MULTIPLY:
  case CORE_DATALOG_EXPR_DIVIDE:
  case CORE_DATALOG_EXPR_MODULO:
    core_datalog_expr_free(expression->value.binary.left);
    core_datalog_expr_free(expression->value.binary.right);
    break;
  case CORE_DATALOG_EXPR_NEGATE:
    core_datalog_expr_free(expression->value.operand);
    break;
  default:
    break;
  }
  core_free(expression);
}
