#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/option.h"
#include "lib/result.h"
#include "lib/string.h"
#include "lib/variant.h"

typedef int64_t core_datalog_symbol_t;
typedef int32_t core_datalog_relation_id_t;

#define CORE_DATALOG_SYMBOL_INVALID ((core_datalog_symbol_t) - 1)
#define CORE_DATALOG_VARIABLE_BASE ((core_datalog_symbol_t)(INT64_MIN / 2))

static inline bool core_datalog_is_variable(core_datalog_symbol_t symbol) {
  return symbol <= CORE_DATALOG_VARIABLE_BASE;
}

typedef enum {
  CORE_DATALOG_EXPR_SYMBOL,
  CORE_DATALOG_EXPR_INTEGER,
  CORE_DATALOG_EXPR_ADD,
  CORE_DATALOG_EXPR_SUBTRACT,
  CORE_DATALOG_EXPR_MULTIPLY,
  CORE_DATALOG_EXPR_DIVIDE,
  CORE_DATALOG_EXPR_MODULO,
  CORE_DATALOG_EXPR_NEGATE,
} core_datalog_expr_kind_t;

typedef struct core_datalog_expr core_datalog_expr_t;
struct core_datalog_expr {
  core_datalog_expr_kind_t kind;
  union {
    core_datalog_symbol_t symbol;
    int64_t integer;
    struct {
      core_datalog_expr_t *left;
      core_datalog_expr_t *right;
    } binary;
    core_datalog_expr_t *operand;
  } value;
};

typedef enum {
  CORE_DATALOG_BUILTIN_IS,
  CORE_DATALOG_BUILTIN_LESS,
  CORE_DATALOG_BUILTIN_GREATER,
  CORE_DATALOG_BUILTIN_LESS_EQUAL,
  CORE_DATALOG_BUILTIN_GREATER_EQUAL,
  CORE_DATALOG_BUILTIN_EQUAL,
  CORE_DATALOG_BUILTIN_NOT_EQUAL,
} core_datalog_builtin_kind_t;

typedef struct {
  core_datalog_builtin_kind_t kind;
  core_datalog_expr_t *left;
  core_datalog_expr_t *right;
} core_datalog_builtin_t;

typedef struct {
  core_datalog_relation_id_t relation;
  int32_t arity;
  core_datalog_symbol_t *arguments;
  bool negated;
} core_datalog_literal_t;

core_variant_decl(core_datalog_goal_t, core_datalog_literal_t,
                  core_datalog_builtin_t);

typedef struct {
  core_datalog_literal_t head;
  core_datalog_goal_t *body;
  size_t body_count;
  int32_t stratum;
} core_datalog_rule_t;

typedef struct core_datalog_rule_builder core_datalog_rule_builder_t;
core_datalog_rule_builder_t *core_datalog_rule_builder_new(void);
bool core_datalog_rule_builder_head(core_datalog_rule_builder_t *builder,
                                    core_datalog_relation_id_t relation,
                                    int32_t arity,
                                    const core_datalog_symbol_t *arguments);
bool core_datalog_rule_builder_add_literal(
    core_datalog_rule_builder_t *builder, core_datalog_relation_id_t relation,
    int32_t arity, const core_datalog_symbol_t *arguments, bool negated);
bool core_datalog_rule_builder_add_builtin(core_datalog_rule_builder_t *builder,
                                           core_datalog_builtin_t builtin);
core_datalog_rule_t
core_datalog_rule_builder_finish(core_datalog_rule_builder_t *builder);
void core_datalog_rule_builder_free(core_datalog_rule_builder_t *builder);

core_result_decl(core_datalog_int_result_t, int64_t, int);
core_option_decl(core_datalog_relation_id_t);

typedef struct core_datalog_engine core_datalog_engine_t;
typedef bool (*core_datalog_query_fn)(const core_datalog_symbol_t *tuple,
                                      int32_t arity, void *context);

core_datalog_engine_t *core_datalog_engine_new(void);
void core_datalog_engine_free(core_datalog_engine_t *engine);

core_datalog_relation_id_t core_datalog_relation(core_datalog_engine_t *engine,
                                                 core_string_t name,
                                                 int32_t arity);
core_option_t(core_datalog_relation_id_t)
    core_datalog_find_relation(core_datalog_engine_t *engine,
                               core_string_t name);
core_string_t core_datalog_relation_name(core_datalog_engine_t *engine,
                                         core_datalog_relation_id_t relation);
core_option_t(int32_t)
    core_datalog_relation_arity(core_datalog_engine_t *engine,
                                core_datalog_relation_id_t relation);

core_datalog_symbol_t core_datalog_constant(core_datalog_engine_t *engine,
                                            core_string_t name);
core_datalog_symbol_t core_datalog_integer(core_datalog_engine_t *engine,
                                           int64_t value);
core_datalog_symbol_t core_datalog_variable(core_datalog_engine_t *engine,
                                            core_string_t name);
core_string_t core_datalog_symbol_name(const core_datalog_engine_t *engine,
                                       core_datalog_symbol_t symbol);
core_datalog_int_result_t
core_datalog_symbol_integer(const core_datalog_engine_t *engine,
                            core_datalog_symbol_t symbol);

bool core_datalog_add_fact(core_datalog_engine_t *engine,
                           core_datalog_relation_id_t relation, int32_t arity,
                           const core_datalog_symbol_t *arguments);
bool core_datalog_add_rule(core_datalog_engine_t *engine,
                           core_datalog_rule_t rule);
bool core_datalog_run(core_datalog_engine_t *engine);

typedef bool (*core_datalog_explain_fn)(core_datalog_relation_id_t relation,
                                        const core_datalog_symbol_t *tuple,
                                        int32_t arity, int32_t rule,
                                        int32_t depth, void *context);
void core_datalog_set_provenance(core_datalog_engine_t *engine, bool enabled);
bool core_datalog_contains(const core_datalog_engine_t *engine,
                           core_datalog_relation_id_t relation,
                           const core_datalog_symbol_t *arguments);
bool core_datalog_explain(const core_datalog_engine_t *engine,
                          core_datalog_relation_id_t relation,
                          const core_datalog_symbol_t *arguments,
                          core_datalog_explain_fn visit, void *context);
void core_datalog_query(const core_datalog_engine_t *engine,
                        core_datalog_relation_id_t relation,
                        core_datalog_query_fn callback, void *context);
size_t core_datalog_count(core_datalog_engine_t *engine,
                          core_datalog_relation_id_t relation);
size_t core_datalog_count_by_name(core_datalog_engine_t *engine,
                                  core_string_t relation_name);
int32_t core_datalog_relation_count(const core_datalog_engine_t *engine);
int64_t core_datalog_iterations(const core_datalog_engine_t *engine);
int64_t core_datalog_total_facts(const core_datalog_engine_t *engine);

core_datalog_expr_t *core_datalog_expr_symbol(core_datalog_symbol_t symbol);
core_datalog_expr_t *core_datalog_expr_integer(int64_t value);
core_datalog_expr_t *core_datalog_expr_binary(core_datalog_expr_kind_t kind,
                                              core_datalog_expr_t *left,
                                              core_datalog_expr_t *right);
core_datalog_expr_t *core_datalog_expr_negate(core_datalog_expr_t *operand);
void core_datalog_expr_free(core_datalog_expr_t *expression);
void core_datalog_rule_free(core_datalog_rule_t *rule);
