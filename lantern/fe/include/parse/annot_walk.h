#pragma once

#include "parse/types.h"
#include "parse/parse_tree.h"
#include "parse/grammar.h"
#include "parse/symtab.h"
#include "parse/cf_label.h"
#include "parse/tree_visit.h"


typedef ncc_sym_entry_t *ncc_sym_entry_ptr_t;
ncc_list_decl(ncc_sym_entry_ptr_t);
ncc_list_decl(ncc_parse_tree_ptr_t);

typedef struct {
    ncc_symtab_t *symtab;
    ncc_dict_t   *cf_labels;
    ncc_list_t(ncc_sym_entry_ptr_t) params;
    ncc_list_t(ncc_sym_entry_ptr_t) shadowed;
    ncc_list_t(ncc_parse_tree_ptr_t) undefined;
    void *impl;
    bool  borrowed_symtab;
} ncc_annot_result_t;

typedef enum : uint8_t {
    NCC_ANNOT_PASS_SCOPE,
    NCC_ANNOT_PASS_SYMBOLS,
    NCC_ANNOT_PASS_REFINE,
    NCC_ANNOT_PASS_CFLOW,
    NCC_ANNOT_PASS_INFER,
    NCC_ANNOT_PASS_ALL,
} ncc_annot_pass_t;

ncc_string_t ncc_annot_pass_name(ncc_annot_pass_t p);

ncc_annot_result_t *ncc_annot_result_new(ncc_symtab_t *borrowed);

void ncc_annot_register(ncc_tree_walk_t *w, ncc_annot_result_t *out,
                        th_ctx *types);

void ncc_annot_register_pass(ncc_tree_walk_t    *w,
                             ncc_annot_result_t *out,
                             th_ctx             *types,
                             ncc_annot_pass_t    which);

ncc_annot_result_t *ncc_annot_walk_tree(ncc_grammar_t           *g,
                                        ncc_parse_tree_t        *tree,
                                        th_ctx *types);

void ncc_annot_result_free(ncc_annot_result_t *r);

ncc_cf_label_t *ncc_cf_label_lookup(ncc_annot_result_t *r,
                                    ncc_parse_tree_t   *node);
