#include <stdio.h>
#include <string.h>

#include "harness.h"
#include "logic/datalog.h"

static bool count_row(const core_datalog_symbol_t *tuple, int32_t arity,
                      void *context) {
  (void)tuple;
  if (arity != 2)
    return false;
  (*(size_t *)context)++;
  return true;
}

static core_datalog_literal_t literal(core_datalog_relation_id_t relation,
                                      int32_t arity,
                                      const core_datalog_symbol_t *arguments,
                                      bool negated) {
  core_datalog_literal_t result = {
      .relation = relation,
      .arity = arity,
      .arguments =
          core_alloc_array(core_datalog_symbol_t, arity ? (size_t)arity : 1),
      .negated = negated,
  };
  if (arity)
    memcpy(result.arguments, arguments, (size_t)arity * sizeof(*arguments));
  return result;
}

static void test_transitive_closure_and_wide_bindings(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t edge =
      core_datalog_relation(engine, CORE_STRING_STATIC("edge"), 2);
  core_datalog_relation_id_t path =
      core_datalog_relation(engine, CORE_STRING_STATIC("path"), 2);
  core_datalog_symbol_t a =
      core_datalog_constant(engine, CORE_STRING_STATIC("a"));
  core_datalog_symbol_t b =
      core_datalog_constant(engine, CORE_STRING_STATIC("b"));
  core_datalog_symbol_t c =
      core_datalog_constant(engine, CORE_STRING_STATIC("c"));
  core_datalog_symbol_t d =
      core_datalog_constant(engine, CORE_STRING_STATIC("d"));
  core_datalog_symbol_t e1[] = {a, b};
  core_datalog_symbol_t e2[] = {b, c};
  core_datalog_symbol_t e3[] = {c, d};
  core_datalog_add_fact(engine, edge, 2, e1);
  core_datalog_add_fact(engine, edge, 2, e2);
  core_datalog_add_fact(engine, edge, 2, e3);

  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t y =
      core_datalog_variable(engine, CORE_STRING_STATIC("Y"));
  core_datalog_symbol_t z =
      core_datalog_variable(engine, CORE_STRING_STATIC("Z"));
  core_datalog_symbol_t head1[] = {x, y};
  core_datalog_symbol_t body1[] = {x, y};
  core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, path, 2, head1);
  core_datalog_rule_builder_add_literal(builder, edge, 2, body1, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));

  core_datalog_symbol_t head2[] = {x, z};
  core_datalog_symbol_t body2a[] = {x, y};
  core_datalog_symbol_t body2b[] = {y, z};
  builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, path, 2, head2);
  core_datalog_rule_builder_add_literal(builder, path, 2, body2a, false);
  core_datalog_rule_builder_add_literal(builder, edge, 2, body2b, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));

  check("recursive Datalog closure evaluates", core_datalog_run(engine));
  check("recursive Datalog closure derives six paths",
        core_datalog_count(engine, path) == 6);
  size_t queried = 0;
  core_datalog_query(engine, path, count_row, &queried);
  check("tuple queries visit every derived row", queried == 6);
  check("fixpoint requires multiple iterations",
        core_datalog_iterations(engine) >= 2);
  core_datalog_engine_free(engine);
}

static void test_ancestor_derivation(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t parent =
      core_datalog_relation(engine, CORE_STRING_STATIC("parent"), 2);
  core_datalog_relation_id_t ancestor =
      core_datalog_relation(engine, CORE_STRING_STATIC("ancestor"), 2);
  core_datalog_symbol_t tom =
      core_datalog_constant(engine, CORE_STRING_STATIC("tom"));
  core_datalog_symbol_t bob =
      core_datalog_constant(engine, CORE_STRING_STATIC("bob"));
  core_datalog_symbol_t ann =
      core_datalog_constant(engine, CORE_STRING_STATIC("ann"));
  core_datalog_symbol_t sue =
      core_datalog_constant(engine, CORE_STRING_STATIC("sue"));
  core_datalog_symbol_t p1[] = {tom, bob}, p2[] = {bob, ann}, p3[] = {ann, sue};
  core_datalog_add_fact(engine, parent, 2, p1);
  core_datalog_add_fact(engine, parent, 2, p2);
  core_datalog_add_fact(engine, parent, 2, p3);

  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t y =
      core_datalog_variable(engine, CORE_STRING_STATIC("Y"));
  core_datalog_symbol_t z =
      core_datalog_variable(engine, CORE_STRING_STATIC("Z"));
  core_datalog_symbol_t head1[] = {x, y};
  core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, ancestor, 2, head1);
  core_datalog_rule_builder_add_literal(builder, parent, 2, head1, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));

  core_datalog_symbol_t head2[] = {x, y}, body1[] = {x, z}, body2[] = {z, y};
  builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, ancestor, 2, head2);
  core_datalog_rule_builder_add_literal(builder, parent, 2, body1, false);
  core_datalog_rule_builder_add_literal(builder, ancestor, 2, body2, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));

  check("ancestor rules evaluate", core_datalog_run(engine));
  size_t ancestor_count = core_datalog_count(engine, ancestor);
  check("ancestor closure derives all six pairs", ancestor_count == 6);
  core_datalog_engine_free(engine);
}

static void test_join_and_duplicate_suppression(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t parent =
      core_datalog_relation(engine, CORE_STRING_STATIC("parent"), 2);
  core_datalog_relation_id_t sibling =
      core_datalog_relation(engine, CORE_STRING_STATIC("sibling"), 2);
  core_datalog_symbol_t a =
      core_datalog_constant(engine, CORE_STRING_STATIC("a"));
  core_datalog_symbol_t b =
      core_datalog_constant(engine, CORE_STRING_STATIC("b"));
  core_datalog_symbol_t c =
      core_datalog_constant(engine, CORE_STRING_STATIC("c"));
  core_datalog_symbol_t d =
      core_datalog_constant(engine, CORE_STRING_STATIC("d"));
  core_datalog_symbol_t facts[][2] = {{a, b}, {a, c}, {b, d}, {c, d}};
  for (size_t i = 0; i < sizeof(facts) / sizeof(facts[0]); i++)
    core_datalog_add_fact(engine, parent, 2, facts[i]);

  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t y =
      core_datalog_variable(engine, CORE_STRING_STATIC("Y"));
  core_datalog_symbol_t z =
      core_datalog_variable(engine, CORE_STRING_STATIC("Z"));
  core_datalog_symbol_t head[] = {x, y}, left[] = {z, x}, right[] = {z, y};
  core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, sibling, 2, head);
  core_datalog_rule_builder_add_literal(builder, parent, 2, left, false);
  core_datalog_rule_builder_add_literal(builder, parent, 2, right, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));

  check("multiway join evaluates", core_datalog_run(engine));
  check("joins deduplicate repeated derived tuples",
        core_datalog_count(engine, sibling) == 5);
  core_datalog_engine_free(engine);
}

static void test_unstratified_negation_is_rejected(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t p =
      core_datalog_relation(engine, CORE_STRING_STATIC("p"), 1);
  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t args[] = {x};
  core_datalog_goal_t *body = core_alloc_array(core_datalog_goal_t, 1);
  body[0] = core_variant_make_1(core_datalog_goal_t, literal(p, 1, args, true));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(p, 1, args, false),
                                    .body = body,
                                    .body_count = 1,
                                });
  check("recursive negation is rejected", !core_datalog_run(engine));
  core_datalog_engine_free(engine);
}

static void test_stratified_negation_and_arithmetic_builtin(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t node =
      core_datalog_relation(engine, CORE_STRING_STATIC("node"), 1);
  core_datalog_relation_id_t blocked =
      core_datalog_relation(engine, CORE_STRING_STATIC("blocked"), 1);
  core_datalog_relation_id_t allowed =
      core_datalog_relation(engine, CORE_STRING_STATIC("allowed"), 1);
  core_datalog_relation_id_t number =
      core_datalog_relation(engine, CORE_STRING_STATIC("number"), 1);
  core_datalog_relation_id_t doubled =
      core_datalog_relation(engine, CORE_STRING_STATIC("doubled"), 2);
  core_datalog_symbol_t n =
      core_datalog_constant(engine, CORE_STRING_STATIC("n"));
  core_datalog_symbol_t fact[] = {n};
  core_datalog_add_fact(engine, node, 1, fact);
  core_datalog_symbol_t five = core_datalog_integer(engine, 5);
  core_datalog_symbol_t number_fact[] = {five};
  core_datalog_add_fact(engine, number, 1, number_fact);

  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t one_arg[] = {x};
  core_datalog_goal_t *not_blocked = core_alloc_array(core_datalog_goal_t, 2);
  not_blocked[0] = core_variant_make_1(core_datalog_goal_t,
                                       literal(node, 1, one_arg, false));
  not_blocked[1] = core_variant_make_1(core_datalog_goal_t,
                                       literal(blocked, 1, one_arg, true));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(allowed, 1, one_arg, false),
                                    .body = not_blocked,
                                    .body_count = 2,
                                });

  core_datalog_symbol_t y =
      core_datalog_variable(engine, CORE_STRING_STATIC("Y"));
  core_datalog_symbol_t num_arg[] = {x};
  core_datalog_symbol_t out_args[] = {x, y};
  core_datalog_goal_t *calc = core_alloc_array(core_datalog_goal_t, 2);
  calc[0] = core_variant_make_1(core_datalog_goal_t,
                                literal(number, 1, num_arg, false));
  calc[1] = core_variant_make_2(
      core_datalog_goal_t,
      ((core_datalog_builtin_t){
          .kind = CORE_DATALOG_BUILTIN_IS,
          .left = core_datalog_expr_symbol(y),
          .right = core_datalog_expr_binary(CORE_DATALOG_EXPR_MULTIPLY,
                                            core_datalog_expr_symbol(x),
                                            core_datalog_expr_integer(2)),
      }));
  core_datalog_add_rule(engine,
                        (core_datalog_rule_t){
                            .head = literal(doubled, 2, out_args, false),
                            .body = calc,
                            .body_count = 2,
                        });

  check("stratified negation and arithmetic evaluate",
        core_datalog_run(engine));
  check("stratified negation derives allowed node",
        core_datalog_count(engine, allowed) == 1);
  check("arithmetic assignment derives one tuple",
        core_datalog_count(engine, doubled) == 1);
  core_datalog_engine_free(engine);
}

static void test_multiple_negation_strata(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t base =
      core_datalog_relation(engine, CORE_STRING_STATIC("base"), 1);
  core_datalog_relation_id_t derived =
      core_datalog_relation(engine, CORE_STRING_STATIC("derived"), 1);
  core_datalog_relation_id_t excluded =
      core_datalog_relation(engine, CORE_STRING_STATIC("excluded"), 1);
  core_datalog_relation_id_t final =
      core_datalog_relation(engine, CORE_STRING_STATIC("final"), 1);
  for (int64_t i = 1; i <= 3; i++) {
    core_datalog_symbol_t value = core_datalog_integer(engine, i);
    core_datalog_add_fact(engine, base, 1, &value);
  }
  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t args[] = {x};

  core_datalog_goal_t *derive_body = core_alloc_array(core_datalog_goal_t, 1);
  derive_body[0] =
      core_variant_make_1(core_datalog_goal_t, literal(base, 1, args, false));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(derived, 1, args, false),
                                    .body = derive_body,
                                    .body_count = 1,
                                });

  core_datalog_goal_t *exclude_body = core_alloc_array(core_datalog_goal_t, 2);
  exclude_body[0] =
      core_variant_make_1(core_datalog_goal_t, literal(base, 1, args, false));
  exclude_body[1] =
      core_variant_make_1(core_datalog_goal_t, literal(derived, 1, args, true));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(excluded, 1, args, false),
                                    .body = exclude_body,
                                    .body_count = 2,
                                });

  core_datalog_goal_t *final_body = core_alloc_array(core_datalog_goal_t, 2);
  final_body[0] = core_variant_make_1(core_datalog_goal_t,
                                      literal(derived, 1, args, false));
  final_body[1] = core_variant_make_1(core_datalog_goal_t,
                                      literal(excluded, 1, args, true));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(final, 1, args, false),
                                    .body = final_body,
                                    .body_count = 2,
                                });

  check("multi-stratum program is stratified", core_datalog_run(engine));
  check("first stratum derives all values",
        core_datalog_count(engine, derived) == 3);
  check("second stratum sees completed derived relation",
        core_datalog_count(engine, excluded) == 0);
  check("third stratum sees completed exclusions",
        core_datalog_count(engine, final) == 3);
  core_datalog_engine_free(engine);
}

static void test_rule_bindings_are_not_capped_at_64(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t source =
      core_datalog_relation(engine, CORE_STRING_STATIC("source"), 70);
  core_datalog_relation_id_t copy =
      core_datalog_relation(engine, CORE_STRING_STATIC("copy"), 70);
  core_datalog_symbol_t values[70];
  core_datalog_symbol_t variables[70];
  for (size_t i = 0; i < 70; i++) {
    values[i] = core_datalog_integer(engine, (int64_t)i);
    char name[24];
    int length = snprintf(name, sizeof(name), "V%zu", i);
    variables[i] = core_datalog_variable(
        engine, core_string_borrow(name, (size_t)length));
  }
  core_datalog_add_fact(engine, source, 70, values);
  core_datalog_goal_t *body = core_alloc_array(core_datalog_goal_t, 1);
  body[0] = core_variant_make_1(core_datalog_goal_t,
                                literal(source, 70, variables, false));
  core_datalog_add_rule(engine, (core_datalog_rule_t){
                                    .head = literal(copy, 70, variables, false),
                                    .body = body,
                                    .body_count = 1,
                                });
  check("rules with 70 distinct variables evaluate", core_datalog_run(engine));
  check("wide rule preserves its full tuple",
        core_datalog_count(engine, copy) == 1);
  core_datalog_engine_free(engine);
}

static void test_symbol_and_relation_introspection(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_relation_id_t items =
      core_datalog_relation(engine, CORE_STRING_STATIC("items"), 1);
  core_datalog_symbol_t atom =
      core_datalog_constant(engine, CORE_STRING_STATIC("item"));
  core_datalog_symbol_t same_atom =
      core_datalog_constant(engine, CORE_STRING_STATIC("item"));
  core_datalog_symbol_t variable =
      core_datalog_variable(engine, CORE_STRING_STATIC("item"));
  core_datalog_symbol_t integer = core_datalog_integer(engine, -42);
  core_datalog_symbol_t hello =
      core_datalog_constant(engine, CORE_STRING_STATIC("hello"));
  core_datalog_symbol_t positive = core_datalog_integer(engine, 42);
  core_datalog_symbol_t named_variable =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t fact[] = {atom};
  core_datalog_add_fact(engine, items, 1, fact);
  check("duplicate facts are ignored",
        !core_datalog_add_fact(engine, items, 1, fact));
  check("fact-only engines run", core_datalog_run(engine));

  core_option_t(core_datalog_relation_id_t) found =
      core_datalog_find_relation(engine, CORE_STRING_STATIC("items"));
  core_option_t(int32_t) arity = core_datalog_relation_arity(engine, items);
  core_datalog_int_result_t decoded =
      core_datalog_symbol_integer(engine, integer);
  check("relation lookup and arity introspection work",
        found.has_value && found.value == items && arity.has_value &&
            arity.value == 1 &&
            core_string_eq(core_datalog_relation_name(engine, items),
                           CORE_STRING_STATIC("items")));
  check("Datalog constant and variable namespaces are independent",
        atom != variable && atom == same_atom &&
            core_string_eq(core_datalog_symbol_name(engine, variable),
                           CORE_STRING_STATIC("item")));
  check("integer symbols round-trip",
        decoded.is_ok && decoded.result.value == -42);
  check("constant, integer, and variable symbols retain their spellings",
        core_string_eq(core_datalog_symbol_name(engine, hello),
                       CORE_STRING_STATIC("hello")) &&
            core_string_eq(core_datalog_symbol_name(engine, positive),
                           CORE_STRING_STATIC("42")) &&
            core_string_eq(core_datalog_symbol_name(engine, named_variable),
                           CORE_STRING_STATIC("X")));
  check("count-by-name returns relation cardinality",
        core_datalog_count_by_name(engine, CORE_STRING_STATIC("items")) == 1 &&
            core_datalog_count_by_name(engine, CORE_STRING_STATIC("missing")) ==
                0 &&
            core_datalog_relation_count(engine) == 1);
  core_datalog_engine_free(engine);
}

typedef struct {
  int32_t depths[8];
  int32_t rules[8];
  core_datalog_relation_id_t relations[8];
  size_t count;
} explanation_t;

static bool record_step(core_datalog_relation_id_t relation,
                        const core_datalog_symbol_t *tuple, int32_t arity,
                        int32_t rule, int32_t depth, void *context) {
  (void)tuple;
  (void)arity;
  explanation_t *explanation = context;
  if (explanation->count == 8)
    return false;
  explanation->relations[explanation->count] = relation;
  explanation->rules[explanation->count] = rule;
  explanation->depths[explanation->count++] = depth;
  return true;
}

static void test_provenance_reports_derivation_chain(void) {
  core_datalog_engine_t *engine = core_datalog_engine_new();
  core_datalog_set_provenance(engine, true);
  core_datalog_relation_id_t edge =
      core_datalog_relation(engine, CORE_STRING_STATIC("edge"), 2);
  core_datalog_relation_id_t path =
      core_datalog_relation(engine, CORE_STRING_STATIC("path"), 2);
  core_datalog_symbol_t a =
      core_datalog_constant(engine, CORE_STRING_STATIC("a"));
  core_datalog_symbol_t b =
      core_datalog_constant(engine, CORE_STRING_STATIC("b"));
  core_datalog_symbol_t c =
      core_datalog_constant(engine, CORE_STRING_STATIC("c"));
  core_datalog_symbol_t e1[] = {a, b}, e2[] = {b, c};
  core_datalog_add_fact(engine, edge, 2, e1);
  core_datalog_add_fact(engine, edge, 2, e2);
  core_datalog_symbol_t x =
      core_datalog_variable(engine, CORE_STRING_STATIC("X"));
  core_datalog_symbol_t y =
      core_datalog_variable(engine, CORE_STRING_STATIC("Y"));
  core_datalog_symbol_t z =
      core_datalog_variable(engine, CORE_STRING_STATIC("Z"));
  core_datalog_symbol_t xy[] = {x, y}, xz[] = {x, z}, yz[] = {y, z};
  core_datalog_rule_builder_t *builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, path, 2, xy);
  core_datalog_rule_builder_add_literal(builder, edge, 2, xy, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));
  builder = core_datalog_rule_builder_new();
  core_datalog_rule_builder_head(builder, path, 2, xz);
  core_datalog_rule_builder_add_literal(builder, edge, 2, xy, false);
  core_datalog_rule_builder_add_literal(builder, path, 2, yz, false);
  core_datalog_add_rule(engine, core_datalog_rule_builder_finish(builder));
  check("provenance engine runs", core_datalog_run(engine));

  core_datalog_symbol_t ac[] = {a, c};
  explanation_t explanation = {0};
  check("a derived fact can be explained",
        core_datalog_explain(engine, path, ac, record_step, &explanation));
  check("the explanation walks rule, edge, path, edge",
        explanation.count == 4 && explanation.relations[0] == path &&
            explanation.rules[0] == 1 && explanation.depths[0] == 0 &&
            explanation.relations[1] == edge && explanation.rules[1] == -1 &&
            explanation.depths[1] == 1 && explanation.relations[2] == path &&
            explanation.rules[2] == 0 && explanation.depths[2] == 1 &&
            explanation.relations[3] == edge && explanation.depths[3] == 2);
  check("membership queries see derived facts",
        core_datalog_contains(engine, path, ac));
  core_datalog_symbol_t ca[] = {c, a};
  check("membership queries reject underived facts",
        !core_datalog_contains(engine, path, ca));
  check("underived facts have no explanation",
        !core_datalog_explain(engine, path, ca, record_step, &explanation));
  core_datalog_engine_free(engine);
}

int main(void) {
  test_transitive_closure_and_wide_bindings();
  test_ancestor_derivation();
  test_join_and_duplicate_suppression();
  test_unstratified_negation_is_rejected();
  test_stratified_negation_and_arithmetic_builtin();
  test_multiple_negation_strata();
  test_rule_bindings_are_not_capped_at_64();
  test_symbol_and_relation_introspection();
  test_provenance_reports_derivation_chain();
  return fails ? 1 : 0;
}
