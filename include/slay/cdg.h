#pragma once

/**
 * @file cdg.h
 * @brief Control dependence graph from a CFG via post-dominator tree.
 *
 * Computes the post-dominator tree using the Cooper/Harvey/Kennedy
 * iterative fixpoint algorithm, then derives control dependence edges.
 * A block B is control-dependent on block A when A's branch decision
 * determines whether B executes.
 *
 * @see cfg.h  for the source CFG
 */

#include "slay/cfg.h"
#include "core/array.h"

// ============================================================================
// Post-dominator info
// ============================================================================

/**
 * @brief Per-block post-dominator information.
 *
 * Stored in an array indexed by block ID.
 */
typedef struct {
    int32_t idom;   /**< Immediate post-dominator block ID (-1 = none). */
    int32_t depth;  /**< Depth in post-dominator tree (exit = 0). */
} n00b_pdom_info_t;

n00b_array_decl(n00b_pdom_info_t);

// ============================================================================
// Control dependence edge
// ============================================================================

/**
 * @brief A single control dependence edge.
 *
 * Says that @c dependent_id only executes because of a branch
 * decision made by @c controller_id, along the CFG edge of
 * kind @c edge_kind.
 */
typedef struct {
    int32_t              controller_id;  /**< Block whose branch decides. */
    int32_t              dependent_id;   /**< Block that depends on it. */
    n00b_cfg_edge_kind_t edge_kind;      /**< CFG edge kind (true/false/case/...). */
} n00b_cd_edge_t;

n00b_list_decl(n00b_cd_edge_t);

// ============================================================================
// CDG
// ============================================================================

/**
 * @brief Control dependence graph.
 *
 * Built from a CFG via `n00b_build_cdg()`.  The CFG pointer is
 * borrowed (not owned); the caller must keep it alive while the
 * CDG is in use.
 */
typedef struct {
    n00b_cfg_t                     *cfg;       /**< Source CFG (not owned). */
    n00b_array_t(n00b_pdom_info_t)  pdom;      /**< Indexed by block ID. */
    n00b_list_t(n00b_cd_edge_t)     cd_edges;  /**< All CD edges. */
    n00b_string_t                   name;      /**< Name (from CFG). */
} n00b_cdg_t;

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Build a control dependence graph from a CFG.
 *
 * Computes the post-dominator tree via iterative fixpoint, then
 * derives control dependence edges.
 *
 * @param cfg  Source CFG (must not be NULL).
 * @return Heap-allocated CDG, or NULL on error.
 *
 * @pre @p cfg was built by `n00b_build_cfg()`.
 */
n00b_cdg_t *n00b_build_cdg(n00b_cfg_t *cfg);

// ============================================================================
// Query — inline
// ============================================================================

/** @brief Immediate post-dominator of @p block_id (-1 if none). */
static inline int32_t
n00b_cdg_idom(n00b_cdg_t *cdg, int32_t block_id)
{
    if (!cdg || block_id < 0
        || (size_t)block_id >= n00b_array_len(cdg->pdom)) {
        return -1;
    }

    return n00b_array_get(cdg->pdom, block_id).idom;
}

/**
 * @brief Does block @p a post-dominate block @p b?
 *
 * Walks the idom chain from @p b; returns true if @p a is found.
 */
static inline bool
n00b_cdg_postdominates(n00b_cdg_t *cdg, int32_t a, int32_t b)
{
    if (!cdg || a < 0 || b < 0) {
        return false;
    }

    size_t nb = n00b_array_len(cdg->pdom);

    if ((size_t)a >= nb || (size_t)b >= nb) {
        return false;
    }

    int32_t cur = b;

    while (cur >= 0 && (size_t)cur < nb) {
        if (cur == a) {
            return true;
        }

        int32_t next = cdg->pdom.data[cur].idom;

        if (next == cur) {
            break;  // Exit block (idom[exit] = exit).
        }

        cur = next;
    }

    return false;
}

/** @brief Total number of control dependence edges. */
static inline int32_t
n00b_cdg_cd_count(n00b_cdg_t *cdg)
{
    return cdg ? (int32_t)n00b_list_len(cdg->cd_edges) : 0;
}

// ============================================================================
// Query — non-inline
// ============================================================================

/**
 * @brief Collect CD edges where @p block_id is the dependent (who controls me?).
 *
 * @param cdg       The CDG.
 * @param block_id  Dependent block.
 * @return List of CD edges (caller should free).
 */
n00b_list_t(n00b_cd_edge_t)
n00b_cdg_controllers(n00b_cdg_t *cdg, int32_t block_id);

/**
 * @brief Collect CD edges where @p block_id is the controller (who do I control?).
 *
 * @param cdg       The CDG.
 * @param block_id  Controller block.
 * @return List of CD edges (caller should free).
 */
n00b_list_t(n00b_cd_edge_t)
n00b_cdg_dependents(n00b_cdg_t *cdg, int32_t block_id);

// ============================================================================
// Cleanup
// ============================================================================

/**
 * @brief Free a CDG.
 *
 * Frees the pdom array and cd_edges list but NOT the borrowed CFG pointer.
 */
void n00b_cdg_free(n00b_cdg_t *cdg);
