#include "test_unicode_helpers.h"
#include "logic/logic_program.h"

// ============================================================================
// Store API additions
// ============================================================================

TEST(test_var_count)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    ASSERT_EQ(n00b_csp_var_count(s), 0);

    n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 5));
    ASSERT_EQ(n00b_csp_var_count(s), 1);

    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 5));
    ASSERT_EQ(n00b_csp_var_count(s), 2);

    n00b_csp_store_free(s);
}

typedef struct {
    int64_t *values;
    int32_t  count;
    int32_t  cap;
} iter_ctx_t;

static bool
iter_cb(int64_t value, void *ctx)
{
    iter_ctx_t *c = (iter_ctx_t *)ctx;
    if (c->count >= c->cap) {
        return false;
    }
    c->values[c->count++] = value;
    return true;
}

TEST(test_dom_iterate_interval)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 5));

    int64_t    buf[10];
    iter_ctx_t ctx = { .values = buf, .count = 0, .cap = 10 };

    int32_t n = n00b_csp_dom_iterate(s, x, iter_cb, &ctx);
    ASSERT_EQ(n, 5);
    ASSERT_EQ(ctx.count, 5);
    ASSERT_EQ(buf[0], 1);
    ASSERT_EQ(buf[1], 2);
    ASSERT_EQ(buf[2], 3);
    ASSERT_EQ(buf[3], 4);
    ASSERT_EQ(buf[4], 5);

    n00b_csp_store_free(s);
}

TEST(test_dom_iterate_bitset)
{
    n00b_csp_store_t *s = n00b_csp_store_new();

    // Create a bitset domain by starting with [1,5] and removing 3.
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 5));
    // Remove middle value to promote to bitset.
    int64_t vals[] = {1, 2, 4, 5};
    n00b_csp_post_in(s, x, n00b_csp_dom_from_values(vals, 4));

    int64_t    buf[10];
    iter_ctx_t ctx = { .values = buf, .count = 0, .cap = 10 };

    int32_t n = n00b_csp_dom_iterate(s, x, iter_cb, &ctx);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(buf[0], 1);
    ASSERT_EQ(buf[1], 2);
    ASSERT_EQ(buf[2], 4);
    ASSERT_EQ(buf[3], 5);

    n00b_csp_store_free(s);
}

TEST(test_dom_iterate_sparse)
{
    n00b_csp_store_t *s = n00b_csp_store_new();

    // Values spread more than 64 apart forces sparse representation.
    int64_t vals[] = {10, 100, 1000};
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_from_values(vals, 3));

    int64_t    buf[10];
    iter_ctx_t ctx = { .values = buf, .count = 0, .cap = 10 };

    int32_t n = n00b_csp_dom_iterate(s, x, iter_cb, &ctx);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(buf[0], 10);
    ASSERT_EQ(buf[1], 100);
    ASSERT_EQ(buf[2], 1000);

    n00b_csp_store_free(s);
}

// ============================================================================
// Labeling: find-first
// ============================================================================

TEST(test_label_trivial)
{
    // All variables already ground -> label returns true immediately.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"X", n00b_csp_dom_singleton(1));
    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_singleton(2));

    ASSERT(n00b_csp_label(s));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, 0)), 1);
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, 1)), 2);

    n00b_csp_store_free(s);
}

TEST(test_label_single_var)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 3));

    ASSERT(n00b_csp_label(s));
    // MRV picks the only unground var, tries smallest value first.
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 1);

    n00b_csp_store_free(s);
}

TEST(test_label_ne_pair)
{
    // X, Y in {1,2}, X != Y -> should find a solution.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(1, 2));
    n00b_csp_post_ne(s, x, y);

    ASSERT(n00b_csp_label(s));
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));

    int64_t vx = n00b_result_get(n00b_csp_var_value(s, x));
    int64_t vy = n00b_result_get(n00b_csp_var_value(s, y));
    ASSERT(vx != vy);
    ASSERT(vx >= 1 && vx <= 2);
    ASSERT(vy >= 1 && vy <= 2);

    n00b_csp_store_free(s);
}

TEST(test_label_triangle)
{
    // Triangle graph: a-b, b-c, a-c, colors in {1,2,3}.
    // Must find a valid 3-coloring.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t a = n00b_csp_new_var(s, *r"a",
                                             n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t b = n00b_csp_new_var(s, *r"b",
                                             n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t c = n00b_csp_new_var(s, *r"c",
                                             n00b_csp_dom_range(1, 3));

    n00b_csp_post_ne(s, a, b);
    n00b_csp_post_ne(s, b, c);
    n00b_csp_post_ne(s, a, c);

    ASSERT(n00b_csp_label(s));

    int64_t va = n00b_result_get(n00b_csp_var_value(s, a));
    int64_t vb = n00b_result_get(n00b_csp_var_value(s, b));
    int64_t vc = n00b_result_get(n00b_csp_var_value(s, c));

    ASSERT(va != vb);
    ASSERT(vb != vc);
    ASSERT(va != vc);
    ASSERT(va >= 1 && va <= 3);
    ASSERT(vb >= 1 && vb <= 3);
    ASSERT(vc >= 1 && vc <= 3);

    n00b_csp_store_free(s);
}

TEST(test_label_infeasible)
{
    // X in {1}, Y in {1}, X != Y -> unsatisfiable.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"X", n00b_csp_dom_singleton(1));
    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_singleton(1));
    n00b_csp_post_ne(s, 0, 1);

    // NE with both ground and equal fails at posting.
    // Either post_ne returns false or label can't find a solution.
    // Since both are already ground with same value, post_ne
    // should make domain empty. label should return false.
    // Actually, the store is already failed after post_ne, but
    // let's test label on a non-trivially infeasible problem.
    n00b_csp_store_free(s);

    // Better test: X,Y in {1,2}, X != Y, X == Y.
    s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(1, 2));
    n00b_csp_post_ne(s, x, y);
    n00b_csp_post_eq(s, x, y);

    // Should be immediately unsatisfiable.
    // But if not, label should return false.
    bool solved = n00b_csp_label(s);
    ASSERT(!solved);

    n00b_csp_store_free(s);
}

// ============================================================================
// Labeling: enumerate all
// ============================================================================

typedef struct {
    int64_t count;
} count_ctx_t;

static bool
count_solution_cb(n00b_csp_store_t *s, void *ctx)
{
    (void)s;
    count_ctx_t *c = (count_ctx_t *)ctx;
    c->count++;
    return true;
}

TEST(test_label_all_count)
{
    // X, Y in {1,2}, X != Y -> exactly 2 solutions.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 2));
    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 2));
    n00b_csp_post_ne(s, 0, 1);

    count_ctx_t ctx = { .count = 0 };
    int64_t     n   = n00b_csp_label_all(s, count_solution_cb, &ctx);
    ASSERT_EQ(n, 2);
    ASSERT_EQ(ctx.count, 2);

    // Store should be restored to pre-call state.
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, 0)));
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, 1)));

    n00b_csp_store_free(s);
}

TEST(test_label_all_triangle)
{
    // Triangle 3-coloring: 3! = 6 solutions.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"a", n00b_csp_dom_range(1, 3));
    n00b_csp_new_var(s, *r"b", n00b_csp_dom_range(1, 3));
    n00b_csp_new_var(s, *r"c", n00b_csp_dom_range(1, 3));
    n00b_csp_post_ne(s, 0, 1);
    n00b_csp_post_ne(s, 1, 2);
    n00b_csp_post_ne(s, 0, 2);

    int64_t n = n00b_csp_label_all(s, nullptr, nullptr);
    ASSERT_EQ(n, 6);

    n00b_csp_store_free(s);
}

static bool
stop_after_one_cb(n00b_csp_store_t *s, void *ctx)
{
    (void)s;
    count_ctx_t *c = (count_ctx_t *)ctx;
    c->count++;
    return false;  // Stop after first solution.
}

TEST(test_label_all_early_stop)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 3));
    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 3));
    n00b_csp_post_ne(s, 0, 1);

    count_ctx_t ctx = { .count = 0 };
    int64_t     n   = n00b_csp_label_all(s, stop_after_one_cb, &ctx);

    // Should stop after finding the first solution.
    ASSERT_EQ(ctx.count, 1);
    ASSERT_EQ(n, 1);

    n00b_csp_store_free(s);
}

// ============================================================================
// Logic program solve
// ============================================================================

TEST(test_logic_solve)
{
    // Full pipeline: Datalog graph coloring with labeling.
    n00b_logic_t prog;
    n00b_logic_init(&prog);

    n00b_dl_rel_id_t edge = n00b_logic_relation(&prog, *r"edge", 2);
    n00b_dl_sym_t a = n00b_logic_const(&prog, *r"a");
    n00b_dl_sym_t b = n00b_logic_const(&prog, *r"b");
    n00b_dl_sym_t c = n00b_logic_const(&prog, *r"c");

    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, b});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){b, c});
    n00b_logic_add_fact(&prog, edge, 2, (n00b_dl_sym_t[]){a, c});

    n00b_logic_run_datalog(&prog);

    n00b_logic_vars_from_rel(&prog, edge, 0, n00b_csp_dom_range(1, 3));
    n00b_logic_vars_from_rel(&prog, edge, 1, n00b_csp_dom_range(1, 3));
    n00b_logic_constrain_pairs(&prog, edge, N00B_CSP_CON_NE);

    // Don't call run() again — solve() does it internally.
    // Actually, we already ran datalog and posted constraints.
    // We just need labeling. Use the store directly.
    ASSERT(n00b_csp_label(prog.store));

    n00b_option_t(n00b_csp_var_id_t) va = n00b_logic_csp_find(&prog, a);
    n00b_option_t(n00b_csp_var_id_t) vb = n00b_logic_csp_find(&prog, b);
    n00b_option_t(n00b_csp_var_id_t) vc = n00b_logic_csp_find(&prog, c);

    ASSERT(n00b_option_is_set(va));
    ASSERT(n00b_option_is_set(vb));
    ASSERT(n00b_option_is_set(vc));

    int64_t color_a = n00b_result_get(n00b_logic_csp_value(&prog, n00b_option_get(va)));
    int64_t color_b = n00b_result_get(n00b_logic_csp_value(&prog, n00b_option_get(vb)));
    int64_t color_c = n00b_result_get(n00b_logic_csp_value(&prog, n00b_option_get(vc)));

    ASSERT(color_a != color_b);
    ASSERT(color_b != color_c);
    ASSERT(color_a != color_c);

    n00b_logic_free(&prog);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    // Store API
    RUN_TEST(test_var_count);
    RUN_TEST(test_dom_iterate_interval);
    RUN_TEST(test_dom_iterate_bitset);
    RUN_TEST(test_dom_iterate_sparse);

    // Labeling: find-first
    RUN_TEST(test_label_trivial);
    RUN_TEST(test_label_single_var);
    RUN_TEST(test_label_ne_pair);
    RUN_TEST(test_label_triangle);
    RUN_TEST(test_label_infeasible);

    // Labeling: enumerate all
    RUN_TEST(test_label_all_count);
    RUN_TEST(test_label_all_triangle);
    RUN_TEST(test_label_all_early_stop);

    // Logic program integration
    RUN_TEST(test_logic_solve);
}

TEST_MAIN()
