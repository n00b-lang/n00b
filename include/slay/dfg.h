#pragma once

/**
 * @file dfg.h
 * @brief Data flow graph: reaching definitions analysis.
 *
 * Builds def/use facts from a CFG with CF labels (`@assigns`, `@varref`),
 * then computes reaching definitions via iterative worklist to produce
 * data dependence (DD) edges.  A DD edge connects a variable definition
 * to each use that the definition can reach.
 *
 * @see cfg.h      for the source CFG
 * @see cf_label.h for the label types (N00B_CF_ASSIGNS, N00B_CF_VARREF)
 */

#include "slay/cfg.h"
#include "slay/cf_label.h"
#include "slay/grammar.h"
#include "core/list.h"

// ============================================================================
// Def/use fact
// ============================================================================

/**
 * @brief A single definition or use of a variable.
 *
 * Extracted from CFG blocks by walking statements and their CF labels.
 */
typedef struct {
    n00b_string_t      var_name;   /**< Variable name. */
    n00b_parse_tree_t *node;       /**< Parse tree node of the def/use. */
    int32_t            block_id;   /**< CFG block containing this. */
    int32_t            stmt_ix;    /**< Index within block's stmts list. */
    int32_t            id;         /**< Index of this fact in dfg->facts. */
    bool               is_def;     /**< true = def, false = use. */
} n00b_du_fact_t;

n00b_list_decl(n00b_du_fact_t);

// ============================================================================
// Data dependence edge
// ============================================================================

/**
 * @brief A data dependence edge: a reaching definition flows to a use.
 */
typedef struct {
    int32_t def_id;  /**< Index into dfg->facts for the def. */
    int32_t use_id;  /**< Index into dfg->facts for the use. */
} n00b_dd_edge_t;

n00b_list_decl(n00b_dd_edge_t);

// ============================================================================
// Data flow graph
// ============================================================================

/**
 * @brief Data flow graph.
 *
 * Built from a CFG + CF labels via `n00b_build_dfg()`.  The CFG and
 * cf_labels pointers are borrowed (not owned); the caller must keep
 * them alive while the DFG is in use.
 */
typedef struct {
    n00b_cfg_t                  *cfg;       /**< Source CFG (not owned). */
    n00b_cf_labels_t            *cf_labels; /**< CF labels (not owned). */
    n00b_list_t(n00b_du_fact_t)  facts;     /**< All def/use facts. */
    n00b_list_t(n00b_dd_edge_t)  edges;     /**< All DD edges. */
    n00b_string_t                name;      /**< Name (from CFG). */
} n00b_dfg_t;

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Build a data flow graph from a CFG and its CF labels.
 *
 * Extracts def/use facts from statements, computes reaching definitions
 * via iterative worklist, and emits DD edges.  If @p annot is provided
 * and contains parameter symbols, synthetic DEF facts are emitted at
 * the CFG entry block for each parameter.
 *
 * @param cfg        Source CFG (must not be NULL).
 * @param cf_labels  CF label map from `n00b_annot_walk_tree_full`.
 * @param grammar    Grammar for keyword filtering (may be NULL).
 * @param annot      Annotation walk result (may be NULL).
 * @return Heap-allocated DFG, or NULL on error.
 *
 * @pre @p cfg was built by `n00b_build_cfg()`.
 * @pre @p cf_labels was produced by walking the same tree.
 */
n00b_dfg_t *n00b_build_dfg(n00b_cfg_t          *cfg,
                             n00b_cf_labels_t    *cf_labels,
                             n00b_grammar_t      *grammar,
                             n00b_annot_result_t *annot);

// ============================================================================
// Query — inline
// ============================================================================

/** @brief Number of def/use facts. */
static inline int32_t
n00b_dfg_fact_count(n00b_dfg_t *dfg)
{
    return dfg ? (int32_t)n00b_list_len(dfg->facts) : 0;
}

/** @brief Number of DD edges. */
static inline int32_t
n00b_dfg_edge_count(n00b_dfg_t *dfg)
{
    return dfg ? (int32_t)n00b_list_len(dfg->edges) : 0;
}

// ============================================================================
// Query — non-inline
// ============================================================================

/**
 * @brief Collect DD edges where @p use_id is the use (what defs reach me?).
 *
 * @param dfg     The DFG.
 * @param use_id  Fact index of the use.
 * @return List of DD edges (caller should free).
 */
n00b_list_t(n00b_dd_edge_t)
n00b_dfg_reaching_defs(n00b_dfg_t *dfg, int32_t use_id);

/**
 * @brief Collect DD edges where @p def_id is the def (what uses do I reach?).
 *
 * @param dfg     The DFG.
 * @param def_id  Fact index of the def.
 * @return List of DD edges (caller should free).
 */
n00b_list_t(n00b_dd_edge_t)
n00b_dfg_reached_uses(n00b_dfg_t *dfg, int32_t def_id);

// ============================================================================
// Cleanup
// ============================================================================

/**
 * @brief Free a DFG.
 *
 * Frees the facts and edges lists but NOT the borrowed CFG/cf_labels.
 */
void n00b_dfg_free(n00b_dfg_t *dfg);
