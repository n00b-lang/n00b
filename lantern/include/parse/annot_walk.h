#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/symtab.h"
#include "parse/cf_label.h"
#include "parse/tree_visit.h"
#include "diag/diagnostics.h"
#include "th/typehash.h"

typedef void (*parse_annot_node_callback_fn)(parse_visit_t *visit,
                                             bool entering,
                                             void *context);

typedef parse_sym_entry_t *parse_sym_entry_ptr_t;
core_list_decl(parse_sym_entry_ptr_t);
core_list_decl(parse_tree_ptr_t);
typedef parse_cf_label_t *parse_cf_label_ptr_t;
core_list_decl(parse_cf_label_ptr_t);

typedef struct {
    parse_symtab_t *symtab;
    core_dict_t   *cf_labels;
    core_list_t(parse_cf_label_ptr_t) cf_label_order;
    core_list_t(parse_tree_ptr_t) undefined;
    void *impl;
    bool  borrowed_symtab;
    core_diagnostics_t *diagnostics;
    const core_translation_unit_t *diagnostic_unit;
    parse_annot_node_callback_fn on_node;
    void *on_node_context;
} parse_annot_result_t;

parse_annot_result_t *parse_annot_result_new(parse_symtab_t *borrowed);
void parse_annot_result_set_node_callback(
    parse_annot_result_t *result, parse_annot_node_callback_fn callback,
    void *context);
void parse_annot_result_set_diagnostics(
    parse_annot_result_t *result, core_diagnostics_t *diagnostics,
    const core_translation_unit_t *unit);
size_t parse_annot_resolve_undefined(parse_annot_result_t *result);

void parse_annot_register_declarations(parse_tree_walk_t *w,
                                       parse_annot_result_t *out);
void parse_annot_register(parse_tree_walk_t *w, parse_annot_result_t *out);

void parse_annot_result_free(parse_annot_result_t *r);

parse_cf_label_t *parse_cf_label_lookup(parse_annot_result_t *r,
                                    parse_tree_t   *node);
