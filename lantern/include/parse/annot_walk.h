#pragma once

#include "parse/types.h"
#include "parse/tree.h"
#include "parse/grammar.h"
#include "parse/symtab.h"
#include "parse/cf_label.h"
#include "parse/tree_visit.h"
#include "lib/diagnostic.h"
#include "parse/infer.h"

typedef struct ir_module_t ir_module_t;

typedef void (*parse_annot_node_callback_fn)(parse_visit_t *visit,
                                             bool entering,
                                             void *context);


typedef parse_sym_entry_t *parse_sym_entry_ptr_t;
core_list_decl(parse_sym_entry_ptr_t);
core_list_decl(parse_tree_ptr_t);

typedef struct {
    parse_symtab_t *symtab;
    core_dict_t   *cf_labels;
    core_list_t(parse_sym_entry_ptr_t) params;
    core_list_t(parse_sym_entry_ptr_t) shadowed;
    core_list_t(parse_tree_ptr_t) undefined;
    void *impl;
    bool  borrowed_symtab;
    bool  inference_failed;
    core_diagnostics_t *diagnostics;
    const core_translation_unit_t *diagnostic_unit;
    parse_annot_node_callback_fn on_node;
    void *on_node_context;
    parse_infer_type_node_resolver_fn type_node_resolver;
    void *type_node_resolver_context;
} parse_annot_result_t;

typedef enum : uint8_t {
    PARSE_ANNOT_PASS_SCOPE,
    PARSE_ANNOT_PASS_SYMBOLS,
    PARSE_ANNOT_PASS_REFINE,
    PARSE_ANNOT_PASS_CFLOW,
    PARSE_ANNOT_PASS_INFER,
    PARSE_ANNOT_PASS_INITIAL_SYMBOLS,
    PARSE_ANNOT_PASS_ALL,
} parse_annot_pass_t;

core_string_t parse_annot_pass_name(parse_annot_pass_t p);

parse_annot_result_t *parse_annot_result_new(parse_symtab_t *borrowed);
void parse_annot_result_set_node_callback(
    parse_annot_result_t *result, parse_annot_node_callback_fn callback,
    void *context);
/* A null result uses the type already attached to the node, if any. */
void parse_annot_result_set_type_node_resolver(
    parse_annot_result_t *result,
    parse_infer_type_node_resolver_fn resolver,
    void *context);
th_type *parse_annot_resolve_type_node(parse_annot_result_t *result,
                                       th_ctx *types,
                                       parse_grammar_t *grammar,
                                       parse_tree_t *node);
void parse_annot_result_set_diagnostics(
    parse_annot_result_t *result, core_diagnostics_t *diagnostics,
    const core_translation_unit_t *unit);
size_t parse_annot_resolve_undefined(parse_annot_result_t *result);

void parse_annot_register(parse_tree_walk_t *w, parse_annot_result_t *out,
                        th_ctx *types, ir_module_t *m);

void parse_annot_register_pass(parse_tree_walk_t    *w,
                             parse_annot_result_t *out,
                             th_ctx             *types,
                             ir_module_t        *m,
                             parse_annot_pass_t    which);

parse_annot_result_t *parse_annot_walk_tree(parse_grammar_t    *g,
                                        parse_tree_t *tree,
                                        th_ctx           *types,
                                        ir_module_t      *m);

void parse_annot_result_free(parse_annot_result_t *r);

parse_cf_label_t *parse_cf_label_lookup(parse_annot_result_t *r,
                                    parse_tree_t   *node);
