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

n00b_tree_decl(n00b_nt_node_t, n00b_token_info_t *);

/**
 * @brief A parse tree: n-ary tree where interior nodes carry
 *        @c n00b_nt_node_t and leaves carry @c n00b_token_info_t pointers.
 */
typedef n00b_tree_t(n00b_nt_node_t, n00b_token_info_t *) n00b_parse_tree_t;

// ============================================================================
// Parse tree pointer type + array type
// ============================================================================

typedef n00b_parse_tree_t *n00b_parse_tree_ptr_t;

n00b_array_decl(n00b_parse_tree_ptr_t);

/** @brief Array of parse tree pointers. */
typedef n00b_array_t(n00b_parse_tree_ptr_t) n00b_parse_tree_array_t;

// ============================================================================
// Disambiguator callback
// ============================================================================

/**
 * @brief Disambiguator callback for choosing among ambiguous parse trees.
 *
 * Called when the parser produces multiple trees with equal penalty and cost.
 * Must return negative if @p a should be preferred, positive if @p b should
 * be preferred, or zero if they are equivalent.
 *
 * The default disambiguator (`n00b_parse_tree_default_disambig`) uses a
 * structural comparison: prefer more nodes (more specific parse), then
 * lower rule index, then recursive child comparison.
 */
typedef int (*n00b_tree_disambig_fn_t)(n00b_parse_tree_t *a,
                                        n00b_parse_tree_t *b);

/**
 * @brief Default structural disambiguator.
 *
 * Preference order after penalty/cost tie:
 *   1. More total nodes (more specific parse wins)
 *   2. Lower rule index
 *   3. Recursive child comparison for stability
 */
int n00b_parse_tree_default_disambig(n00b_parse_tree_t *a,
                                       n00b_parse_tree_t *b);

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

// ============================================================================
// Extended node inspection
// ============================================================================

/**
 * @brief Get the NT name as a C string for an interior node.
 * @return NT name string data pointer, or NULL if the node is a leaf.
 *
 * @pre The returned pointer borrows from the node's `n00b_string_t` and is
 *      valid as long as the node lives. It is NOT null-terminated in general;
 *      use @c n00b_parse_node_name_len() for the byte length.
 */
const char *n00b_pt_nt_name(n00b_parse_tree_t *t);

/**
 * @brief Get the byte length of the NT name.
 */
size_t n00b_pt_nt_name_len(n00b_parse_tree_t *t);

/**
 * @brief Check whether a node is an interior node with the given NT name.
 * @param t        Node to inspect.
 * @param nt_name  Null-terminated C string to compare against.
 */
bool n00b_pt_is_nt(n00b_parse_tree_t *t, const char *nt_name);

/**
 * @brief Get the NT id for an interior node.
 * @return NT id, or a negative value if the node is a leaf.
 */
int64_t n00b_pt_nt_id(n00b_parse_tree_t *t);

/**
 * @brief Check whether a node is a token leaf.
 */
bool n00b_pt_is_token(n00b_parse_tree_t *t);

/**
 * @brief Get the token text as a C-string data pointer from a leaf.
 *
 * @return Token value data pointer, or NULL if not a token or no value.
 * @note The returned pointer is NOT null-terminated in general; use
 *       @c n00b_pt_token_text_len() for byte length.
 */
const char *n00b_pt_token_text(n00b_parse_tree_t *t);

/**
 * @brief Get the byte length of the token text.
 */
size_t n00b_pt_token_text_len(n00b_parse_tree_t *t);

/**
 * @brief Get the number of children of a node.
 * @return Number of children (0 for leaves).
 */
size_t n00b_pt_num_children(n00b_parse_tree_t *t);

/**
 * @brief Get child at given index (bounds-checked).
 * @return Child node, or NULL if out of range or leaf.
 */
n00b_parse_tree_t *n00b_pt_get_child(n00b_parse_tree_t *t, size_t index);

// ============================================================================
// Pattern matching / search helpers
// ============================================================================

/**
 * @brief Find the first direct child whose NT name matches.
 * @param node     Parent node.
 * @param nt_name  Null-terminated NT name to search for.
 * @return Matching child, or NULL if not found.
 */
n00b_parse_tree_t *n00b_pt_find_child_by_nt(n00b_parse_tree_t *node,
                                              const char        *nt_name);

/**
 * @brief Find the first direct child that is a token with the given text.
 *
 * Comparison is by byte prefix (null-terminated @p text against the
 * token value which may not be null-terminated).
 *
 * @param node        Parent node.
 * @param token_text  Null-terminated token text to match.
 * @return Matching child, or NULL if not found.
 */
n00b_parse_tree_t *n00b_pt_find_child_by_token(n00b_parse_tree_t *node,
                                                 const char        *token_text);

/**
 * @brief Collect all direct children matching an NT name.
 * @param node     Parent node.
 * @param nt_name  NT name to match.
 * @param out      Output array of child pointers.
 * @param max      Maximum matches to collect.
 * @return Number of matches written.
 */
int n00b_pt_collect_children_by_nt(n00b_parse_tree_t  *node,
                                    const char         *nt_name,
                                    n00b_parse_tree_t **out,
                                    int                 max);

/**
 * @brief Collect children matching an NT name, flattening through
 *        synthetic `$$group_N` nodes from BNF quantifiers.
 *
 * Recurses into `$$`-prefixed group nodes, collecting all matching
 * NTs regardless of nesting depth.
 *
 * @param node     Parent node.
 * @param nt_name  NT name to match.
 * @param out      Output array of child pointers.
 * @param max      Maximum matches.
 * @return Number of matches written.
 */
int n00b_pt_collect_nt_deep(n00b_parse_tree_t  *node,
                              const char         *nt_name,
                              n00b_parse_tree_t **out,
                              int                 max);

/**
 * @brief Check whether a node is a synthetic `$$group_N` node.
 *
 * These are generated by BNF quantifiers (`?`, `*`, `+`).
 */
bool n00b_pt_is_group(n00b_parse_tree_t *t);

/**
 * @brief Find the leftmost token leaf in a subtree.
 * @return Token leaf node, or NULL if no tokens.
 */
n00b_parse_tree_t *n00b_pt_first_token(n00b_parse_tree_t *node);

/**
 * @brief Find the rightmost token leaf in a subtree.
 * @return Token leaf node, or NULL if no tokens.
 */
n00b_parse_tree_t *n00b_pt_last_token(n00b_parse_tree_t *node);

// ============================================================================
// Deep search (recursive DFS over entire subtree)
// ============================================================================

/**
 * @brief Recursively find all nodes with the given NT name.
 *
 * DFS pre-order traversal. Matching nodes are collected but their
 * subtrees are still traversed (nested matches are included).
 *
 * @param node     Root of subtree to search.
 * @param nt_name  Null-terminated NT name to match.
 * @param out      Output array of matching nodes.
 * @param max      Maximum matches to collect.
 * @return Number of matches written.
 */
int n00b_pt_search_by_nt(n00b_parse_tree_t  *node,
                           const char         *nt_name,
                           n00b_parse_tree_t **out,
                           int                 max);

/**
 * @brief Recursively find all token leaves whose text matches.
 *
 * DFS traversal collecting all token leaves with matching text.
 *
 * @param node  Root of subtree to search.
 * @param text  Null-terminated token text to match.
 * @param out   Output array of matching nodes.
 * @param max   Maximum matches to collect.
 * @return Number of matches written.
 */
int n00b_pt_search_by_token_text(n00b_parse_tree_t  *node,
                                   const char         *text,
                                   n00b_parse_tree_t **out,
                                   int                 max);

/**
 * @brief Recursively find all token leaves with the given terminal ID.
 *
 * @param node  Root of subtree to search.
 * @param tid   Terminal ID to match.
 * @param out   Output array of matching nodes.
 * @param max   Maximum matches to collect.
 * @return Number of matches written.
 */
int n00b_pt_search_by_tid(n00b_parse_tree_t  *node,
                            int64_t             tid,
                            n00b_parse_tree_t **out,
                            int                 max);
