#pragma once

/**
 * @file tree_util.h
 * @brief Shared parse-tree helpers for the annotation walk and type inference.
 *
 * These functions operate on slay parse trees, recursing through
 * `$$group` wrapper nodes as needed. They are used by `annot_walk.c`,
 * `infer.c`, and `infer_expr.c`.
 */

#include "slay/types.h"
#include "slay/parse_tree.h"
#include "slay/grammar.h"

/**
 * @brief Extract the text of the leftmost terminal (identifier) in a subtree.
 *
 * Recurses through interior and `$$group` nodes to find the first token.
 *
 * @param node  Root of the subtree to search.
 * @return A non-empty string on success, or `n00b_string_empty()` on failure.
 *         Callers must check `.u8_bytes > 0` (not just `.data`).
 */
n00b_string_t *n00b_tree_extract_first_identifier(n00b_parse_tree_t *node);

/**
 * @brief Find a child NT by name, recursing through `$$group` nodes.
 *
 * @param g       Grammar (for NT name lookup).
 * @param parent  Parent node to search.
 * @param name    NT name to match.
 * @return Matching child node, or NULL if not found.
 */
n00b_parse_tree_t *n00b_tree_find_child_by_nt_name(n00b_grammar_t    *g,
                                                      n00b_parse_tree_t *parent,
                                                      n00b_string_t     *name);

/**
 * @brief Find the leftmost terminal token in a subtree.
 *
 * Recurses through interior and group nodes.
 *
 * @param node  Root of the subtree to search.
 * @return Token info pointer, or NULL if no terminal found.
 */
n00b_token_info_t *n00b_tree_find_first_terminal(n00b_parse_tree_t *node);

/**
 * @brief Resolve a `n00b_child_ref_t` to a tree node.
 *
 * Handles both by-index and by-name references.
 *
 * @param g       Grammar (for by-name resolution).
 * @param parent  Parent node containing the children.
 * @param ref     Child reference to resolve.
 * @return The resolved child node, or NULL on failure.
 */
n00b_parse_tree_t *n00b_tree_resolve_child_ref(n00b_grammar_t    *g,
                                                 n00b_parse_tree_t *parent,
                                                 n00b_child_ref_t   ref);

/**
 * @brief Get the Nth non-terminal child, skipping terminals and
 *        recursing through group wrappers.
 *
 * Group nodes' NT children are counted as if they were direct children
 * of the parent (i.e., the group wrapper is transparent).
 *
 * @param node  Parent node.
 * @param n     Zero-based index of the desired NT child.
 * @return The Nth NT child, or NULL if out of range.
 */
n00b_parse_tree_t *n00b_tree_get_nth_nt_child(n00b_parse_tree_t *node,
                                                int32_t            n);

/**
 * @brief Extract the full dotted path from a left-recursive `<member-chain>`.
 *
 * The `<member-chain>` grammar is:
 * ```
 * <member-chain> ::= <member-chain> "." IDENTIFIER | IDENTIFIER
 * ```
 *
 * Walks the tree recursively and joins identifiers with `.` to produce
 * strings like `"pkg.sub.module"`.
 *
 * @param node  Root of the `<member-chain>` subtree.
 * @param buf   Output buffer (caller provides, must be large enough).
 * @param cap   Capacity of @p buf in bytes.
 * @return Length written (excluding NUL), or -1 on error.
 */
int32_t n00b_tree_extract_member_chain(n00b_parse_tree_t *node,
                                         char              *buf,
                                         int32_t            cap);
