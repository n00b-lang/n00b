/**
 * @file rewrite.h
 * @brief Parse tree rewrite API for language extensions.
 *
 * This module provides functions for creating synthetic tokens and nodes,
 * and for replacing/transforming parse tree nodes. It enables language
 * extensions like `foreach(x in arr)` to be rewritten into standard C.
 *
 * ## Key Concepts
 *
 * - **Synthetic tokens**: Tokens not from source, containing generated text
 * - **Synthetic nodes**: Tree nodes created programmatically
 * - **Origin tracking**: Links synthetic nodes to original source for errors
 *
 * ## Usage Example
 *
 * @code
 * // Create a synthetic "for" keyword at line 10
 * tok_t *for_tok = synth_kw("for", 10);
 *
 * // Create a synthetic terminal node
 * tnode_t *for_node = synth_terminal("for", TT_KEYWORD, 10);
 *
 * // Create a non-terminal and add children
 * tnode_t *stmt = synth_nonterminal("for_statement_0");
 * stmt->kids[0] = for_node;
 * stmt->num_kids = 1;
 *
 * // Replace a node in the tree
 * replace_node(old_foreach, new_for, "foreach_expand");
 * @endcode
 */
#pragma once

#include <stdbool.h>
#include "types.h"

/** @name Synthetic Token Creation
 * @{
 */

/**
 * @brief Create a synthetic token with arbitrary text and type.
 *
 * @param text      Token text (will be copied)
 * @param type      Token type (TT_KEYWORD, TT_ID, TT_PUNCT, etc.)
 * @param source_line  Line number for #line directive positioning
 * @return Newly allocated synthetic token
 */
extern tok_t *synth_token(const char *text, ttype_t type, int source_line);

/** Convenience macro: create a synthetic identifier token. */
#define synth_id(name, line) synth_token((name), TT_ID, (line))

/** Convenience macro: create a synthetic keyword token. */
#define synth_kw(keyword, line) synth_token((keyword), TT_KEYWORD, (line))

/** Convenience macro: create a synthetic punctuation token. */
#define synth_punct(punct, line) synth_token((punct), TT_PUNCT, (line))

/** Convenience macro: create a synthetic numeric literal token. */
#define synth_num(num, line) synth_token((num), TT_NUM, (line))

/** @} */

/** @name Synthetic Node Creation
 * @{
 */

/**
 * @brief Create a synthetic terminal node (leaf with token).
 *
 * Creates a new tree node with a synthetic token. The node's `nt` field
 * is set to the token text, and `tptr` points to the synthetic token.
 *
 * @param text        Token text
 * @param type        Token type
 * @param source_line Line number for positioning
 * @return Newly allocated terminal node with synthetic token
 */
extern tnode_t *synth_terminal(const char *text, ttype_t type, int source_line);

/**
 * @brief Create a synthetic non-terminal node (branch node).
 *
 * Creates a new tree node for a grammar production. Children must be
 * added manually by setting kids[] and num_kids.
 *
 * @param nt   Non-terminal name (e.g., "for_statement_0")
 * @return Newly allocated non-terminal node with no children
 */
extern tnode_t *synth_nonterminal(const char *nt);

/** @} */

/** @name Tree Manipulation
 * @{
 */

/**
 * @brief Replace a child node in a parent.
 *
 * Replaces the child at the specified index with a new node, setting
 * up origin tracking on the new node.
 *
 * @param parent       Parent node
 * @param child_idx    Index of child to replace
 * @param new_child    Replacement node
 * @param rewrite_name Name of the rewrite for diagnostics (e.g., "foreach_expand")
 * @return The new_child node (for chaining)
 */
extern tnode_t *replace_child(tnode_t *parent, int child_idx, tnode_t *new_child, const char *rewrite_name);

/**
 * @brief Replace a node in the tree (updates parent's child reference).
 *
 * Finds old_node in its parent's children and replaces it with new_node,
 * setting up origin tracking. If old_node has no parent, only sets origin.
 *
 * @param old_node     Node to replace
 * @param new_node     Replacement node
 * @param rewrite_name Name of the rewrite for diagnostics
 * @return The new_node (for chaining)
 */
extern tnode_t *replace_node(tnode_t *old_node, tnode_t *new_node, const char *rewrite_name);

/**
 * @brief Replace a node without origin tracking (fast path).
 *
 * Like replace_node but skips the expensive source range computation.
 * Use this for internal transformations like flattening where origin
 * tracking is not needed for error messages.
 *
 * @param old_node  Node to replace
 * @param new_node  Replacement node
 * @return The new_node (for chaining)
 */
extern tnode_t *replace_node_fast(tnode_t *old_node, tnode_t *new_node);

/**
 * @brief Deep copy a parse tree.
 *
 * Creates a complete copy of a subtree, including all children.
 * Tokens are NOT copied (the new tree shares token pointers).
 * Origin information is preserved if present.
 *
 * @param node   Root of subtree to copy
 * @return Newly allocated copy of the subtree
 */
extern tnode_t *copy_tree(tnode_t *node);

/**
 * @brief Add a child to a nonterminal node.
 *
 * Appends the child to the parent's kids array and sets up parent pointer.
 *
 * @param parent  Parent nonterminal node
 * @param child   Child node to add
 */
extern void add_child(tnode_t *parent, tnode_t *child);

/** @} */

/** @name Utility Functions
 * @{
 */

/**
 * @brief Get the source line number for a node.
 *
 * For terminals, returns the token's line_no.
 * For non-terminals, recursively finds the first terminal.
 * For synthetic nodes with origin, returns the original start line.
 *
 * @param node   Node to get line for
 * @return Source line number, or -1 if unknown
 */
extern int get_node_line(tnode_t *node);

/**
 * @brief Check if a node is synthetic (created by rewrite).
 *
 * A node is synthetic if either:
 * - It has a synthetic token (tptr->synthetic == 1)
 * - It has origin tracking information
 *
 * @param node   Node to check
 * @return true if synthetic, false if from original parse
 */
extern bool is_synthetic_node(tnode_t *node);

/**
 * @brief Get the source line range covered by a node.
 *
 * Finds the minimum and maximum line numbers of all terminals in the subtree.
 *
 * @param node        Node to analyze
 * @param start_line  Output: first line (set to -1 if no terminals)
 * @param end_line    Output: last line (set to -1 if no terminals)
 */
extern void get_source_range(tnode_t *node, int *start_line, int *end_line);

/**
 * @brief Get the next node ID for synthetic nodes.
 *
 * Returns the next available node ID, ensuring synthetic nodes have
 * unique IDs consistent with parsed nodes.
 *
 * @return Next available node ID
 */
extern int get_next_node_id(void);

/** @} */

// Find a child node with specific nt_id
static inline tnode_t *
find_child(tnode_t *node, nt_type_t nt_id)
{
    if (!node) {
        return nullptr;
    }
    for (int i = 0; i < node->num_kids; i++) {
        tnode_t *kid = tnode_get_kid(node, i);
        if (kid && kid->nt_id == nt_id) {
            return kid;
        }
    }
    return nullptr;
}

// Recursively mark all tokens in a subtree to skip emit
static inline void
mark_skip_emit(tnode_t *node)
{
    if (!node) {
        return;
    }
    if (node->tptr) {
        node->tptr->skip_emit = 1;
    }
    for (int i = 0; i < node->num_kids; i++) {
        mark_skip_emit(tnode_get_kid(node, i));
    }
}
