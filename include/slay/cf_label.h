#pragma once

/**
 * @file cf_label.h
 * @brief Control flow labels produced by the annotation walk.
 *
 * The annotation walk can produce control flow labels for tree nodes
 * annotated with `@branch`, `@loop`, `@switch`, `@jump`, `@capture`,
 * or `@assigns`. Each label records the kind and the resolved child
 * subtrees (condition, body, else, etc.).
 *
 * Labels are stored in a typed dict keyed by parse tree node pointer.
 * Use `n00b_cf_label_lookup()` to query.
 */

#include "slay/types.h"
#include "slay/parse_tree.h"
#include "slay/symtab.h"
#include "core/dict.h"
#include "core/list.h"

typedef struct n00b_tc_ctx_s n00b_tc_ctx_t;

n00b_list_decl(n00b_sym_entry_t *);

// Forward declare so the dict_decl works before the full typedef.
typedef struct n00b_cf_label_s n00b_cf_label_t;

n00b_dict_decl(n00b_parse_tree_t *, n00b_cf_label_t *);

typedef n00b_dict_t(n00b_parse_tree_t *, n00b_cf_label_t *) n00b_cf_labels_t;

// ============================================================================
// Control flow label kind
// ============================================================================

/** @brief Kind of control flow label. */
typedef enum {
    N00B_CF_BRANCH,         /**< if/else, ternary */
    N00B_CF_LOOP,           /**< while, for, do-while */
    N00B_CF_SWITCH,         /**< switch */
    N00B_CF_JUMP,           /**< break, continue, return, goto */
    N00B_CF_CAPTURE,        /**< try/catch/finally */
    N00B_CF_ASSIGNS,        /**< assignment (name = value) */
    N00B_CF_VARREF,         /**< variable reference (use) */
    N00B_CF_UNWRAP_RESULT,  /**< postfix ! (result unwrap with early return) */
} n00b_cf_kind_t;

// ============================================================================
// Control flow label
// ============================================================================

/**
 * @brief A control flow label attached to a parse tree node.
 *
 * Unused fields are zero/NULL for kinds that don't need them.
 */
struct n00b_cf_label_s {
    n00b_cf_kind_t     kind;
    n00b_parse_tree_t *self;      /**< The annotated tree node. */
    n00b_parse_tree_t *cond;      /**< Condition subtree (branch, loop, switch). */
    n00b_parse_tree_t *then_body; /**< Then/body subtree. */
    n00b_parse_tree_t *else_body; /**< Else subtree (branch only, may be NULL). */
    n00b_string_t      jump_kind; /**< "break", "continue", "return", "goto". */
    n00b_string_t      tag;       /**< Capture tag or jump target. */
    bool               capture_by_tag;
};

// ============================================================================
// Walk result
// ============================================================================

// Dict from parse tree node pointer → type pointer for literal/expression types.
n00b_dict_decl(uintptr_t, n00b_tc_type_t *);
typedef n00b_dict_t(uintptr_t, n00b_tc_type_t *) n00b_node_types_t;

/**
 * @brief Result of a full annotation walk (symtab + control flow labels).
 */
typedef struct {
    n00b_symtab_t                    *symtab;
    n00b_cf_labels_t                 *cf_labels;      /**< `n00b_parse_tree_t *` -> `n00b_cf_label_t *` */
    n00b_tc_ctx_t                    *tc_ctx;         /**< Type-checking context (owns type vars). */
    n00b_list_t(n00b_sym_entry_t *)  *params;         /**< Parameter symbols for DFG entry defs. */
    n00b_node_types_t                *node_types;      /**< Parse node → resolved type. */
    n00b_list_t(n00b_sym_entry_t *)  *shadowed_entries; /**< Entries that shadow outer-scope decls. */
} n00b_annot_result_t;

// ============================================================================
// Query
// ============================================================================

/**
 * @brief Look up a control flow label for a parse tree node.
 *
 * @param labels  The cf_labels dict from an `n00b_annot_result_t`.
 * @param node    The parse tree node to look up.
 * @return The label, or NULL if no label exists for this node.
 */
static inline n00b_cf_label_t *
n00b_cf_label_lookup(n00b_cf_labels_t *labels, n00b_parse_tree_t *node)
{
    if (!labels || !node) {
        return NULL;
    }

    bool            found = false;
    n00b_cf_label_t *val  = n00b_dict_get(labels, node, &found);

    return found ? val : NULL;
}

// ============================================================================
// Accessors
// ============================================================================

/** @brief Get the symtab from an annotation walk result. */
static inline n00b_symtab_t *
n00b_annot_result_symtab(n00b_annot_result_t *r)
{
    return r ? r->symtab : NULL;
}

/** @brief Get the control flow labels from an annotation walk result. */
static inline n00b_cf_labels_t *
n00b_annot_result_cf_labels(n00b_annot_result_t *r)
{
    return r ? r->cf_labels : NULL;
}
