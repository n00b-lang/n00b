#pragma once

#include "ir/ir.h"
#include "ir/diag.h"
#include "ir/verify.h"
#include "parse/annot_walk.h"
#include "parse/tree_visit.h"

typedef void (*ir_tree_hook_fn)(ncc_tree_walk_t *w,
                                ir_module_t     *m,
                                void            *state);

extern const ir_pass_t ir_pass_annot_scope;
extern const ir_pass_t ir_pass_annot_symbols;
extern const ir_pass_t ir_pass_annot_refine;
extern const ir_pass_t ir_pass_annot_cflow;
extern const ir_pass_t ir_pass_annot_infer;
extern const ir_pass_t ir_pass_symcheck;
extern const ir_pass_t ir_pass_type_diags;

uint32_t ir_import_type_errors(ir_module_t *m, ir_diags_t *d);

size_t           ir_tree_pass_count(void);
const ir_pass_t *ir_tree_pass_at(size_t i);

ir_pass_result_t ir_tree_pass_run(const ir_pass_t *p,
                                  ir_module_t     *m,
                                  ir_tree_hook_fn  hook,
                                  void            *hook_state,
                                  ir_diags_t      *d);

bool ir_tree_passes_run(ir_module_t *m, ir_diags_t *d);

ncc_annot_result_t *ir_module_tree_annots(ir_module_t *m);
