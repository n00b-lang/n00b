#include "test_unicode_helpers.h"
#include "logic/logic_program.h"

// ============================================================================
// Collect callback for query verification
// ============================================================================

typedef struct {
    n00b_dl_sym_t *tuples;
    int32_t        count;
    int32_t        cap;
    int32_t        arity;
} collect_ctx_t;

static bool
collect_cb(const n00b_dl_sym_t *tuple, int32_t arity, void *ctx)
{
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    if (c->count >= c->cap) {
        return false;
    }
    memcpy(c->tuples + c->count * arity, tuple,
           (size_t)arity * sizeof(n00b_dl_sym_t));
    c->count++;
    c->arity = arity;
    return true;
}

// ============================================================================
// Test 1: Datalog passthrough
// ============================================================================

TEST(test_datalog_passthrough)
{
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_dl_rel_id_t edge = n00b_logic_relation(&prog, *r"edge", 2);
    n00b_dl_rel_id_t path = n00b_logic_relation(&prog, *r"path", 2);

    n00b_dl_sym_t a = n00b_logic_const(&prog, *r"a");
    n00b_dl_sym_t b = n00b_logic_const(&prog, *r"b");
    n00b_dl_sym_t c = n00b_logic_const(&prog, *r"c");

    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, b});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){b, c});

    n00b_dl_sym_t X = n00b_logic_var(&prog, *r"X");
    n00b_dl_sym_t Y = n00b_logic_var(&prog, *r"Y");
    n00b_dl_sym_t Z = n00b_logic_var(&prog, *r"Z");

    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){X, Y},
                                   false);
        n00b_logic_add_rule(&prog, n00b_dl_rule_builder_finish(&rb));
    }

    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, path, 2, (n00b_dl_sym_t[]){X, Z},
                                   false);
        n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){Z, Y},
                                   false);
        n00b_logic_add_rule(&prog, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_logic_run_datalog(&prog));
    ASSERT_EQ((int64_t)n00b_logic_count(&prog, path), 3);
    ASSERT_EQ((int64_t)n00b_logic_count(&prog, edge), 2);

    n00b_logic_free(&prog);
}

// ============================================================================
// Test 2: CSP passthrough
// ============================================================================

TEST(test_csp_passthrough)
{
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_csp_var_id_t x = n00b_logic_csp_var(&prog, *r"x",
                                               n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_logic_csp_var(&prog, *r"y",
                                               n00b_csp_dom_range(1, 3));

    ASSERT(n00b_logic_csp_ne(&prog, x, y));
    ASSERT(n00b_logic_csp_eq_const(&prog, x, 2));
    ASSERT(n00b_logic_run_csp(&prog));

    n00b_result_t(bool) gx = n00b_logic_csp_is_ground(&prog, x);
    ASSERT(n00b_result_is_ok(gx));
    ASSERT(n00b_result_get(gx) == true);

    n00b_result_t(int64_t) vx = n00b_logic_csp_value(&prog, x);
    ASSERT(n00b_result_is_ok(vx));
    ASSERT_EQ(n00b_result_get(vx), 2);

    n00b_result_t(const n00b_csp_domain_t *) dy =
        n00b_logic_csp_domain(&prog, y);
    ASSERT(n00b_result_is_ok(dy));
    ASSERT_EQ(n00b_csp_dom_size(n00b_result_get(dy)), 2);

    n00b_logic_free(&prog);
}

// ============================================================================
// Test 3: Graph coloring (Datalog edges -> CSP NE constraints)
// ============================================================================

TEST(test_graph_coloring)
{
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_dl_rel_id_t edge = n00b_logic_relation(&prog, *r"edge", 2);

    n00b_dl_sym_t a = n00b_logic_const(&prog, *r"a");
    n00b_dl_sym_t b = n00b_logic_const(&prog, *r"b");
    n00b_dl_sym_t c = n00b_logic_const(&prog, *r"c");

    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, b});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){b, c});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, c});

    ASSERT(n00b_logic_run_datalog(&prog));

    int32_t created_col0 = n00b_logic_vars_from_rel(&prog, edge, 0,
                                                      n00b_csp_dom_range(1, 3));
    int32_t created_col1 = n00b_logic_vars_from_rel(&prog, edge, 1,
                                                      n00b_csp_dom_range(1, 3));
    ASSERT_EQ(created_col0 + created_col1, 3);

    ASSERT(n00b_logic_constrain_pairs(&prog, edge, N00B_CSP_CON_NE));

    ASSERT(n00b_logic_run_csp(&prog));

    n00b_option_t(n00b_csp_var_id_t) va = n00b_logic_csp_find(&prog, a);
    n00b_option_t(n00b_csp_var_id_t) vb = n00b_logic_csp_find(&prog, b);
    n00b_option_t(n00b_csp_var_id_t) vc = n00b_logic_csp_find(&prog, c);

    ASSERT(n00b_option_is_set(va));
    ASSERT(n00b_option_is_set(vb));
    ASSERT(n00b_option_is_set(vc));

    n00b_result_t(const n00b_csp_domain_t *) da =
        n00b_logic_csp_domain(&prog, n00b_option_get(va));
    n00b_result_t(const n00b_csp_domain_t *) db =
        n00b_logic_csp_domain(&prog, n00b_option_get(vb));
    n00b_result_t(const n00b_csp_domain_t *) dc =
        n00b_logic_csp_domain(&prog, n00b_option_get(vc));

    ASSERT(n00b_result_is_ok(da));
    ASSERT(n00b_result_is_ok(db));
    ASSERT(n00b_result_is_ok(dc));

    ASSERT(!n00b_csp_dom_is_empty(n00b_result_get(da)));
    ASSERT(!n00b_csp_dom_is_empty(n00b_result_get(db)));
    ASSERT(!n00b_csp_dom_is_empty(n00b_result_get(dc)));

    n00b_logic_free(&prog);
}

// ============================================================================
// Test 4: Transitive closure then constrain
// ============================================================================

TEST(test_transitive_then_constrain)
{
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_dl_rel_id_t edge = n00b_logic_relation(&prog, *r"edge", 2);
    n00b_dl_rel_id_t path = n00b_logic_relation(&prog, *r"path", 2);

    n00b_dl_sym_t a = n00b_logic_const(&prog, *r"a");
    n00b_dl_sym_t b = n00b_logic_const(&prog, *r"b");
    n00b_dl_sym_t c = n00b_logic_const(&prog, *r"c");

    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, b});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){b, c});

    n00b_dl_sym_t X = n00b_logic_var(&prog, *r"X");
    n00b_dl_sym_t Y = n00b_logic_var(&prog, *r"Y");
    n00b_dl_sym_t Z = n00b_logic_var(&prog, *r"Z");

    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){X, Y},
                                   false);
        n00b_logic_add_rule(&prog, n00b_dl_rule_builder_finish(&rb));
    }
    {
        n00b_dl_rule_builder_t rb;
        n00b_dl_rule_builder_init(&rb);
        n00b_dl_rule_builder_head(&rb, path, 2, (n00b_dl_sym_t[]){X, Y});
        n00b_dl_rule_builder_add(&rb, path, 2, (n00b_dl_sym_t[]){X, Z},
                                   false);
        n00b_dl_rule_builder_add(&rb, edge, 2, (n00b_dl_sym_t[]){Z, Y},
                                   false);
        n00b_logic_add_rule(&prog, n00b_dl_rule_builder_finish(&rb));
    }

    ASSERT(n00b_logic_run_datalog(&prog));
    ASSERT_EQ((int64_t)n00b_logic_count(&prog, path), 3);

    n00b_logic_vars_from_rel(&prog, path, 0, n00b_csp_dom_range(1, 10));
    n00b_logic_vars_from_rel(&prog, path, 1, n00b_csp_dom_range(1, 10));
    ASSERT(n00b_logic_constrain_pairs(&prog, path, N00B_CSP_CON_NE));
    ASSERT(n00b_logic_run_csp(&prog));

    n00b_option_t(n00b_csp_var_id_t) va = n00b_logic_csp_find(&prog, a);
    ASSERT(n00b_option_is_set(va));
    n00b_result_t(const n00b_csp_domain_t *) da =
        n00b_logic_csp_domain(&prog, n00b_option_get(va));
    ASSERT(n00b_result_is_ok(da));
    ASSERT(!n00b_csp_dom_is_empty(n00b_result_get(da)));

    n00b_logic_free(&prog);
}

// ============================================================================
// Test 5: Push/pop search
// ============================================================================

TEST(test_push_pop_search)
{
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_csp_var_id_t x = n00b_logic_csp_var(&prog, *r"x",
                                               n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t y = n00b_logic_csp_var(&prog, *r"y",
                                               n00b_csp_dom_range(1, 2));

    ASSERT(n00b_logic_csp_ne(&prog, x, y));

    int solutions = 0;

    n00b_logic_csp_push(&prog);
    if (n00b_logic_csp_eq_const(&prog, x, 1) && n00b_logic_run_csp(&prog)) {
        n00b_result_t(int64_t) vy = n00b_logic_csp_value(&prog, y);
        if (n00b_result_is_ok(vy)) {
            ASSERT_EQ(n00b_result_get(vy), 2);
            solutions++;
        }
    }
    n00b_logic_csp_pop(&prog);

    n00b_logic_csp_push(&prog);
    if (n00b_logic_csp_eq_const(&prog, x, 2) && n00b_logic_run_csp(&prog)) {
        n00b_result_t(int64_t) vy = n00b_logic_csp_value(&prog, y);
        if (n00b_result_is_ok(vy)) {
            ASSERT_EQ(n00b_result_get(vy), 1);
            solutions++;
        }
    }
    n00b_logic_csp_pop(&prog);

    ASSERT_EQ(solutions, 2);

    n00b_logic_free(&prog);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    RUN_TEST(test_datalog_passthrough);
    RUN_TEST(test_csp_passthrough);
    RUN_TEST(test_graph_coloring);
    RUN_TEST(test_transitive_then_constrain);
    RUN_TEST(test_push_pop_search);
}

TEST_MAIN()
