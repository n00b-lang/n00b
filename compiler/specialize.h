#ifndef N00BB_SPECIALIZE_H
#define N00BB_SPECIALIZE_H

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/list.h"

typedef ir_func_t *n00bb_ir_func_ptr_t;
core_list_decl(n00bb_ir_func_ptr_t);

bool n00bb_specialize_runtime_calls(
    core_list_t(n00bb_ir_func_ptr_t) *reachable, core_buffer_t *failure);

#endif
