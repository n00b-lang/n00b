#ifndef N00BB_SPECIALIZE_H
#define N00BB_SPECIALIZE_H

#include "ir/ir.h"
#include "lib/buffer.h"
#include "lib/list.h"

typedef ir_func_t *n00bb_ir_func_ptr_t;
core_list_decl(n00bb_ir_func_ptr_t);

typedef struct {
    ir_func_t *generic;
    ir_func_t *instance;
    th_type *signature;
} n00bb_specialization_t;
core_list_decl(n00bb_specialization_t);

typedef struct {
    ir_module_t *requester;
    ir_func_t *instance;
} n00bb_specialization_request_t;
core_list_decl(n00bb_specialization_request_t);

bool n00bb_specialize_calls(ir_ctx_t *context,
    core_list_t(n00bb_ir_func_ptr_t) *functions,
    core_list_t(n00bb_specialization_t) *instances,
    core_list_t(n00bb_specialization_request_t) *requests,
    core_buffer_t *failure);

#endif
