/**
 * @file transform.h
 * @brief Tree-walking transformer system for post-parse AST transformations.
 *
 * This module provides a framework for registering and applying transformations
 * to parse trees after parsing. Unlike token-level transformations (before parsing),
 * tree transformations operate on the complete AST, enabling:
 *
 *   - Syntax sugar expansion (e.g., foreach → for loop)
 *   - Custom AST rewrites using the rewrite.h API
 *   - Multi-pass transformations until fixed point
 *
 * ## Architecture
 *
 * ```
 * parse_translation_unit()  →  xform_apply()  →  final_output()
 *           │                       │                  │
 *      Build AST            Walk tree, call        Emit code
 *                           registered xforms
 * ```
 *
 * ## Transformer Types
 *
 * Two types of transformers are supported:
 *
 *   1. **Post-order (default)**: Called after children are processed.
 *      Natural for "reduce" semantics where you combine processed children.
 *
 *   2. **Pre-order**: Called before children are processed.
 *      Can skip children via control parameter (useful for replacing entire subtrees).
 *
 * ## Usage Example
 *
 * @code
 * // Define a post-order transformer for iteration statements
 * static tnode_t *
 * xform_foreach_expand(xform_ctx_t *ctx, tnode_t *node)
 * {
 *     if (node->branch != FOREACH_BRANCH) {
 *         return nullptr;  // Not foreach, no change
 *     }
 *
 *     // Build replacement for loop using synth_* from rewrite.h
 *     tnode_t *for_stmt = synth_nonterminal("iteration_statement_2");
 *     // ... populate children ...
 *
 *     replace_node(node, for_stmt, "foreach_expand");
 *     return for_stmt;
 * }
 *
 * // Register and apply
 * xform_registry_t reg;
 * xform_registry_init(&reg);
 * xform_register_post(&reg, NT_iteration_statement, xform_foreach_expand, "foreach");
 *
 * xform_ctx_t ctx;
 * xform_ctx_init(&ctx, input_buf, nullptr, parse_tree);
 * tnode_t *result = xform_apply(&reg, &ctx);
 * @endcode
 */
#pragma once

#include <stdbool.h>
#include "types.h"
#include "nt_types.h"
#include "st.h"
#include "buf.h"

/** @name Configuration
 * @{
 */

/** Default maximum passes for xform_apply_multi. */
#define XFORM_DEFAULT_MAX_PASSES 100

/** @} */

/** @name Forward Declarations
 * @{
 */

typedef struct lex_t            lex_t;
typedef struct xform_registry_t xform_registry_t;
typedef struct xform_ctx_t      xform_ctx_t;

/** @} */

/** @name Control Types
 * @{
 */

/**
 * @brief Control value for pre-order transformers.
 *
 * Returned via pointer parameter to indicate whether children should be processed.
 */
typedef enum {
    XFORM_CONTINUE,      /**< Continue processing children normally */
    XFORM_SKIP_CHILDREN, /**< Skip processing children of this node */
} xform_control_t;

/** @} */

/** @name Transformer Function Types
 * @{
 */

/**
 * @brief Post-order transformer function signature.
 *
 * Called after all children of a node have been processed (bottom-up).
 * This is the most common transformer type.
 *
 * @param ctx  Transformation context (input buffer, stats, etc.)
 * @param node The node to potentially transform
 * @return Replacement node, or nullptr if no transformation
 *
 * @note The returned node replaces the input node in the tree.
 *       Use replace_node() from rewrite.h to set up origin tracking.
 */
typedef tnode_t *(*xform_fn_t)(xform_ctx_t *ctx, tnode_t *node);

/**
 * @brief Pre-order transformer function signature.
 *
 * Called before children are processed (top-down). Can control
 * whether children should be processed via the control parameter.
 *
 * @param ctx     Transformation context
 * @param node    The node to potentially transform
 * @param control [out] Set to XFORM_SKIP_CHILDREN to skip child processing
 * @return Replacement node, or nullptr if no transformation
 *
 * @note When returning a replacement and setting XFORM_SKIP_CHILDREN,
 *       the replacement's children will NOT be walked.
 */
typedef tnode_t *(*xform_pre_fn_t)(xform_ctx_t *ctx, tnode_t *node, xform_control_t *control);

/** @} */

/** @name Transformer Entry
 * @{
 */

/**
 * @brief A registered transformer with metadata.
 */
typedef struct {
    union {
        xform_fn_t     post_fn; /**< Post-order function */
        xform_pre_fn_t pre_fn;  /**< Pre-order function */
    };
    const char *name;           /**< Transformer name for debugging */
    bool        is_pre;         /**< True if pre-order transformer */
} xform_entry_t;

/** @} */

/** @name Registry
 * @{
 */

/**
 * @brief Registry of transformers indexed by node type.
 *
 * Provides O(1) lookup of transformers by nt_type_t enum value.
 * Supports unlimited transformers per node type (executed in registration order).
 * Uses list_t for dynamic storage with no arbitrary limits.
 * Wildcard transformers (NT_NONE) match all nodes.
 */
struct xform_registry_t {
    /** Pre-order transformers indexed by NT enum (list of xform_entry_t*) */
    list_t *pre_order[NT_COUNT];

    /** Post-order transformers indexed by NT enum (list of xform_entry_t*) */
    list_t *post_order[NT_COUNT];

    /** Wildcard pre-order transformers (list of xform_entry_t*) */
    list_t *wildcard_pre;

    /** Wildcard post-order transformers (list of xform_entry_t*) */
    list_t *wildcard_post;
};

/** @} */

/** @name Context
 * @{
 */

/**
 * @brief Transformation context passed to transformer functions.
 *
 * Contains input buffer, symbol table, tree root, and statistics.
 * Created fresh for each xform_apply() call.
 */
struct xform_ctx_t {
    ncc_buf_t *input;           /**< Source buffer for token extraction */
    lex_t    *lex;              /**< Lexer state (for ncc_off range checks) */
    symtab_t *symtab;           /**< Symbol table (may be nullptr) */
    tnode_t  *root;             /**< Root of tree being transformed */
    int       depth;            /**< Current recursion depth */
    int       pass;             /**< Current pass number (0-based) */
    int       nodes_visited;    /**< Total nodes visited this pass */
    int       nodes_replaced;   /**< Total nodes replaced this pass */
    void     *user_data;        /**< User-defined context data */
    tnode_t  *current_func_def; /**< Innermost enclosing function_definition */
};

/** @} */

/** @name Registry Initialization and Cleanup
 * @{
 */

/**
 * @brief Initialize a transformer registry.
 *
 * Must be called before registering transformers.
 *
 * @param reg Registry to initialize
 */
extern void xform_registry_init(xform_registry_t *reg);

/**
 * @brief Free all memory used by a transformer registry.
 *
 * Call this when done with a registry to avoid memory leaks.
 *
 * @param reg Registry to free
 */
extern void xform_registry_free(xform_registry_t *reg);

/** @} */

/** @name Transformer Registration
 * @{
 */

/**
 * @brief Register a post-order transformer for a node type.
 *
 * Post-order transformers are called after children are processed,
 * enabling bottom-up transformations.
 *
 * @param reg   Registry to add to
 * @param nt_id Node type to match (NT_NONE for wildcard)
 * @param fn    Transformer function
 * @param name  Transformer name for debugging (not copied, must be static)
 * @return true if registered successfully, false on allocation failure
 */
extern bool xform_register_post(xform_registry_t *reg, nt_type_t nt_id, xform_fn_t fn, const char *name);

/**
 * @brief Register a pre-order transformer for a node type.
 *
 * Pre-order transformers are called before children are processed,
 * enabling top-down transformations with descent control.
 *
 * @param reg   Registry to add to
 * @param nt_id Node type to match (NT_NONE for wildcard)
 * @param fn    Transformer function
 * @param name  Transformer name for debugging (not copied, must be static)
 * @return true if registered successfully, false on allocation failure
 */
extern bool xform_register_pre(xform_registry_t *reg, nt_type_t nt_id, xform_pre_fn_t fn, const char *name);

/**
 * @brief Get the number of transformers registered for a node type.
 *
 * @param reg   Registry to query
 * @param nt_id Node type (NT_NONE for wildcard)
 * @param pre   If true, count pre-order; if false, count post-order
 * @return Number of registered transformers
 */
extern int xform_count(xform_registry_t *reg, nt_type_t nt_id, bool pre);

/** @} */

/** @name Context Initialization
 * @{
 */

/**
 * @brief Initialize a transformation context.
 *
 * @param ctx    Context to initialize
 * @param lex    Lexer state (for ncc_off checks, may be nullptr)
 * @param symtab Symbol table (may be nullptr)
 * @param root   Root of tree to transform
 */
extern void xform_ctx_init(xform_ctx_t *ctx, lex_t *lex, symtab_t *symtab, tnode_t *root);

/** @} */

/** @name Transformation Application
 * @{
 */

/**
 * @brief Apply all registered transformers to a tree (single pass).
 *
 * Walks the tree recursively, calling registered transformers:
 *   1. Pre-order transformers (may skip children)
 *   2. Recurse into children (unless skipped)
 *   3. Post-order transformers
 *
 * Transformer return values:
 *   - nullptr: No change, keep current node
 *   - non-nullptr: Replace current node with returned node
 *
 * @param reg Registry of transformers
 * @param ctx Transformation context (must have root set)
 * @return Transformed tree root (may differ from ctx->root if root was replaced)
 *
 * @note Replacement nodes are NOT re-walked in the same pass.
 *       Use xform_apply_multi() for iterative transformation.
 */
extern tnode_t *xform_apply(xform_registry_t *reg, xform_ctx_t *ctx);

/**
 * @brief Apply transformers repeatedly until no changes (multi-pass).
 *
 * Calls xform_apply() in a loop until either:
 *   - No nodes are replaced in a pass
 *   - Maximum pass count is reached
 *
 * @param reg      Registry of transformers
 * @param ctx      Transformation context
 * @param max_passes Maximum number of passes (0 for XFORM_DEFAULT_MAX_PASSES)
 * @return Transformed tree root after fixed point or max passes
 */
extern tnode_t *xform_apply_multi(xform_registry_t *reg, xform_ctx_t *ctx, int max_passes);

/** @} */

/** @name Statistics
 * @{
 */

/**
 * @brief Get the number of nodes visited in the last pass.
 * @param ctx Transformation context
 * @return Node visit count
 */
static inline int
xform_nodes_visited(xform_ctx_t *ctx)
{
    return ctx->nodes_visited;
}

/**
 * @brief Get the number of nodes replaced in the last pass.
 * @param ctx Transformation context
 * @return Node replacement count
 */
static inline int
xform_nodes_replaced(xform_ctx_t *ctx)
{
    return ctx->nodes_replaced;
}

/**
 * @brief Get the current pass number.
 * @param ctx Transformation context
 * @return Pass number (0-based)
 */
static inline int
xform_current_pass(xform_ctx_t *ctx)
{
    return ctx->pass;
}

/** @} */
