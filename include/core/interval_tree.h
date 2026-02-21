/**
 * @file interval_tree.h
 * @brief Augmented AVL interval tree.
 *
 * Supports insertion, deletion, point/range queries, and ordered
 * traversal of [low, high] intervals.  Used by the mmap subsystem
 * for fast address-range lookups.
 */
#pragma once
#include <stdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/stack.h"
#include "core/result.h"
#include "core/data_lock.h"

// ============================================================================
// Error codes
// ============================================================================

#define N00B_INTERVAL_OK            0
#define N00B_INTERVAL_ERR_INVALID   1
#define N00B_INTERVAL_ERR_NOT_FOUND 2

// ============================================================================
// Stack type used internally and for search results
// ============================================================================

n00b_stack_decl(void *);

// ============================================================================
// Types
// ============================================================================

typedef struct n00b_interval_node_t {
    uint64_t                     low;
    uint64_t                     high;
    uint64_t                     height;
    uint64_t                     maximum;
    uint64_t                     minimum;
    struct n00b_interval_node_t *left;
    struct n00b_interval_node_t *right;
    void                        *data;
} n00b_interval_node_t;

typedef struct n00b_interval_tree_t {
    n00b_interval_node_t *root;
    n00b_stack_t(void *)  stack;
    n00b_allocator_t     *allocator;
    n00b_rwlock_t        *lock;
} n00b_interval_tree_t;

// ============================================================================
// Result types for the interval tree API
// ============================================================================

n00b_result_decl(n00b_interval_node_t *);

// ============================================================================
// API
// ============================================================================

/**
 * @brief Initialize an interval tree structure.
 * @param tree Tree to initialize.
 *
 * @kw allocator Allocator for internal node storage (nullptr = runtime default).
 */
extern void
n00b_interval_tree_init(n00b_interval_tree_t *tree) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Insert an interval [low, high] with associated data.
 * @param tree Tree to insert into.
 * @param low  Interval lower bound.
 * @param high Interval upper bound.
 * @param data User data to associate with the interval.
 * @return     Result containing the new node, or an error code.
 * @pre @p low <= @p high.
 */
n00b_result_t(n00b_interval_node_t *)
    n00b_interval_insert(n00b_interval_tree_t *tree,
                         uint64_t              low,
                         uint64_t              high,
                         void                 *data);

/**
 * @brief Delete a specific node from the tree.
 * @param tree   Tree to delete from.
 * @param target Node to remove.
 * @return       Result containing 0 on success, or an error code.
 */
n00b_result_t(int)
    n00b_interval_delete(n00b_interval_tree_t *tree,
                         n00b_interval_node_t *target);

/**
 * @brief Find any single interval overlapping [low, high].
 * @param tree Tree to search.
 * @param low  Query lower bound.
 * @param high Query upper bound.
 * @return     Result containing an overlapping node, or not-found error.
 */
n00b_result_t(n00b_interval_node_t *)
    n00b_interval_search_any(n00b_interval_tree_t *tree,
                             uint64_t              low,
                             uint64_t              high);

/**
 * @brief Find all intervals overlapping [low, high].
 * @param tree          Tree to search.
 * @param low           Query lower bound.
 * @param high          Query upper bound.
 * @param intersections Stack to push matching nodes onto.
 * @return              Result containing the count, or an error code.
 */
n00b_result_t(int)
    n00b_interval_search(n00b_interval_tree_t *tree,
                         uint64_t              low,
                         uint64_t              high,
                         n00b_stack_t(void *)  *intersections);

/**
 * @brief Find all intervals overlapping [low, high], sorted by low bound.
 * @param tree          Tree to search.
 * @param low           Query lower bound.
 * @param high          Query upper bound.
 * @param intersections Stack to push matching nodes onto (ordered).
 * @return              Result containing the count, or an error code.
 */
n00b_result_t(int)
    n00b_interval_search_ordered(n00b_interval_tree_t *tree,
                                 uint64_t              low,
                                 uint64_t              high,
                                 n00b_stack_t(void *)  *intersections);

/**
 * @brief Get the global maximum endpoint across all intervals.
 * @param tree Tree to query.
 * @return     Result containing the maximum, or error if tree is empty.
 */
n00b_result_t(uint64_t)
    n00b_interval_max(n00b_interval_tree_t *tree);

/**
 * @brief Get the global minimum endpoint across all intervals.
 * @param tree Tree to query.
 * @return     Result containing the minimum, or error if tree is empty.
 */
n00b_result_t(uint64_t)
    n00b_interval_min(n00b_interval_tree_t *tree);

/**
 * @brief Find the interval whose low bound is the smallest value > @p point.
 * @param tree  Tree to search.
 * @param point Reference point.
 * @return      Result containing the next node, or not-found error.
 */
n00b_result_t(n00b_interval_node_t *)
    n00b_interval_next_low(n00b_interval_tree_t *tree,
                           uint64_t              point);
