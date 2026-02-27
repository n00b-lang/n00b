#pragma once

/**
 * @file parse_tree.h
 * @brief Parse tree type and node inspection helpers.
 *
 * Defines the concrete parse tree type shared by all parser engines
 * (PWZ, Earley, packrat). Interior nodes carry @c n00b_nt_node_t and
 * leaves carry @c n00b_token_info_t pointers.
 */

#include "slay/types.h"
#include "core/tree.h"

// ============================================================================
// Parse tree type
// ============================================================================

n00b_tree_decl(n00b_nt_node_t, n00b_token_info_ptr_t);

/**
 * @brief A parse tree: n-ary tree where interior nodes carry
 *        @c n00b_nt_node_t and leaves carry @c n00b_token_info_t pointers.
 */
typedef n00b_tree_t(n00b_nt_node_t, n00b_token_info_ptr_t) n00b_parse_tree_t;

// ============================================================================
// Parse tree pointer type + array type
// ============================================================================

typedef n00b_parse_tree_t *n00b_parse_tree_ptr_t;

n00b_array_decl(n00b_parse_tree_ptr_t);

/** @brief Array of parse tree pointers. */
typedef n00b_array_t(n00b_parse_tree_ptr_t) n00b_parse_tree_array_t;

// ============================================================================
// Quick accessors
// ============================================================================

/** @brief Get the NT id from a parse tree node (-1 if leaf or NULL). */
static inline int64_t
n00b_pt_nt_id(n00b_parse_tree_t *t)
{
    if (!t || n00b_tree_is_leaf(t)) {
        return -1;
    }
    return n00b_tree_node_value(t).id;
}

/** @brief Get the number of children (0 for leaves or NULL). */
static inline size_t
n00b_pt_num_children(n00b_parse_tree_t *t)
{
    if (!t) {
        return 0;
    }
    return n00b_tree_num_children(t);
}

// ============================================================================
// Node inspection (engine-agnostic)
// ============================================================================

/**
 * @brief Check whether a parse tree node represents a terminal token.
 * @param t  Parse tree node to inspect.
 * @return True if the node is a terminal (token) leaf.
 */
bool n00b_parse_node_is_token(n00b_parse_tree_t *t);

/**
 * @brief Get the token from a terminal parse tree node.
 * @param t  Parse tree node (must be a terminal leaf).
 * @return Pointer to the token info, or NULL if not a terminal.
 */
n00b_token_info_t *n00b_parse_node_token(n00b_parse_tree_t *t);

/**
 * @brief Get the non-terminal name from a parse tree node.
 * @param t  Parse tree node (must be a non-terminal interior node).
 * @return The NT name, or none if the node is a terminal.
 */
n00b_option_t(n00b_string_t) n00b_parse_node_name(n00b_parse_tree_t *t);

// ============================================================================
// Tree walking (engine-agnostic)
// ============================================================================

/**
 * @brief Walk a parse tree using grammar-registered actions.
 *
 * Performs a depth-first traversal. For each non-terminal node, calls
 * the walk action registered on the grammar (or the default action).
 * Terminal nodes return their token pointer directly.
 *
 * @param g      Grammar with registered walk actions.
 * @param node   Root of the subtree to walk.
 * @param thunk  User-defined context pointer passed to actions.
 * @return The result value from the root action.
 */
void *n00b_parse_tree_walk(n00b_grammar_t *g, n00b_parse_tree_t *node, void *thunk);

/**
 * @brief Recursively free a parse tree.
 *
 * Frees all tree node structures. Does not free the tokens
 * themselves (those are owned by the token list / parser).
 *
 * @param t  Root of the tree to free.
 */
void n00b_parse_tree_free(n00b_parse_tree_t *t);
