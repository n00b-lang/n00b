#pragma once

/**
 * @file cfg.h
 * @brief Control flow graph construction from annotated parse trees.
 *
 * Builds a CFG from a parse tree whose grammar has control flow
 * annotations (`@branch`, `@loop`, `@switch`, `@jump`).  The annotation
 * walk (`n00b_annot_walk_tree_full`) must be run first to produce the
 * `n00b_dict_untyped_t` label map consumed here.
 *
 * @see cf_label.h  for the label types
 * @see annot_walk.h  for the annotation walk
 */

#include "slay/cf_label.h"
#include "core/list.h"

// ============================================================================
// Edge kinds
// ============================================================================

typedef enum {
    N00B_CFG_FALLTHROUGH,   /**< Sequential flow to next block. */
    N00B_CFG_BRANCH_TRUE,   /**< Condition true (then-arm, loop body). */
    N00B_CFG_BRANCH_FALSE,  /**< Condition false (else-arm, skip). */
    N00B_CFG_LOOP_BACK,     /**< Back-edge: loop body end -> header. */
    N00B_CFG_LOOP_EXIT,     /**< Loop exit when condition is false. */
    N00B_CFG_JUMP,          /**< Unconditional (break/continue/return). */
    N00B_CFG_CASE_BRANCH,   /**< N-way switch case arm. */
} n00b_cfg_edge_kind_t;

// ============================================================================
// Basic block
// ============================================================================

n00b_list_decl(n00b_parse_tree_ptr_t);

/**
 * @brief A basic block in the control flow graph.
 *
 * Holds a list of statement-level parse tree nodes that execute
 * sequentially with no internal branching.
 */
typedef struct {
    int32_t        id;
    n00b_string_t  label;
    n00b_list_t(n00b_parse_tree_ptr_t) stmts;
    int32_t        start_pos;
    int32_t        end_pos;
    bool           is_entry;
    bool           is_exit;
} n00b_cfg_block_t;

// ============================================================================
// Edge
// ============================================================================

typedef struct {
    int32_t              from_id;
    int32_t              to_id;
    n00b_cfg_edge_kind_t kind;
    n00b_string_t        label;
} n00b_cfg_edge_t;

// ============================================================================
// CFG
// ============================================================================

n00b_list_decl(n00b_cfg_block_t);
n00b_list_decl(n00b_cfg_edge_t);

typedef struct {
    n00b_list_t(n00b_cfg_block_t) blocks;
    n00b_list_t(n00b_cfg_edge_t)  edges;
    int32_t                        entry_id;
    int32_t                        exit_id;
    n00b_string_t                  name;
} n00b_cfg_t;

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Build a CFG from an annotated parse tree.
 *
 * @param cf_labels  Label map from `n00b_annot_walk_tree_full`.
 * @param func_body  Root of the function/scope body to graph.
 * @param func_name  Name for the CFG (function name, etc.).
 * @return Heap-allocated CFG, or NULL on error.
 *
 * @pre `cf_labels` was produced by walking the same tree.
 */
n00b_cfg_t *n00b_build_cfg(n00b_dict_untyped_t *cf_labels,
                            n00b_parse_tree_t   *func_body,
                            n00b_string_t        func_name);

// ============================================================================
// Query
// ============================================================================

/** @brief Return the entry block. */
static inline n00b_cfg_block_t *
n00b_cfg_entry(n00b_cfg_t *cfg)
{
    if (!cfg || cfg->entry_id < 0) {
        return NULL;
    }

    return &cfg->blocks.data[cfg->entry_id];
}

/** @brief Return the exit block. */
static inline n00b_cfg_block_t *
n00b_cfg_exit(n00b_cfg_t *cfg)
{
    if (!cfg || cfg->exit_id < 0) {
        return NULL;
    }

    return &cfg->blocks.data[cfg->exit_id];
}

/** @brief Return block by ID. */
static inline n00b_cfg_block_t *
n00b_cfg_block(n00b_cfg_t *cfg, int32_t id)
{
    if (!cfg || id < 0 || (size_t)id >= n00b_list_len(cfg->blocks)) {
        return NULL;
    }

    return &cfg->blocks.data[id];
}

/** @brief Number of blocks. */
static inline int32_t
n00b_cfg_block_count(n00b_cfg_t *cfg)
{
    return cfg ? (int32_t)n00b_list_len(cfg->blocks) : 0;
}

/** @brief Number of edges. */
static inline int32_t
n00b_cfg_edge_count(n00b_cfg_t *cfg)
{
    return cfg ? (int32_t)n00b_list_len(cfg->edges) : 0;
}

/**
 * @brief Collect successor edges from a block.
 *
 * @param cfg       The CFG.
 * @param block_id  Source block.
 * @param out       Caller-provided output array.
 * @param max       Size of @p out.
 * @return Number of successors written.
 */
int32_t n00b_cfg_successors(n00b_cfg_t    *cfg,
                             int32_t        block_id,
                             n00b_cfg_edge_t *out,
                             int32_t        max);

/**
 * @brief Collect predecessor edges into a block.
 *
 * @param cfg       The CFG.
 * @param block_id  Destination block.
 * @param out       Caller-provided output array.
 * @param max       Size of @p out.
 * @return Number of predecessors written.
 */
int32_t n00b_cfg_predecessors(n00b_cfg_t    *cfg,
                               int32_t        block_id,
                               n00b_cfg_edge_t *out,
                               int32_t        max);

// ============================================================================
// Cleanup
// ============================================================================

void n00b_cfg_free(n00b_cfg_t *cfg);
