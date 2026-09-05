#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/symtab.h"
#include "parse/cf_label.h"
#include "parse/tree_visit.h"

// A type annotation names its type either as a parse subtree (@declares($n,$t),
// @field, @method, @type($n)) or as typelang surface text (@type($n,"spec")).
// Both hand back a finished type descriptor: the walk stores that and never
// keeps the subtree or the spelling around to be re-derived later.
typedef tdesc_t *(*ncc_type_from_tree_fn)(void             *ctx,
                                                  ncc_grammar_t    *g,
                                                  ncc_parse_tree_t *node);
typedef tdesc_t *(*ncc_type_from_spec_fn)(void        *ctx,
                                                  ncc_string_t spec);

typedef struct {
    ncc_type_from_tree_fn from_tree;
    ncc_type_from_spec_fn from_spec;
    void                 *ctx;
} ncc_type_binder_t;

typedef ncc_sym_entry_t *ncc_sym_entry_ptr_t;
ncc_list_decl(ncc_sym_entry_ptr_t);

typedef struct {
    ncc_symtab_t *symtab;
    // Parse node -> ncc_cf_label_t *.
    ncc_dict_t   *cf_labels;
    ncc_list_t(ncc_sym_entry_ptr_t) params;
    ncc_list_t(ncc_sym_entry_ptr_t) shadowed;
    // State the registered visitors carry between phases. Owned here so that
    // freeing the result frees it, whoever owned the walk.
    void *impl;
} ncc_annot_result_t;

// Register the symbol-table and control-flow visitors on an existing walk, so
// a caller can add its own visitors alongside them. @p binder may be null, in
// which case type annotations register their symbol but bind no type.
void ncc_annot_register(ncc_tree_walk_t *w, ncc_annot_result_t *out,
                        const ncc_type_binder_t *binder);

// Convenience: build a walk carrying only the annotation visitors, run it, and
// return what it produced. The caller owns the result and its symbol table.
ncc_annot_result_t *ncc_annot_walk_tree(ncc_grammar_t           *g,
                                        ncc_parse_tree_t        *tree,
                                        const ncc_type_binder_t *binder);

void ncc_annot_result_free(ncc_annot_result_t *r);

ncc_cf_label_t *ncc_cf_label_lookup(ncc_annot_result_t *r,
                                    ncc_parse_tree_t   *node);
