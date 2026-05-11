#pragma once

/**
 * @file annot_walk.h
 * @brief Post-parse annotation walk — DFS the best tree, firing annotations
 *        to build a symbol table.
 *
 * Scope push happens automatically when entering a production with
 * `@scope`; scope pop happens when leaving it. This replaces the old
 * mid-parse annotation firing that was incompatible with ambiguity-aware
 * parsers (Earley, PWZ).
 */

#include "slay/symtab.h"
#include "slay/cf_label.h"
#include "slay/n00b_parse.h"

// ============================================================================
// Type-spec translation callback
// ============================================================================

/**
 * @brief Callback for translating language-specific type-spec parse subtrees.
 *
 * Different languages have different `<type-spec>` grammars. This callback
 * decouples slay's annotation system from any particular grammar's type AST.
 * When NULL, type translation is skipped (symbols get fresh type variables
 * but no explicit type binding from parse subtrees).
 */
typedef n00b_tc_type_t *(*n00b_translate_type_spec_fn)(
    n00b_tc_ctx_t     *ctx,
    n00b_grammar_t    *g,
    n00b_parse_tree_t *type_node);

// ============================================================================
// Walk context
// ============================================================================

/** @brief Context carried through the annotation walk. */
typedef struct {
    n00b_symtab_t                    *symtab;
    n00b_grammar_t                   *grammar;
    n00b_cf_labels_t                 *cf_labels;
    n00b_tc_ctx_t                    *tc_ctx;      /**< Type-checking context for fresh vars. */
    struct n00b_diag_ctx_s           *diag;        /**< Diagnostic accumulator (may be NULL). */
    n00b_list_t(n00b_sym_entry_t *)  *params;            /**< Accumulated parameter symbols. */
    n00b_node_types_t                *node_types;         /**< Parse node → resolved type. */
    n00b_list_t(n00b_sym_entry_t *)  *shadowed_entries;   /**< Entries that shadow outer decls. */
    n00b_translate_type_spec_fn       translate_type_spec; /**< Language-specific type translator (may be NULL). */
    int32_t                           anon_counter;       /**< Counter for unique anonymous ADT scope names. */
    n00b_sym_mutability_t             current_mutability;  /**< Set by `<variable-decl>` for child `@declares`. */
    n00b_string_t                    *current_visibility;  /**< Set by `@visibility` for sibling `@declares`. */
} n00b_annot_walk_ctx_t;

// ============================================================================
// Walk API — symtab only (original)
// ============================================================================

/**
 * @brief Walk the best tree from a parse result, firing annotations
 *        to populate a new symbol table.
 *
 * Scope annotations push/pop automatically as the walker enters and
 * leaves the annotated production.
 *
 * @param result  Parse result (must have succeeded).
 * @return Newly allocated symbol table, or NULL if result is invalid.
 *         Caller owns the returned table (free via `n00b_symtab_free`).
 */
n00b_symtab_t *n00b_annot_walk(n00b_parse_result_t *result);

/**
 * @brief Walk a specific tree against a grammar's annotations.
 *
 * @param g     Grammar with annotations.
 * @param tree  Root of the tree to walk.
 * @return Newly allocated symbol table, or NULL on error.
 *         Caller owns the returned table.
 */
n00b_symtab_t *n00b_annot_walk_tree(n00b_grammar_t    *g,
                                      n00b_parse_tree_t *tree);

// ============================================================================
// Walk API — full result (symtab + control flow labels)
// ============================================================================

/**
 * @brief Walk the best tree, producing both a symbol table and CF labels.
 *
 * @param result  Parse result (must have succeeded).
 * @return Walk result, or NULL if result is invalid. Caller owns it.
 */
n00b_annot_result_t *n00b_annot_walk_full(n00b_parse_result_t *result);

/**
 * @brief Walk a specific tree, producing both a symbol table and CF labels.
 *
 * @param g     Grammar with annotations.
 * @param tree  Root of the tree to walk.
 * @return Walk result, or NULL on error. Caller owns it.
 */
n00b_annot_result_t *n00b_annot_walk_tree_full(n00b_grammar_t    *g,
                                                 n00b_parse_tree_t *tree);

/**
 * @brief Walk a specific tree with a custom type-spec translator.
 *
 * Like `n00b_annot_walk_tree_full`, but uses @p ts_fn to translate
 * type-spec subtrees instead of the default (no translation).
 *
 * @param g      Grammar with annotations.
 * @param tree   Root of the tree to walk.
 * @param ts_fn  Language-specific type translator (may be NULL).
 * @return Walk result, or NULL on error. Caller owns it.
 */
n00b_annot_result_t *n00b_annot_walk_tree_full_ex(n00b_grammar_t             *g,
                                                    n00b_parse_tree_t          *tree,
                                                    n00b_translate_type_spec_fn ts_fn)
_kargs {
    n00b_symtab_t          *symtab;   /**< Pre-populated symtab (NULL = create fresh). */
    n00b_tc_ctx_t          *tc_ctx;   /**< Pre-populated type context (NULL = create fresh). */
    struct n00b_diag_ctx_s *diag;     /**< Pre-populated diag context (NULL = create fresh). */
};
