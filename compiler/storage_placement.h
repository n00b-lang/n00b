#ifndef N00BB_STORAGE_PLACEMENT_H
#define N00BB_STORAGE_PLACEMENT_H

#include "ir/ir.h"
#include "ir/summary.h"

bool n00bb_place_storage(ir_ctx_t *context, ir_summaries_t *summaries,
                         ir_module_t *module);
bool n00bb_binding_escapes(ir_summaries_t *summaries, ir_func_t *function,
                           const ir_value_t *binding);

#endif
