#ifndef TYPES_REFINE_H
#define TYPES_REFINE_H

#include "ir/ir.h"
#include "ir/pdg.h"

typedef void (*ty_unwrap_fn)(void *state, ir_instr_t *unwrap);

size_t ty_refine_options(ir_func_t *f, const ir_pdg_t *g,
                         ty_unwrap_fn on_unwrap, void *state);

#endif
