/**
 * @file clpfd_label.h
 * @brief Backtracking search (labeling) for the CSP solver.
 *
 * Extends the AC-3 constraint propagation in `clpfd_store` with
 * depth-first search using the MRV (minimum remaining values)
 * heuristic.  After propagation narrows domains, labeling tries
 * concrete assignments and backtracks on failure until a complete
 * solution is found.
 */
#pragma once

#include "logic/clpfd_store.h"

/**
 * @brief Callback invoked for each solution found by n00b_csp_label_all().
 *
 * @param s   Store in a solved state (all variables ground).
 * @param ctx User context pointer.
 * @return    true to continue searching, false to stop.
 */
typedef bool (*n00b_csp_solution_cb)(n00b_csp_store_t *s, void *ctx);

/**
 * @brief Find the first complete assignment via backtracking search.
 *
 * Uses DFS with MRV (minimum remaining values) variable selection.
 * On success the store is left in a solved state with all variables
 * ground.  On failure the store is restored to its pre-call state.
 *
 * @param s Store (must have propagation already run).
 * @return  true if a solution was found.
 */
bool n00b_csp_label(n00b_csp_store_t *s);

/**
 * @brief Enumerate all solutions via backtracking search.
 *
 * Calls @p cb for each complete assignment found.  If @p cb returns
 * false, enumeration stops early.  The store is always restored to
 * its pre-call state when this function returns.
 *
 * @param s   Store (must have propagation already run).
 * @param cb  Callback for each solution.
 * @param ctx User context passed to cb.
 * @return    Number of solutions found (may be 0).
 */
int64_t n00b_csp_label_all(n00b_csp_store_t *s, n00b_csp_solution_cb cb,
                             void *ctx);
