#ifndef N00BB_COMPILER_BUFFER_BORROW_H
#define N00BB_COMPILER_BUFFER_BORROW_H

#include "ir/ir.h"
#include "ir/diag.h"

bool n00bb_buffer_borrow_pass(ir_module_t *module, ir_diags_t *diagnostics);

#endif
