/**
 * @file clpfd_store.h
 * @brief Constraint store API for the constraint satisfaction solver.
 *
 * Manages constraint variables, constraint posting, AC-3 propagation,
 * and trail-based backtracking. Choice points record both the trail
 * position and constraint count, so constraints posted during a
 * branch are properly removed on backtrack.
 */
#pragma once

#include "logic/clpfd_domain.h"
#include "core/option.h"
#include "core/result.h"

n00b_option_decl(n00b_csp_var_id_t);
n00b_result_decl(const n00b_csp_domain_t *);

/**
 * @brief Opaque constraint store.
 */
typedef struct n00b_csp_store n00b_csp_store_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * @brief Create a new constraint store.
 * @return Heap-allocated store. Must be freed with n00b_csp_store_free().
 */
n00b_csp_store_t *n00b_csp_store_new(void);

/**
 * @brief Free a constraint store and all its resources.
 * @param store Store to free (may be nullptr).
 */
void n00b_csp_store_free(n00b_csp_store_t *store);

// ============================================================================
// Variables
// ============================================================================

/**
 * @brief Create a new constraint variable with the given domain.
 *
 * @param s     Store.
 * @param name  Variable name.
 * @param dom   Initial domain (consumed; caller should not free).
 * @return      Variable ID.
 */
n00b_csp_var_id_t n00b_csp_new_var(n00b_csp_store_t *s, n00b_string_t name,
                                    n00b_csp_domain_t dom);

/**
 * @brief Find a variable by name.
 * @param s    Store.
 * @param name Name to search for.
 * @return     Option containing variable ID, or none if not found.
 */
n00b_option_t(n00b_csp_var_id_t) n00b_csp_find_var(n00b_csp_store_t *s,
                                                     n00b_string_t     name);

/**
 * @brief Get a variable's current domain.
 * @param s Store.
 * @param v Variable ID.
 * @return  Result containing pointer to the domain, or EINVAL error.
 */
n00b_result_t(const n00b_csp_domain_t *) n00b_csp_var_domain(
    n00b_csp_store_t *s, n00b_csp_var_id_t v);

/**
 * @brief Test whether a variable is ground (singleton domain).
 * @param s Store.
 * @param v Variable ID.
 * @return  Result containing bool, or EINVAL error.
 */
n00b_result_t(bool) n00b_csp_var_is_ground(n00b_csp_store_t *s,
                                            n00b_csp_var_id_t v);

/**
 * @brief Get the value of a ground variable.
 * @param s Store.
 * @param v Variable ID.
 * @return  Result containing value, or EINVAL error.
 * @pre     Variable is ground.
 */
n00b_result_t(int64_t) n00b_csp_var_value(n00b_csp_store_t *s,
                                           n00b_csp_var_id_t v);

/**
 * @brief Get the number of variables in the store.
 */
int32_t n00b_csp_var_count(const n00b_csp_store_t *s);

/**
 * @brief Callback for domain iteration.
 * @param value Current domain value.
 * @param ctx   User context pointer.
 * @return      true to continue, false to stop.
 */
typedef bool (*n00b_csp_dom_iter_cb)(int64_t value, void *ctx);

/**
 * @brief Iterate over all values in a variable's domain in ascending order.
 *
 * Handles all three domain representations (interval, bitset, sparse).
 *
 * @param s  Store.
 * @param v  Variable ID.
 * @param cb Callback invoked for each value.
 * @param ctx User context passed to cb.
 * @return   Count of values iterated, or -1 on bad variable ID.
 */
int32_t n00b_csp_dom_iterate(n00b_csp_store_t *s, n00b_csp_var_id_t v,
                               n00b_csp_dom_iter_cb cb, void *ctx);

// ============================================================================
// Constraint posting (returns false if immediately unsatisfiable)
// ============================================================================

/** @brief Post X = Y. */
bool n00b_csp_post_eq(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                       n00b_csp_var_id_t y);

/** @brief Post X = c (constant). */
bool n00b_csp_post_eq_const(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                             int64_t c);

/** @brief Post X != Y. */
bool n00b_csp_post_ne(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                       n00b_csp_var_id_t y);

/** @brief Post X < Y. */
bool n00b_csp_post_lt(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                       n00b_csp_var_id_t y);

/** @brief Post X <= Y. */
bool n00b_csp_post_le(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                       n00b_csp_var_id_t y);

/** @brief Post X in D (domain membership). */
bool n00b_csp_post_in(n00b_csp_store_t *s, n00b_csp_var_id_t x,
                       n00b_csp_domain_t dom);

/**
 * @brief Post a linear constraint: coeffs[0]*vars[0] + ... + coeffs[n-1]*vars[n-1] = rhs.
 *
 * Uses bounds propagation to narrow variable domains.
 *
 * @param s      Store.
 * @param vars   Array of n variable IDs.
 * @param coeffs Array of n integer coefficients.
 * @param n      Number of terms.
 * @param rhs    Right-hand-side constant.
 * @return       false if immediately unsatisfiable.
 */
bool n00b_csp_post_linear(n00b_csp_store_t *s, const n00b_csp_var_id_t *vars,
                            const int64_t *coeffs, int32_t n, int64_t rhs);

/**
 * @brief Post an all-different constraint on n variables.
 *
 * Uses Régin's algorithm (maximum bipartite matching + SCC decomposition)
 * for domain-consistent filtering.
 *
 * @param s    Store.
 * @param vars Array of n variable IDs.
 * @param n    Number of variables.
 * @return     false if immediately unsatisfiable.
 */
bool n00b_csp_post_alldiff(n00b_csp_store_t *s, const n00b_csp_var_id_t *vars,
                             int32_t n);

// ============================================================================
// Propagation
// ============================================================================

/**
 * @brief Run AC-3 propagation to fixpoint.
 * @return false if any domain became empty (unsatisfiable).
 */
bool n00b_csp_propagate(n00b_csp_store_t *s);

// ============================================================================
// Backtracking
// ============================================================================

/**
 * @brief Save the current store state (push a choice point).
 *
 * Records both the trail position and the constraint count, so
 * constraints posted after this point are removed on pop.
 */
void n00b_csp_push_state(n00b_csp_store_t *s);

/**
 * @brief Restore the store to the last saved state (pop a choice point).
 *
 * Undoes trail entries, removes constraints added since the push,
 * cleans up their watchlist entries, and resets entailment flags for
 * constraints whose watched variables were trailed.
 */
void n00b_csp_pop_state(n00b_csp_store_t *s);
