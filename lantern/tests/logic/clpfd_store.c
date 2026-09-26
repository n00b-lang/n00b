#include "logic/clpfd_store.h"
#include "harness.h"
#include "logic/clpfd_label.h"

#define TEST(name) static void name(void)
#define RUN_TEST(name) name()
#define ASSERT(expr) check(#expr, (expr))
#define ASSERT_EQ(a, b) check(#a " == " #b, (a) == (b))

// ============================================================================
// Lifecycle
// ============================================================================

TEST(test_store_new_free) {
  core_csp_store_t *s = core_csp_store_new();
  ASSERT(s != nullptr);
  core_csp_store_free(s);
}

TEST(test_new_var) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 100));

  ASSERT(x != y);

  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  ASSERT(dx != nullptr);
  ASSERT_EQ(core_csp_dom_min(dx), 1);
  ASSERT_EQ(core_csp_dom_max(dx), 10);

  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, y)));

  core_csp_store_free(s);
}

TEST(test_find_var) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));
  core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 10));

  ASSERT(core_option_is_set(core_csp_find_var(s, CORE_STRING_STATIC("X"))));
  ASSERT(core_option_is_set(core_csp_find_var(s, CORE_STRING_STATIC("Y"))));
  ASSERT(!core_option_is_set(core_csp_find_var(s, CORE_STRING_STATIC("Z"))));

  core_csp_store_free(s);
}

TEST(test_singleton_var) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_singleton(42));

  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 42);

  core_csp_store_free(s);
}

// ============================================================================
// Constraint posting and propagation
// ============================================================================

TEST(test_eq_const) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 100));

  bool ok = core_csp_post_eq_const(s, x, 50);
  ASSERT(ok);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 50);

  core_csp_store_free(s);
}

TEST(test_eq_const_infeasible) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));

  bool ok = core_csp_post_eq_const(s, x, 50);
  ASSERT(!ok);

  core_csp_store_free(s);
}

TEST(test_eq_vars) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(5, 20));

  bool ok = core_csp_post_eq(s, x, y);
  ASSERT(ok);

  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  const core_csp_domain_t *dy = core_result_get_ok(core_csp_var_domain(s, y));

  ASSERT_EQ(core_csp_dom_min(dx), 5);
  ASSERT_EQ(core_csp_dom_max(dx), 10);
  ASSERT_EQ(core_csp_dom_min(dy), 5);
  ASSERT_EQ(core_csp_dom_max(dy), 10);

  core_csp_store_free(s);
}

TEST(test_lt) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 100));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 100));

  core_csp_post_eq_const(s, y, 50);
  bool ok = core_csp_post_lt(s, x, y);
  ASSERT(ok);

  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            49);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, y)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, y)), 50);

  core_csp_store_free(s);
}

TEST(test_le) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 100));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 100));

  core_csp_post_eq_const(s, y, 50);
  bool ok = core_csp_post_le(s, x, y);
  ASSERT(ok);
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            50);

  core_csp_store_free(s);
}

TEST(test_ne) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_singleton(2));

  bool ok = core_csp_post_ne(s, x, y);
  ASSERT(ok);

  ASSERT_EQ(
      (int64_t)core_csp_dom_size(core_result_get_ok(core_csp_var_domain(s, x))),
      2);
  ASSERT(
      !core_csp_dom_contains(core_result_get_ok(core_csp_var_domain(s, x)), 2));

  core_csp_store_free(s);
}

TEST(test_in) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 100));

  int64_t vals[] = {10, 20, 30};
  bool ok = core_csp_post_in(s, x, core_csp_dom_from_values(vals, 3));
  ASSERT(ok);
  ASSERT_EQ(
      (int64_t)core_csp_dom_size(core_result_get_ok(core_csp_var_domain(s, x))),
      3);

  core_csp_store_free(s);
}

// ============================================================================
// Combined constraint propagation
// ============================================================================

TEST(test_combined_propagation) {
  // X in [0, 100], Y in [0, 100]
  // X < Y, Y = 50
  // Expected: X in [0, 49], Y = 50
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 100));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 100));

  core_csp_post_lt(s, x, y);
  core_csp_post_eq_const(s, y, 50);

  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            49);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, y)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, y)), 50);

  core_csp_store_free(s);
}

TEST(test_chain_propagation) {
  // X < Y < Z, Z = 5
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 100));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 100));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(0, 100));

  core_csp_post_lt(s, x, y);
  core_csp_post_lt(s, y, z);
  core_csp_post_eq_const(s, z, 5);

  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, y))), 4);
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))), 3);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, z)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, z)), 5);

  core_csp_store_free(s);
}

// ============================================================================
// Backtracking
// ============================================================================

TEST(test_backtracking) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));

  core_csp_push_state(s);
  core_csp_post_eq_const(s, x, 5);

  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 5);

  core_csp_pop_state(s);

  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_csp_dom_min(dx), 1);
  ASSERT_EQ(core_csp_dom_max(dx), 10);

  core_csp_store_free(s);
}

TEST(test_nested_backtracking) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 100));

  core_csp_push_state(s);
  core_csp_post_le(s, x,
                   core_csp_new_var(s, CORE_STRING_STATIC("_t1"),
                                    core_csp_dom_singleton(50)));
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            50);

  core_csp_push_state(s);
  core_csp_post_eq_const(s, x, 25);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));

  core_csp_pop_state(s);
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            50);
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, x)));

  core_csp_pop_state(s);
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            100);

  core_csp_store_free(s);
}

// ============================================================================
// Failure detection
// ============================================================================

TEST(test_failure_detection) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 5));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(10, 20));

  // X = Y should fail (disjoint domains).
  bool ok = core_csp_post_eq(s, x, y);
  ASSERT(!ok);

  core_csp_store_free(s);
}

TEST(test_failure_provenance) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("x"), core_csp_dom_singleton(3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("y"), core_csp_dom_singleton(3));
  int source_marker = 42;

  bool ok = core_csp_post_ne_with_provenance(s, x, y, &source_marker);
  check("failed propagation identifies its source constraint",
        !ok && core_csp_failed_constraint(s) == 0 &&
            core_csp_failed_provenance(s) == &source_marker);

  core_csp_store_free(s);
}

TEST(test_failed_provenance_rolls_back_with_constraint) {
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("x"), core_csp_dom_singleton(3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("y"), core_csp_dom_singleton(3));
  int source_marker = 17;

  core_csp_push_state(s);
  bool ok = core_csp_post_ne_with_provenance(s, x, y, &source_marker);
  check("branch records the failing constraint source",
        !ok && core_csp_failed_provenance(s) == &source_marker);
  core_csp_pop_state(s);
  check("backtracking clears provenance for removed constraints",
        core_csp_failed_constraint(s) == -1 &&
            core_csp_failed_provenance(s) == nullptr && core_csp_propagate(s));

  core_csp_store_free(s);
}

// ============================================================================
// Bug fix regression tests
// ============================================================================

TEST(test_entailment_after_backtrack) {
  // Bug fix 1a regression test:
  // Post EQ, let it become entailed (both ground), pop, verify
  // the constraint re-propagates correctly on the restored domains.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 10));

  // Post X = Y.
  core_csp_post_eq(s, x, y);

  core_csp_push_state(s);

  // Narrow X to singleton 5 -> Y also narrows to 5 -> EQ is entailed.
  core_csp_post_eq_const(s, x, 5);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, y)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 5);
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, y)), 5);

  // Pop: X and Y restored to [1, 10].
  core_csp_pop_state(s);
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, y)));

  // Now narrow X to 3 — Y should also narrow to 3 via the EQ
  // constraint, which must NOT still be marked entailed.
  bool ok = core_csp_post_eq_const(s, x, 3);
  ASSERT(ok);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 3);
  // The EQ constraint should have re-propagated.
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, y)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, y)), 3);

  core_csp_store_free(s);
}

TEST(test_constraint_backtracking) {
  // Bug fix 1b regression test:
  // Push, post constraint, pop — verify the constraint is removed
  // and doesn't affect further propagation.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 100));

  core_csp_push_state(s);

  // Post X = 50 during this branch.
  core_csp_post_eq_const(s, x, 50);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 50);

  // Pop: X restored to [1, 100], and the eq_const constraint is gone.
  core_csp_pop_state(s);
  ASSERT(!core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_csp_dom_min(core_result_get_ok(core_csp_var_domain(s, x))), 1);
  ASSERT_EQ(core_csp_dom_max(core_result_get_ok(core_csp_var_domain(s, x))),
            100);

  // Post X = 75 — should succeed because the old constraint is gone.
  bool ok = core_csp_post_eq_const(s, x, 75);
  ASSERT(ok);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 75);

  core_csp_store_free(s);
}

TEST(test_backtrack_with_ne_constraint) {
  // More complex regression test: NE constraint posted in branch
  // should not persist after pop.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_singleton(2));

  core_csp_push_state(s);

  // Post X != Y: removes 2 from X -> X = {1, 3}.
  core_csp_post_ne(s, x, y);
  ASSERT(
      !core_csp_dom_contains(core_result_get_ok(core_csp_var_domain(s, x)), 2));

  core_csp_pop_state(s);

  // X should be restored to {1, 2, 3}.
  ASSERT_EQ(
      (int64_t)core_csp_dom_size(core_result_get_ok(core_csp_var_domain(s, x))),
      3);
  ASSERT(
      core_csp_dom_contains(core_result_get_ok(core_csp_var_domain(s, x)), 2));

  // Should be able to set X = 2 now (no NE constraint).
  bool ok = core_csp_post_eq_const(s, x, 2);
  ASSERT(ok);
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 2);

  core_csp_store_free(s);
}

// ============================================================================
// LINEAR constraint
// ============================================================================

TEST(test_linear_basic) {
  // X + Y = 10, X in [0,10], Y in [0,10]
  // Should narrow both to [0,10] (unchanged), but establish the constraint.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 10));

  core_csp_var_id_t vars[] = {x, y};
  int64_t coeffs[] = {1, 1};

  bool ok = core_csp_post_linear(s, vars, coeffs, 2, 10);
  ASSERT(ok);

  // Both domains should still include 0..10.
  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  const core_csp_domain_t *dy = core_result_get_ok(core_csp_var_domain(s, y));
  ASSERT_EQ(core_csp_dom_min(dx), 0);
  ASSERT_EQ(core_csp_dom_max(dx), 10);
  ASSERT_EQ(core_csp_dom_min(dy), 0);
  ASSERT_EQ(core_csp_dom_max(dy), 10);

  core_csp_store_free(s);
}

TEST(test_linear_narrowing) {
  // 2*X + 3*Y = 12, X in [0,10], Y in [0,10]
  // X: ceil(12 - 3*10) / 2 .. floor(12 - 3*0) / 2 = ceil(-18/2)..floor(12/2) =
  // 0..6
  //    but also X >= 0, so [0, 6]
  // Y: (12 - 2*6) / 3 .. (12 - 2*0) / 3 = 0/3..12/3 = 0..4
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 10));

  core_csp_var_id_t vars[] = {x, y};
  int64_t coeffs[] = {2, 3};

  bool ok = core_csp_post_linear(s, vars, coeffs, 2, 12);
  ASSERT(ok);

  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  const core_csp_domain_t *dy = core_result_get_ok(core_csp_var_domain(s, y));
  ASSERT_EQ(core_csp_dom_max(dx), 6);
  ASSERT_EQ(core_csp_dom_max(dy), 4);

  core_csp_store_free(s);
}

TEST(test_linear_infeasible) {
  // X + Y = 100, X in [0,10], Y in [0,10]
  // sum_hi = 20 < 100 → fail
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(0, 10));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(0, 10));

  core_csp_var_id_t vars[] = {x, y};
  int64_t coeffs[] = {1, 1};

  bool ok = core_csp_post_linear(s, vars, coeffs, 2, 100);
  ASSERT(!ok);

  core_csp_store_free(s);
}

TEST(test_linear_negative_coeffs) {
  // X - Y = 0 (i.e., X = Y), X in [1,5], Y in [3,8]
  // X: needs Y in [3,5] → X in [3,5]
  // Y: needs X in [3,5] → Y in [3,5]
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 5));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(3, 8));

  core_csp_var_id_t vars[] = {x, y};
  int64_t coeffs[] = {1, -1};

  bool ok = core_csp_post_linear(s, vars, coeffs, 2, 0);
  ASSERT(ok);

  const core_csp_domain_t *dx = core_result_get_ok(core_csp_var_domain(s, x));
  const core_csp_domain_t *dy = core_result_get_ok(core_csp_var_domain(s, y));
  ASSERT_EQ(core_csp_dom_min(dx), 3);
  ASSERT_EQ(core_csp_dom_max(dx), 5);
  ASSERT_EQ(core_csp_dom_min(dy), 3);
  ASSERT_EQ(core_csp_dom_max(dy), 5);

  core_csp_store_free(s);
}

TEST(test_linear_with_label) {
  // X + Y = 7, X in [1,5], Y in [1,5]
  // After propagation: X in [2,5], Y in [2,5]
  // Label should find a solution.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 5));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 5));

  core_csp_var_id_t vars[] = {x, y};
  int64_t coeffs[] = {1, 1};

  bool ok = core_csp_post_linear(s, vars, coeffs, 2, 7);
  ASSERT(ok);

  ok = core_csp_label(s);
  ASSERT(ok);

  int64_t xv = core_result_get_ok(core_csp_var_value(s, x));
  int64_t yv = core_result_get_ok(core_csp_var_value(s, y));
  ASSERT_EQ(xv + yv, 7);

  core_csp_store_free(s);
}

TEST(test_linear_three_vars) {
  // X + Y + Z = 6, all in [1,3]
  // After propagation: each in [1,3] (sum_lo=3, sum_hi=9, rhs=6 in range)
  // Label should find solutions like (1,2,3), (2,2,2), etc.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 3));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(1, 3));

  core_csp_var_id_t vars[] = {x, y, z};
  int64_t coeffs[] = {1, 1, 1};

  bool ok = core_csp_post_linear(s, vars, coeffs, 3, 6);
  ASSERT(ok);

  ok = core_csp_label(s);
  ASSERT(ok);

  int64_t xv = core_result_get_ok(core_csp_var_value(s, x));
  int64_t yv = core_result_get_ok(core_csp_var_value(s, y));
  int64_t zv = core_result_get_ok(core_csp_var_value(s, z));
  ASSERT_EQ(xv + yv + zv, 6);

  core_csp_store_free(s);
}

TEST(test_linear_wide_domains_and_exact_ground_sum) {
  core_csp_store_t *store = core_csp_store_new();
  core_csp_var_id_t wide_vars[] = {
      core_csp_new_var(store, CORE_STRING_STATIC("wide-x"),
                       core_csp_dom_range(INT64_MIN, INT64_MAX)),
      core_csp_new_var(store, CORE_STRING_STATIC("wide-y"),
                       core_csp_dom_range(INT64_MIN, INT64_MAX)),
  };
  int64_t unit_coefficients[] = {1, 1};
  ASSERT(core_csp_post_linear(store, wide_vars, unit_coefficients, 2, 0));
  ASSERT_EQ(core_csp_dom_min(
                core_result_get_ok(core_csp_var_domain(store, wide_vars[0]))),
            -INT64_MAX);
  ASSERT_EQ(core_csp_dom_max(
                core_result_get_ok(core_csp_var_domain(store, wide_vars[1]))),
            INT64_MAX);
  ASSERT_EQ(core_csp_dom_min(
                core_result_get_ok(core_csp_var_domain(store, wide_vars[1]))),
            -INT64_MAX);
  ASSERT_EQ(core_csp_dom_max(
                core_result_get_ok(core_csp_var_domain(store, wide_vars[0]))),
            INT64_MAX);
  core_csp_store_free(store);

  store = core_csp_store_new();
  core_csp_var_id_t cancel_vars[] = {
      core_csp_new_var(store, CORE_STRING_STATIC("cancel-x"),
                       core_csp_dom_singleton(INT64_MAX)),
      core_csp_new_var(store, CORE_STRING_STATIC("cancel-y"),
                       core_csp_dom_singleton(INT64_MAX)),
  };
  int64_t cancel_coefficients[] = {INT64_MAX, -INT64_MAX};
  ASSERT(core_csp_post_linear(store, cancel_vars, cancel_coefficients, 2, 0));
  core_csp_store_free(store);

  store = core_csp_store_new();
  core_csp_var_id_t overflow_vars[] = {
      core_csp_new_var(store, CORE_STRING_STATIC("overflow-x"),
                       core_csp_dom_singleton(1)),
      core_csp_new_var(store, CORE_STRING_STATIC("overflow-y"),
                       core_csp_dom_singleton(1)),
  };
  int64_t overflow_coefficients[] = {INT64_MAX, INT64_MAX};
  ASSERT(!core_csp_post_linear(store, overflow_vars, overflow_coefficients,
                               2, INT64_MAX));
  core_csp_store_free(store);

  store = core_csp_store_new();
  core_csp_var_id_t min_coefficient_var = core_csp_new_var(
      store, CORE_STRING_STATIC("min-coefficient"), core_csp_dom_singleton(1));
  core_csp_var_id_t min_coefficient_vars[] = {min_coefficient_var};
  int64_t min_coefficient[] = {INT64_MIN};
  ASSERT(core_csp_post_linear(store, min_coefficient_vars, min_coefficient, 1,
                              INT64_MIN));
  core_csp_store_free(store);
}

TEST(test_linear_matches_small_integer_enumeration) {
  bool matches = true;
  for (int64_t a = -3; a <= 3 && matches; a++) {
    for (int64_t b = -3; b <= 3 && matches; b++) {
      for (int64_t rhs = -8; rhs <= 8 && matches; rhs++) {
        int64_t expected = 0;
        for (int64_t x = -2; x <= 2; x++)
          for (int64_t y = -2; y <= 2; y++)
            expected += a * x + b * y == rhs;

        core_csp_store_t *store = core_csp_store_new();
        core_csp_var_id_t vars[] = {
            core_csp_new_var(store, CORE_STRING_STATIC("x"),
                             core_csp_dom_range(-2, 2)),
            core_csp_new_var(store, CORE_STRING_STATIC("y"),
                             core_csp_dom_range(-2, 2)),
        };
        int64_t coefficients[] = {a, b};
        bool posted = core_csp_post_linear(store, vars, coefficients, 2, rhs);
        int64_t found = posted ? core_csp_label_all(store, nullptr, nullptr) : 0;
        matches = found == expected;
        core_csp_store_free(store);
      }
    }
  }
  ASSERT(matches);
}

// ============================================================================
// ALLDIFF constraint
// ============================================================================

TEST(test_alldiff_basic) {
  // Three vars in [1,3], all different → exactly one solution per permutation.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 3));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(1, 3));

  core_csp_var_id_t vars[] = {x, y, z};
  bool ok = core_csp_post_alldiff(s, vars, 3);
  ASSERT(ok);

  core_csp_store_free(s);
}

TEST(test_alldiff_infeasible) {
  // Three vars in [1,2], all different → only 2 values for 3 vars → fail.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 2));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 2));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(1, 2));

  core_csp_var_id_t vars[] = {x, y, z};
  bool ok = core_csp_post_alldiff(s, vars, 3);
  ASSERT(!ok);

  core_csp_store_free(s);
}

TEST(test_alldiff_pruning) {
  // X in {1}, Y in {1,2}, Z in {1,2,3}, all different.
  // X=1 is forced → Régin should prune 1 from Y and Z.
  // Y forced to {2} → Z forced to {3}.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_singleton(1));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 2));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(1, 3));

  core_csp_var_id_t vars[] = {x, y, z};
  bool ok = core_csp_post_alldiff(s, vars, 3);
  ASSERT(ok);

  // All should be ground after domain-consistent pruning.
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, x)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, x)), 1);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, y)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, y)), 2);
  ASSERT(core_result_get_ok(core_csp_var_is_ground(s, z)));
  ASSERT_EQ(core_result_get_ok(core_csp_var_value(s, z)), 3);

  core_csp_store_free(s);
}

TEST(test_alldiff_with_label) {
  // Four vars in [1,4], all different → 4! = 24 solutions.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t a =
      core_csp_new_var(s, CORE_STRING_STATIC("A"), core_csp_dom_range(1, 4));
  core_csp_var_id_t b =
      core_csp_new_var(s, CORE_STRING_STATIC("B"), core_csp_dom_range(1, 4));
  core_csp_var_id_t c =
      core_csp_new_var(s, CORE_STRING_STATIC("C"), core_csp_dom_range(1, 4));
  core_csp_var_id_t d =
      core_csp_new_var(s, CORE_STRING_STATIC("D"), core_csp_dom_range(1, 4));

  core_csp_var_id_t vars[] = {a, b, c, d};
  bool ok = core_csp_post_alldiff(s, vars, 4);
  ASSERT(ok);

  int64_t count = core_csp_label_all(s, nullptr, nullptr);
  ASSERT_EQ(count, 24);

  core_csp_store_free(s);
}

TEST(test_alldiff_single_var) {
  // Edge case: alldiff with 1 var is trivially entailed.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 10));

  core_csp_var_id_t vars[] = {x};
  bool ok = core_csp_post_alldiff(s, vars, 1);
  ASSERT(ok);

  core_csp_store_free(s);
}

TEST(test_alldiff_defers_broad_intervals) {
  core_csp_store_t *store = core_csp_store_new();
  core_csp_var_id_t vars[] = {
      core_csp_new_var(store, CORE_STRING_STATIC("wide-a"),
                       core_csp_dom_range(INT64_MIN, INT64_MAX)),
      core_csp_new_var(store, CORE_STRING_STATIC("wide-b"),
                       core_csp_dom_range(INT64_MIN, INT64_MAX)),
  };
  ASSERT(core_csp_post_alldiff(store, vars, 2));
  ASSERT_EQ(core_csp_dom_size(
                core_result_get_ok(core_csp_var_domain(store, vars[0]))),
            INT64_MAX);
  ASSERT_EQ(core_csp_dom_size(
                core_result_get_ok(core_csp_var_domain(store, vars[1]))),
            INT64_MAX);
  core_csp_store_free(store);

  store = core_csp_store_new();
  core_csp_var_id_t wide = core_csp_new_var(
      store, CORE_STRING_STATIC("wide"),
      core_csp_dom_range(INT64_MIN, INT64_MAX));
  core_csp_var_id_t empty = core_csp_new_var(
      store, CORE_STRING_STATIC("empty"), core_csp_dom_empty());
  core_csp_var_id_t wide_and_empty[] = {wide, empty};
  ASSERT(!core_csp_post_alldiff(store, wide_and_empty, 2));
  core_csp_store_free(store);
}

TEST(test_alldiff_partial_overlap) {
  // X in [1,3], Y in [2,4], Z in [3,5], all different.
  // Should be satisfiable. Label and verify.
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 3));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(2, 4));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(3, 5));

  core_csp_var_id_t vars[] = {x, y, z};
  bool ok = core_csp_post_alldiff(s, vars, 3);
  ASSERT(ok);

  ok = core_csp_label(s);
  ASSERT(ok);

  int64_t xv = core_result_get_ok(core_csp_var_value(s, x));
  int64_t yv = core_result_get_ok(core_csp_var_value(s, y));
  int64_t zv = core_result_get_ok(core_csp_var_value(s, z));
  ASSERT(xv != yv && yv != zv && xv != zv);

  core_csp_store_free(s);
}

TEST(test_linear_plus_alldiff) {
  // X + Y + Z = 6, all in [1,4], all different.
  // Valid solutions: permutations of (1,2,3) only (sum=6).
  core_csp_store_t *s = core_csp_store_new();
  core_csp_var_id_t x =
      core_csp_new_var(s, CORE_STRING_STATIC("X"), core_csp_dom_range(1, 4));
  core_csp_var_id_t y =
      core_csp_new_var(s, CORE_STRING_STATIC("Y"), core_csp_dom_range(1, 4));
  core_csp_var_id_t z =
      core_csp_new_var(s, CORE_STRING_STATIC("Z"), core_csp_dom_range(1, 4));

  core_csp_var_id_t vars_lin[] = {x, y, z};
  int64_t coeffs[] = {1, 1, 1};
  core_csp_var_id_t vars_diff[] = {x, y, z};

  bool ok = core_csp_post_linear(s, vars_lin, coeffs, 3, 6);
  ASSERT(ok);
  ok = core_csp_post_alldiff(s, vars_diff, 3);
  ASSERT(ok);

  // Should have exactly 3! = 6 solutions (permutations of 1,2,3).
  int64_t count = core_csp_label_all(s, nullptr, nullptr);
  ASSERT_EQ(count, 6);

  core_csp_store_free(s);
}

typedef struct {
  int64_t count;
  int64_t sum;
} labeled_values_t;

static bool count_labeled_value(core_csp_store_t *store, void *context) {
  labeled_values_t *values = context;
  core_csp_int_result_t result = core_csp_var_value(store, 0);
  if (!result.is_ok)
    return false;
  values->count++;
  values->sum += core_result_get_ok(result);
  return true;
}

TEST(test_label_all_visits_domains_over_64_values) {
  core_csp_store_t *store = core_csp_store_new();
  core_csp_new_var(store, CORE_STRING_STATIC("wide"),
                   core_csp_dom_range(1, 70));
  labeled_values_t values = {0};
  ASSERT_EQ(core_csp_label_all(store, count_labeled_value, &values), 70);
  ASSERT_EQ(values.count, 70);
  ASSERT_EQ(values.sum, 2485);
  ASSERT_EQ(
      core_csp_dom_size(core_result_get_ok(core_csp_var_domain(store, 0))), 70);
  core_csp_store_free(store);
}

// ============================================================================
// Test runner
// ============================================================================

static void run_tests(void) {
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
  RUN_TEST(test_failure_provenance);
  RUN_TEST(test_failed_provenance_rolls_back_with_constraint);
  RUN_TEST(test_entailment_after_backtrack);
  RUN_TEST(test_constraint_backtracking);
  RUN_TEST(test_backtrack_with_ne_constraint);
  RUN_TEST(test_linear_basic);
  RUN_TEST(test_linear_narrowing);
  RUN_TEST(test_linear_infeasible);
  RUN_TEST(test_linear_negative_coeffs);
  RUN_TEST(test_linear_with_label);
  RUN_TEST(test_linear_three_vars);
  RUN_TEST(test_linear_wide_domains_and_exact_ground_sum);
  RUN_TEST(test_linear_matches_small_integer_enumeration);
  RUN_TEST(test_alldiff_basic);
  RUN_TEST(test_alldiff_infeasible);
  RUN_TEST(test_alldiff_pruning);
  RUN_TEST(test_alldiff_with_label);
  RUN_TEST(test_alldiff_single_var);
  RUN_TEST(test_alldiff_defers_broad_intervals);
  RUN_TEST(test_alldiff_partial_overlap);
  RUN_TEST(test_linear_plus_alldiff);
  RUN_TEST(test_label_all_visits_domains_over_64_values);
}

int main(void) {
  run_tests();
  return report("CSP store");
}
