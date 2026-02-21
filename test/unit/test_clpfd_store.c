#include "test_unicode_helpers.h"
#include "logic/clpfd_store.h"
#include "logic/clpfd_label.h"

// ============================================================================
// Lifecycle
// ============================================================================

TEST(test_store_new_free)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    ASSERT(s != nullptr);
    n00b_csp_store_free(s);
}

TEST(test_new_var)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(1, 100));

    ASSERT(x != y);

    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    ASSERT(dx != nullptr);
    ASSERT_EQ(n00b_csp_dom_min(dx), 1);
    ASSERT_EQ(n00b_csp_dom_max(dx), 10);

    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, y)));

    n00b_csp_store_free(s);
}

TEST(test_find_var)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 10));
    n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 10));

    ASSERT(n00b_option_is_set(n00b_csp_find_var(s, *r"X")));
    ASSERT(n00b_option_is_set(n00b_csp_find_var(s, *r"Y")));
    ASSERT(!n00b_option_is_set(n00b_csp_find_var(s, *r"Z")));

    n00b_csp_store_free(s);
}

TEST(test_singleton_var)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_singleton(42));

    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 42);

    n00b_csp_store_free(s);
}

// ============================================================================
// Constraint posting and propagation
// ============================================================================

TEST(test_eq_const)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 100));

    bool ok = n00b_csp_post_eq_const(s, x, 50);
    ASSERT(ok);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 50);

    n00b_csp_store_free(s);
}

TEST(test_eq_const_infeasible)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 10));

    bool ok = n00b_csp_post_eq_const(s, x, 50);
    ASSERT(!ok);

    n00b_csp_store_free(s);
}

TEST(test_eq_vars)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(5, 20));

    bool ok = n00b_csp_post_eq(s, x, y);
    ASSERT(ok);

    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    const n00b_csp_domain_t *dy = n00b_result_get(n00b_csp_var_domain(s, y));

    ASSERT_EQ(n00b_csp_dom_min(dx), 5);
    ASSERT_EQ(n00b_csp_dom_max(dx), 10);
    ASSERT_EQ(n00b_csp_dom_min(dy), 5);
    ASSERT_EQ(n00b_csp_dom_max(dy), 10);

    n00b_csp_store_free(s);
}

TEST(test_lt)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(0, 100));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(0, 100));

    n00b_csp_post_eq_const(s, y, 50);
    bool ok = n00b_csp_post_lt(s, x, y);
    ASSERT(ok);

    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 49);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, y)), 50);

    n00b_csp_store_free(s);
}

TEST(test_le)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(0, 100));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(0, 100));

    n00b_csp_post_eq_const(s, y, 50);
    bool ok = n00b_csp_post_le(s, x, y);
    ASSERT(ok);
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 50);

    n00b_csp_store_free(s);
}

TEST(test_ne)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_singleton(2));

    bool ok = n00b_csp_post_ne(s, x, y);
    ASSERT(ok);

    ASSERT_EQ((int64_t)n00b_csp_dom_size(n00b_result_get(n00b_csp_var_domain(s, x))), 2);
    ASSERT(!n00b_csp_dom_contains(n00b_result_get(n00b_csp_var_domain(s, x)), 2));

    n00b_csp_store_free(s);
}

TEST(test_in)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 100));

    int64_t vals[] = {10, 20, 30};
    bool ok = n00b_csp_post_in(s, x, n00b_csp_dom_from_values(vals, 3));
    ASSERT(ok);
    ASSERT_EQ((int64_t)n00b_csp_dom_size(n00b_result_get(n00b_csp_var_domain(s, x))), 3);

    n00b_csp_store_free(s);
}

// ============================================================================
// Combined constraint propagation
// ============================================================================

TEST(test_combined_propagation)
{
    // X in [0, 100], Y in [0, 100]
    // X < Y, Y = 50
    // Expected: X in [0, 49], Y = 50
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(0, 100));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(0, 100));

    n00b_csp_post_lt(s, x, y);
    n00b_csp_post_eq_const(s, y, 50);

    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 49);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, y)), 50);

    n00b_csp_store_free(s);
}

TEST(test_chain_propagation)
{
    // X < Y < Z, Z = 5
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(0, 100));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(0, 100));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z",
                                             n00b_csp_dom_range(0, 100));

    n00b_csp_post_lt(s, x, y);
    n00b_csp_post_lt(s, y, z);
    n00b_csp_post_eq_const(s, z, 5);

    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, y))), 4);
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 3);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, z)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, z)), 5);

    n00b_csp_store_free(s);
}

// ============================================================================
// Backtracking
// ============================================================================

TEST(test_backtracking)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 10));

    n00b_csp_push_state(s);
    n00b_csp_post_eq_const(s, x, 5);

    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 5);

    n00b_csp_pop_state(s);

    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_csp_dom_min(dx), 1);
    ASSERT_EQ(n00b_csp_dom_max(dx), 10);

    n00b_csp_store_free(s);
}

TEST(test_nested_backtracking)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 100));

    n00b_csp_push_state(s);
    n00b_csp_post_le(s, x,
                      n00b_csp_new_var(s, *r"_t1",
                                        n00b_csp_dom_singleton(50)));
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 50);

    n00b_csp_push_state(s);
    n00b_csp_post_eq_const(s, x, 25);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));

    n00b_csp_pop_state(s);
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 50);
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, x)));

    n00b_csp_pop_state(s);
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 100);

    n00b_csp_store_free(s);
}

// ============================================================================
// Failure detection
// ============================================================================

TEST(test_failure_detection)
{
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 5));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(10, 20));

    // X = Y should fail (disjoint domains).
    bool ok = n00b_csp_post_eq(s, x, y);
    ASSERT(!ok);

    n00b_csp_store_free(s);
}

// ============================================================================
// Bug fix regression tests
// ============================================================================

TEST(test_entailment_after_backtrack)
{
    // Bug fix 1a regression test:
    // Post EQ, let it become entailed (both ground), pop, verify
    // the constraint re-propagates correctly on the restored domains.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_range(1, 10));

    // Post X = Y.
    n00b_csp_post_eq(s, x, y);

    n00b_csp_push_state(s);

    // Narrow X to singleton 5 -> Y also narrows to 5 -> EQ is entailed.
    n00b_csp_post_eq_const(s, x, 5);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 5);
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, y)), 5);

    // Pop: X and Y restored to [1, 10].
    n00b_csp_pop_state(s);
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, y)));

    // Now narrow X to 3 — Y should also narrow to 3 via the EQ
    // constraint, which must NOT still be marked entailed.
    bool ok = n00b_csp_post_eq_const(s, x, 3);
    ASSERT(ok);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 3);
    // The EQ constraint should have re-propagated.
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, y)), 3);

    n00b_csp_store_free(s);
}

TEST(test_constraint_backtracking)
{
    // Bug fix 1b regression test:
    // Push, post constraint, pop — verify the constraint is removed
    // and doesn't affect further propagation.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 100));

    n00b_csp_push_state(s);

    // Post X = 50 during this branch.
    n00b_csp_post_eq_const(s, x, 50);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 50);

    // Pop: X restored to [1, 100], and the eq_const constraint is gone.
    n00b_csp_pop_state(s);
    ASSERT(!n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_csp_dom_min(n00b_result_get(n00b_csp_var_domain(s, x))), 1);
    ASSERT_EQ(n00b_csp_dom_max(n00b_result_get(n00b_csp_var_domain(s, x))), 100);

    // Post X = 75 — should succeed because the old constraint is gone.
    bool ok = n00b_csp_post_eq_const(s, x, 75);
    ASSERT(ok);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 75);

    n00b_csp_store_free(s);
}

TEST(test_backtrack_with_ne_constraint)
{
    // More complex regression test: NE constraint posted in branch
    // should not persist after pop.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X",
                                             n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y",
                                             n00b_csp_dom_singleton(2));

    n00b_csp_push_state(s);

    // Post X != Y: removes 2 from X -> X = {1, 3}.
    n00b_csp_post_ne(s, x, y);
    ASSERT(!n00b_csp_dom_contains(n00b_result_get(n00b_csp_var_domain(s, x)), 2));

    n00b_csp_pop_state(s);

    // X should be restored to {1, 2, 3}.
    ASSERT_EQ((int64_t)n00b_csp_dom_size(n00b_result_get(n00b_csp_var_domain(s, x))), 3);
    ASSERT(n00b_csp_dom_contains(n00b_result_get(n00b_csp_var_domain(s, x)), 2));

    // Should be able to set X = 2 now (no NE constraint).
    bool ok = n00b_csp_post_eq_const(s, x, 2);
    ASSERT(ok);
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 2);

    n00b_csp_store_free(s);
}

// ============================================================================
// LINEAR constraint
// ============================================================================

TEST(test_linear_basic)
{
    // X + Y = 10, X in [0,10], Y in [0,10]
    // Should narrow both to [0,10] (unchanged), but establish the constraint.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(0, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(0, 10));

    n00b_csp_var_id_t vars[]  = {x, y};
    int64_t           coeffs[] = {1, 1};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 2, 10);
    ASSERT(ok);

    // Both domains should still include 0..10.
    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    const n00b_csp_domain_t *dy = n00b_result_get(n00b_csp_var_domain(s, y));
    ASSERT_EQ(n00b_csp_dom_min(dx), 0);
    ASSERT_EQ(n00b_csp_dom_max(dx), 10);
    ASSERT_EQ(n00b_csp_dom_min(dy), 0);
    ASSERT_EQ(n00b_csp_dom_max(dy), 10);

    n00b_csp_store_free(s);
}

TEST(test_linear_narrowing)
{
    // 2*X + 3*Y = 12, X in [0,10], Y in [0,10]
    // X: ceil(12 - 3*10) / 2 .. floor(12 - 3*0) / 2 = ceil(-18/2)..floor(12/2) = 0..6
    //    but also X >= 0, so [0, 6]
    // Y: (12 - 2*6) / 3 .. (12 - 2*0) / 3 = 0/3..12/3 = 0..4
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(0, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(0, 10));

    n00b_csp_var_id_t vars[]  = {x, y};
    int64_t           coeffs[] = {2, 3};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 2, 12);
    ASSERT(ok);

    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    const n00b_csp_domain_t *dy = n00b_result_get(n00b_csp_var_domain(s, y));
    ASSERT_EQ(n00b_csp_dom_max(dx), 6);
    ASSERT_EQ(n00b_csp_dom_max(dy), 4);

    n00b_csp_store_free(s);
}

TEST(test_linear_infeasible)
{
    // X + Y = 100, X in [0,10], Y in [0,10]
    // sum_hi = 20 < 100 → fail
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(0, 10));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(0, 10));

    n00b_csp_var_id_t vars[]  = {x, y};
    int64_t           coeffs[] = {1, 1};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 2, 100);
    ASSERT(!ok);

    n00b_csp_store_free(s);
}

TEST(test_linear_negative_coeffs)
{
    // X - Y = 0 (i.e., X = Y), X in [1,5], Y in [3,8]
    // X: needs Y in [3,5] → X in [3,5]
    // Y: needs X in [3,5] → Y in [3,5]
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 5));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(3, 8));

    n00b_csp_var_id_t vars[]  = {x, y};
    int64_t           coeffs[] = {1, -1};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 2, 0);
    ASSERT(ok);

    const n00b_csp_domain_t *dx = n00b_result_get(n00b_csp_var_domain(s, x));
    const n00b_csp_domain_t *dy = n00b_result_get(n00b_csp_var_domain(s, y));
    ASSERT_EQ(n00b_csp_dom_min(dx), 3);
    ASSERT_EQ(n00b_csp_dom_max(dx), 5);
    ASSERT_EQ(n00b_csp_dom_min(dy), 3);
    ASSERT_EQ(n00b_csp_dom_max(dy), 5);

    n00b_csp_store_free(s);
}

TEST(test_linear_with_label)
{
    // X + Y = 7, X in [1,5], Y in [1,5]
    // After propagation: X in [2,5], Y in [2,5]
    // Label should find a solution.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 5));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 5));

    n00b_csp_var_id_t vars[]  = {x, y};
    int64_t           coeffs[] = {1, 1};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 2, 7);
    ASSERT(ok);

    ok = n00b_csp_label(s);
    ASSERT(ok);

    int64_t xv = n00b_result_get(n00b_csp_var_value(s, x));
    int64_t yv = n00b_result_get(n00b_csp_var_value(s, y));
    ASSERT_EQ(xv + yv, 7);

    n00b_csp_store_free(s);
}

TEST(test_linear_three_vars)
{
    // X + Y + Z = 6, all in [1,3]
    // After propagation: each in [1,3] (sum_lo=3, sum_hi=9, rhs=6 in range)
    // Label should find solutions like (1,2,3), (2,2,2), etc.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(1, 3));

    n00b_csp_var_id_t vars[]  = {x, y, z};
    int64_t           coeffs[] = {1, 1, 1};

    bool ok = n00b_csp_post_linear(s, vars, coeffs, 3, 6);
    ASSERT(ok);

    ok = n00b_csp_label(s);
    ASSERT(ok);

    int64_t xv = n00b_result_get(n00b_csp_var_value(s, x));
    int64_t yv = n00b_result_get(n00b_csp_var_value(s, y));
    int64_t zv = n00b_result_get(n00b_csp_var_value(s, z));
    ASSERT_EQ(xv + yv + zv, 6);

    n00b_csp_store_free(s);
}

// ============================================================================
// ALLDIFF constraint
// ============================================================================

TEST(test_alldiff_basic)
{
    // Three vars in [1,3], all different → exactly one solution per permutation.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(1, 3));

    n00b_csp_var_id_t vars[] = {x, y, z};
    bool ok = n00b_csp_post_alldiff(s, vars, 3);
    ASSERT(ok);

    n00b_csp_store_free(s);
}

TEST(test_alldiff_infeasible)
{
    // Three vars in [1,2], all different → only 2 values for 3 vars → fail.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(1, 2));

    n00b_csp_var_id_t vars[] = {x, y, z};
    bool ok = n00b_csp_post_alldiff(s, vars, 3);
    ASSERT(!ok);

    n00b_csp_store_free(s);
}

TEST(test_alldiff_pruning)
{
    // X in {1}, Y in {1,2}, Z in {1,2,3}, all different.
    // X=1 is forced → Régin should prune 1 from Y and Z.
    // Y forced to {2} → Z forced to {3}.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_singleton(1));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 2));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(1, 3));

    n00b_csp_var_id_t vars[] = {x, y, z};
    bool ok = n00b_csp_post_alldiff(s, vars, 3);
    ASSERT(ok);

    // All should be ground after domain-consistent pruning.
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, x)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, x)), 1);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, y)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, y)), 2);
    ASSERT(n00b_result_get(n00b_csp_var_is_ground(s, z)));
    ASSERT_EQ(n00b_result_get(n00b_csp_var_value(s, z)), 3);

    n00b_csp_store_free(s);
}

TEST(test_alldiff_with_label)
{
    // Four vars in [1,4], all different → 4! = 24 solutions.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t a = n00b_csp_new_var(s, *r"A", n00b_csp_dom_range(1, 4));
    n00b_csp_var_id_t b = n00b_csp_new_var(s, *r"B", n00b_csp_dom_range(1, 4));
    n00b_csp_var_id_t c = n00b_csp_new_var(s, *r"C", n00b_csp_dom_range(1, 4));
    n00b_csp_var_id_t d = n00b_csp_new_var(s, *r"D", n00b_csp_dom_range(1, 4));

    n00b_csp_var_id_t vars[] = {a, b, c, d};
    bool ok = n00b_csp_post_alldiff(s, vars, 4);
    ASSERT(ok);

    int64_t count = n00b_csp_label_all(s, nullptr, nullptr);
    ASSERT_EQ(count, 24);

    n00b_csp_store_free(s);
}

TEST(test_alldiff_single_var)
{
    // Edge case: alldiff with 1 var is trivially entailed.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 10));

    n00b_csp_var_id_t vars[] = {x};
    bool ok = n00b_csp_post_alldiff(s, vars, 1);
    ASSERT(ok);

    n00b_csp_store_free(s);
}

TEST(test_alldiff_partial_overlap)
{
    // X in [1,3], Y in [2,4], Z in [3,5], all different.
    // Should be satisfiable. Label and verify.
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 3));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(2, 4));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(3, 5));

    n00b_csp_var_id_t vars[] = {x, y, z};
    bool ok = n00b_csp_post_alldiff(s, vars, 3);
    ASSERT(ok);

    ok = n00b_csp_label(s);
    ASSERT(ok);

    int64_t xv = n00b_result_get(n00b_csp_var_value(s, x));
    int64_t yv = n00b_result_get(n00b_csp_var_value(s, y));
    int64_t zv = n00b_result_get(n00b_csp_var_value(s, z));
    ASSERT(xv != yv && yv != zv && xv != zv);

    n00b_csp_store_free(s);
}

TEST(test_linear_plus_alldiff)
{
    // X + Y + Z = 6, all in [1,4], all different.
    // Valid solutions: permutations of (1,2,3) only (sum=6).
    n00b_csp_store_t *s = n00b_csp_store_new();
    n00b_csp_var_id_t x = n00b_csp_new_var(s, *r"X", n00b_csp_dom_range(1, 4));
    n00b_csp_var_id_t y = n00b_csp_new_var(s, *r"Y", n00b_csp_dom_range(1, 4));
    n00b_csp_var_id_t z = n00b_csp_new_var(s, *r"Z", n00b_csp_dom_range(1, 4));

    n00b_csp_var_id_t vars_lin[]  = {x, y, z};
    int64_t           coeffs[]    = {1, 1, 1};
    n00b_csp_var_id_t vars_diff[] = {x, y, z};

    bool ok = n00b_csp_post_linear(s, vars_lin, coeffs, 3, 6);
    ASSERT(ok);
    ok = n00b_csp_post_alldiff(s, vars_diff, 3);
    ASSERT(ok);

    // Should have exactly 3! = 6 solutions (permutations of 1,2,3).
    int64_t count = n00b_csp_label_all(s, nullptr, nullptr);
    ASSERT_EQ(count, 6);

    n00b_csp_store_free(s);
}

// ============================================================================
// Test runner
// ============================================================================

static void
run_tests(void)
{
    RUN_TEST(test_store_new_free);
    RUN_TEST(test_new_var);
    RUN_TEST(test_find_var);
    RUN_TEST(test_singleton_var);
    RUN_TEST(test_eq_const);
    RUN_TEST(test_eq_const_infeasible);
    RUN_TEST(test_eq_vars);
    RUN_TEST(test_lt);
    RUN_TEST(test_le);
    RUN_TEST(test_ne);
    RUN_TEST(test_in);
    RUN_TEST(test_combined_propagation);
    RUN_TEST(test_chain_propagation);
    RUN_TEST(test_backtracking);
    RUN_TEST(test_nested_backtracking);
    RUN_TEST(test_failure_detection);
    RUN_TEST(test_entailment_after_backtrack);
    RUN_TEST(test_constraint_backtracking);
    RUN_TEST(test_backtrack_with_ne_constraint);
    RUN_TEST(test_linear_basic);
    RUN_TEST(test_linear_narrowing);
    RUN_TEST(test_linear_infeasible);
    RUN_TEST(test_linear_negative_coeffs);
    RUN_TEST(test_linear_with_label);
    RUN_TEST(test_linear_three_vars);
    RUN_TEST(test_alldiff_basic);
    RUN_TEST(test_alldiff_infeasible);
    RUN_TEST(test_alldiff_pruning);
    RUN_TEST(test_alldiff_with_label);
    RUN_TEST(test_alldiff_single_var);
    RUN_TEST(test_alldiff_partial_overlap);
    RUN_TEST(test_linear_plus_alldiff);
}

TEST_MAIN()
