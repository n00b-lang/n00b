#ifndef N00BB_COMPILER_BUFFER_BORROW_H
#define N00BB_COMPILER_BUFFER_BORROW_H

#include "ir/ir.h"
#include "ir/diag.h"
#include "ir/summary.h"

bool n00bb_buffer_borrow_pass(ir_module_t *module, ir_summaries_t *summaries,
                              ir_diags_t *diagnostics);
bool n00bb_buffer_flow(void *context, ir_summaries_t *summaries,
                       ir_func_t *function, const ir_sflow_t *previous,
                       ir_sflow_t *out);
bool n00bb_buffer_native_flow(const ir_func_t *function, ir_sflow_t *out);

#endif
