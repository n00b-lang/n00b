/**
 * @file xform_helpers.h
 * @brief Shared AST construction and navigation utilities for transforms.
 *
 * Provides common tree-building primitives used by multiple `xform_*.c`
 * modules: identifier/expression construction, child manipulation, and
 * node search.  Centralising these eliminates ~500 lines of identical
 * static copies scattered across transform files.
 */
#pragma once

#include "types.h"
#include "nt_types.h"
#include "rewrite.h"
#include "transform.h"
#include "token.h"

// ====================================================================
// Child manipulation
// ====================================================================

/**
 * @brief Find the index of @p child in @p parent's kids array.
 * @return Index (>= 0), or -1 if not found.
 */
extern int find_child_index(tnode_t *parent, tnode_t *child);

/**
 * @brief Remove the child at @p idx, shifting later children left.
 * @pre 0 <= idx < parent->num_kids
 */
extern void remove_child_at(tnode_t *parent, int idx);

/**
 * @brief Insert @p child at @p idx, shifting later children right.
 * @pre 0 <= idx <= parent->num_kids
 * @post child->parent == parent
 */
extern void insert_child_at(tnode_t *parent, int idx, tnode_t *child);

/**
 * @brief Replace the child at @p idx (no origin tracking).
 * @post new_child->parent == parent
 */
extern void replace_child_at(tnode_t *parent, int idx, tnode_t *new_child);

// ====================================================================
// Node search
// ====================================================================

/**
 * @brief Recursively find the first identifier node in a subtree.
 * @return Pointer to the identifier tnode_t, or nullptr.
 */
extern tnode_t *find_identifier_node(tnode_t *node);

/**
 * @brief Extract identifier text from a declarator subtree.
 * @return Null-terminated string, or nullptr.
 */
extern const char *get_identifier_text(ncc_buf_t *input, tnode_t *declarator);

/**
 * @brief Iterative DFS search for the first node with @p nt_id.
 * @return Matching node, or nullptr.
 */
extern tnode_t *find_node_by_type(tnode_t *node, nt_type_t nt_id);

// ====================================================================
// AST construction
// ====================================================================

/**
 * @brief Build a synthetic identifier node.
 *
 * Creates: `identifier -> TT_ID(name)`
 */
extern tnode_t *build_identifier(const char *name, int line);

/**
 * @brief Build a primary_expression wrapping an identifier.
 *
 * Creates: `primary_expression_7 -> identifier -> TT_ID(name)`
 */
extern tnode_t *build_primary_id(const char *name, int line);

/**
 * @brief Wrap an inner expression in the full C precedence hierarchy.
 *
 * Produces the chain:
 *   unary -> cast -> mult -> add -> shift -> rel -> eq
 *   -> AND -> XOR -> OR -> logAND -> logOR -> cond -> assign
 *
 * If @p inner is a `primary_expression`, a `postfix_expression` wrapper
 * is inserted automatically.
 *
 * @return The outermost `assignment_expression` node.
 */
extern tnode_t *wrap_in_expr_hierarchy(tnode_t *inner, int line);

// ====================================================================
// Emit / callee / numeric helpers
// ====================================================================

/**
 * @brief Recursively find an identifier token in a subtree.
 *
 * Only walks into primary_expression / identifier nodes (not member
 * access or subscript expressions).
 */
extern tok_t *find_identifier_tok(tnode_t *node);

/**
 * @brief Extract the callee name from a CALL postfix_expression.
 * @return Allocated string (caller must free), or nullptr.
 */
extern char *get_callee_name(tree_xform_t *ctx, tnode_t *node);

/**
 * @brief Emit a subtree to a dynamically allocated string.
 */
extern char *emit_node_to_string(tree_xform_t *ctx, tnode_t *node);

/**
 * @brief Strip #line directives from emitted source text.
 */
extern char *strip_line_directives(const char *src);

/**
 * @brief Build a numeric literal replacement node.
 *
 * Creates: `postfix_expression(PRIMARY) -> primary_expression -> constant -> TT_NUM`
 */
extern tnode_t *build_numeric_literal(const char *value_str, int line);
