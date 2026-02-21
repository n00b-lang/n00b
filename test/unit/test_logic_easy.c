#include "test_unicode_helpers.h"
#include "logic/logic_program.h"

// ============================================================================
// Test 1: n00b_logic_new / n00b_logic_free (heap lifecycle)
// ============================================================================

TEST(test_new_free)
{
    n00b_logic_t *prog = n00b_logic_new();
    ASSERT(prog != nullptr);
    ASSERT(prog->_heap == true);
    n00b_logic_free(prog);
}

// ============================================================================
// Test 2: n00b_logic_fact variadic with different arities
// ============================================================================

TEST(test_fact_variadic)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_fact(prog, *r"node", r"a");
    n00b_logic_fact(prog, *r"edge", r"a", r"b");
    n00b_logic_fact(prog, *r"edge", r"b", r"c");
    n00b_logic_fact(prog, *r"triple", r"x", r"y", r"z");

    ASSERT(n00b_logic_run_datalog(prog));

    ASSERT_EQ((int64_t)n00b_logic_count(prog, n00b_logic_relation(prog, *r"node", 1)), 1);
    ASSERT_EQ((int64_t)n00b_logic_count(prog, n00b_logic_relation(prog, *r"edge", 2)), 2);
    ASSERT_EQ((int64_t)n00b_logic_count(prog, n00b_logic_relation(prog, *r"triple", 3)), 1);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 3: n00b_logic_bridge creates vars for all columns
// ============================================================================

TEST(test_bridge_basic)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_fact(prog, *r"edge", r"a", r"b");
    n00b_logic_fact(prog, *r"edge", r"b", r"c");

    int32_t created = n00b_logic_bridge(prog, *r"edge",
                                          .domain = n00b_csp_dom_range(1, 5));
    ASSERT_EQ(created, 3);

    n00b_result_t(int64_t) r = n00b_logic_get_int(prog, *r"a");
    ASSERT(n00b_result_is_ok(r) || n00b_result_get_err(r) == EINVAL);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 4: n00b_logic_bridge with NE constraint
// ============================================================================

TEST(test_bridge_with_constraint)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_fact(prog, *r"edge", r"a", r"b");
    n00b_logic_fact(prog, *r"edge", r"b", r"c");
    n00b_logic_fact(prog, *r"edge", r"a", r"c");

    int32_t created = n00b_logic_bridge(prog, *r"edge",
                                          .domain     = n00b_csp_dom_range(1, 3),
                                          .constraint = N00B_CSP_CON_NE);
    ASSERT_EQ(created, 3);

    ASSERT(n00b_logic_run_csp(prog));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 5: n00b_logic_int_var
// ============================================================================

TEST(test_int_var)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_csp_var_id_t x = n00b_logic_int_var(prog, *r"x", 1, 10);
    n00b_csp_var_id_t y = n00b_logic_int_var(prog, *r"y", 1, 10);

    ASSERT(n00b_logic_csp_ne(prog, x, y));
    ASSERT(n00b_logic_csp_eq_const(prog, x, 5));
    ASSERT(n00b_logic_run_csp(prog));

    n00b_result_t(int64_t) vx = n00b_logic_csp_value(prog, x);
    ASSERT(n00b_result_is_ok(vx));
    ASSERT_EQ(n00b_result_get(vx), 5);

    n00b_result_t(const n00b_csp_domain_t *) dy = n00b_logic_csp_domain(prog, y);
    ASSERT(n00b_result_is_ok(dy));
    ASSERT(!n00b_csp_dom_contains(n00b_result_get(dy), 5));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 6: n00b_logic_constrain by name
// ============================================================================

TEST(test_constrain_by_name)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);

    ASSERT(n00b_logic_constrain(prog, *r"x", *r"y", N00B_CSP_CON_NE));
    ASSERT(n00b_logic_run_csp(prog));

    ASSERT(!n00b_logic_constrain(prog, *r"x", *r"z", N00B_CSP_CON_NE));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 7: n00b_logic_get_int reads solved value
// ============================================================================

TEST(test_get_int)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);
    n00b_logic_constrain(prog, *r"x", *r"y", N00B_CSP_CON_NE);
    n00b_logic_csp_eq_const(prog, n00b_logic_int_var(prog, *r"z", 7, 7), 7);

    ASSERT(n00b_logic_solve(prog));

    n00b_result_t(int64_t) vz = n00b_logic_get_int(prog, *r"z");
    ASSERT(n00b_result_is_ok(vz));
    ASSERT_EQ(n00b_result_get(vz), 7);

    n00b_result_t(int64_t) vx = n00b_logic_get_int(prog, *r"x");
    n00b_result_t(int64_t) vy = n00b_logic_get_int(prog, *r"y");
    ASSERT(n00b_result_is_ok(vx));
    ASSERT(n00b_result_is_ok(vy));
    ASSERT(n00b_result_get(vx) != n00b_result_get(vy));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 8: n00b_logic_get_int returns ENOENT for unknown
// ============================================================================

TEST(test_get_int_missing)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    ASSERT(n00b_logic_solve(prog));

    n00b_result_t(int64_t) r = n00b_logic_get_int(prog, *r"nonexistent");
    ASSERT(!n00b_result_is_ok(r));
    ASSERT_EQ(n00b_result_get_err(r), ENOENT);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 9: Full graph coloring end-to-end
// ============================================================================

TEST(test_graph_coloring)
{
    auto prog = n00b_logic_new();

    n00b_logic_fact(prog, *r"edge", r"a", r"b");
    n00b_logic_fact(prog, *r"edge", r"b", r"c");
    n00b_logic_fact(prog, *r"edge", r"a", r"c");

    n00b_logic_bridge(prog, *r"edge",
                       .domain     = n00b_csp_dom_range(1, 3),
                       .constraint = N00B_CSP_CON_NE);

    ASSERT(n00b_logic_solve(prog));

    n00b_result_t(int64_t) ca = n00b_logic_get_int(prog, *r"a");
    n00b_result_t(int64_t) cb = n00b_logic_get_int(prog, *r"b");
    n00b_result_t(int64_t) cc = n00b_logic_get_int(prog, *r"c");

    ASSERT(n00b_result_is_ok(ca));
    ASSERT(n00b_result_is_ok(cb));
    ASSERT(n00b_result_is_ok(cc));

    int64_t va = n00b_result_get(ca);
    int64_t vb = n00b_result_get(cb);
    int64_t vc = n00b_result_get(cc);

    ASSERT(va >= 1 && va <= 3);
    ASSERT(vb >= 1 && vb <= 3);
    ASSERT(vc >= 1 && vc <= 3);
    ASSERT(va != vb);
    ASSERT(vb != vc);
    ASSERT(va != vc);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 10: n00b_logic_solve_all enumerates solutions
// ============================================================================

static int64_t solve_all_count;

static bool
count_solutions_cb(n00b_logic_t *prog, void *ctx)
{
    (void)ctx;
    n00b_result_t(int64_t) vx = n00b_logic_get_int(prog, *r"x");
    n00b_result_t(int64_t) vy = n00b_logic_get_int(prog, *r"y");
    if (!n00b_result_is_ok(vx) || !n00b_result_is_ok(vy)) {
        return false;
    }
    if (n00b_result_get(vx) == n00b_result_get(vy)) {
        return false;
    }
    solve_all_count++;
    return true;
}

TEST(test_solve_all)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);
    n00b_logic_constrain(prog, *r"x", *r"y", N00B_CSP_CON_NE);

    solve_all_count = 0;
    int64_t total = n00b_logic_solve_all(prog, count_solutions_cb, nullptr);

    ASSERT_EQ(total, 6);
    ASSERT_EQ(solve_all_count, 6);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 11: n00b_logic_alldiff by name
// ============================================================================

TEST(test_alldiff_by_name)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);
    n00b_logic_int_var(prog, *r"z", 1, 3);

    ASSERT(n00b_logic_alldiff(prog, r"x", r"y", r"z"));
    ASSERT(n00b_logic_solve(prog));

    n00b_result_t(int64_t) vx = n00b_logic_get_int(prog, *r"x");
    n00b_result_t(int64_t) vy = n00b_logic_get_int(prog, *r"y");
    n00b_result_t(int64_t) vz = n00b_logic_get_int(prog, *r"z");

    ASSERT(n00b_result_is_ok(vx));
    ASSERT(n00b_result_is_ok(vy));
    ASSERT(n00b_result_is_ok(vz));

    int64_t x = n00b_result_get(vx);
    int64_t y = n00b_result_get(vy);
    int64_t z = n00b_result_get(vz);

    ASSERT(x != y);
    ASSERT(y != z);
    ASSERT(x != z);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 12: n00b_logic_alldiff with missing var returns false
// ============================================================================

TEST(test_alldiff_missing_var)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);

    ASSERT(!n00b_logic_alldiff(prog, r"x", r"y", r"missing"));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 13: n00b_logic_alldiff solve_all (3 vars in 1..3 => 3! = 6)
// ============================================================================

static int64_t alldiff_solution_count;

static bool
alldiff_count_cb(n00b_logic_t *prog, void *ctx)
{
    (void)ctx;
    n00b_result_t(int64_t) vx = n00b_logic_get_int(prog, *r"x");
    n00b_result_t(int64_t) vy = n00b_logic_get_int(prog, *r"y");
    n00b_result_t(int64_t) vz = n00b_logic_get_int(prog, *r"z");
    if (!n00b_result_is_ok(vx) || !n00b_result_is_ok(vy)
        || !n00b_result_is_ok(vz)) {
        return false;
    }

    int64_t x = n00b_result_get(vx);
    int64_t y = n00b_result_get(vy);
    int64_t z = n00b_result_get(vz);
    if (x == y || y == z || x == z) {
        return false;
    }

    alldiff_solution_count++;
    return true;
}

TEST(test_alldiff_solve_all)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);
    n00b_logic_int_var(prog, *r"z", 1, 3);

    ASSERT(n00b_logic_alldiff(prog, r"x", r"y", r"z"));

    alldiff_solution_count = 0;
    int64_t total = n00b_logic_solve_all(prog, alldiff_count_cb, nullptr);

    ASSERT_EQ(total, 6);
    ASSERT_EQ(alldiff_solution_count, 6);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 14: n00b_logic_linear by name (2x + 3y == 12)
// ============================================================================

TEST(test_linear_by_name)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 0, 10);
    n00b_logic_int_var(prog, *r"y", 0, 10);

    n00b_linear_term_t terms[] = {
        { .coeff = 2, .name = *r"x" },
        { .coeff = 3, .name = *r"y" },
    };
    ASSERT(n00b_logic_linear(prog, terms, 2, .rhs = 12));
    ASSERT(n00b_logic_solve(prog));

    n00b_result_t(int64_t) vx = n00b_logic_get_int(prog, *r"x");
    n00b_result_t(int64_t) vy = n00b_logic_get_int(prog, *r"y");
    ASSERT(n00b_result_is_ok(vx));
    ASSERT(n00b_result_is_ok(vy));

    int64_t x = n00b_result_get(vx);
    int64_t y = n00b_result_get(vy);
    ASSERT_EQ(2 * x + 3 * y, 12);

    n00b_logic_free(prog);
}

// ============================================================================
// Test 15: n00b_logic_linear missing var
// ============================================================================

TEST(test_linear_missing_var)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 0, 10);

    n00b_linear_term_t terms[] = {
        { .coeff = 1, .name = *r"x" },
        { .coeff = 1, .name = *r"missing" },
    };
    ASSERT(!n00b_logic_linear(prog, terms, 2, .rhs = 5));

    n00b_logic_free(prog);
}

// ============================================================================
// Test 16: linear + alldiff combined (permutations of 1,2,3 summing to 6)
// ============================================================================

TEST(test_linear_plus_alldiff)
{
    n00b_logic_t *prog = n00b_logic_new();

    n00b_logic_int_var(prog, *r"x", 1, 3);
    n00b_logic_int_var(prog, *r"y", 1, 3);
    n00b_logic_int_var(prog, *r"z", 1, 3);

    ASSERT(n00b_logic_alldiff(prog, r"x", r"y", r"z"));

    n00b_linear_term_t terms[] = {
        { .coeff = 1, .name = *r"x" },
        { .coeff = 1, .name = *r"y" },
        { .coeff = 1, .name = *r"z" },
    };
    ASSERT(n00b_logic_linear(prog, terms, 3, .rhs = 6));

    int64_t total = n00b_logic_solve_all(prog, nullptr, nullptr);
    // alldiff {1,2,3} with x+y+z=6 => all permutations of (1,2,3) => 6
    ASSERT_EQ(total, 6);

    n00b_logic_free(prog);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    RUN_TEST(test_new_free);
    RUN_TEST(test_fact_variadic);
    RUN_TEST(test_bridge_basic);
    RUN_TEST(test_bridge_with_constraint);
    RUN_TEST(test_int_var);
    RUN_TEST(test_constrain_by_name);
    RUN_TEST(test_get_int);
    RUN_TEST(test_get_int_missing);
    RUN_TEST(test_graph_coloring);
    RUN_TEST(test_solve_all);
    RUN_TEST(test_alldiff_by_name);
    RUN_TEST(test_alldiff_missing_var);
    RUN_TEST(test_alldiff_solve_all);
    RUN_TEST(test_linear_by_name);
    RUN_TEST(test_linear_missing_var);
    RUN_TEST(test_linear_plus_alldiff);
}

TEST_MAIN()
