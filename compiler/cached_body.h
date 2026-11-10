#ifndef N00BB_CACHED_BODY_H
#define N00BB_CACHED_BODY_H

#include "ir/ir.h"
#include "ir/operands.h"
#include "qualified_names.h"

static inline core_string_t
n00bb_native_symbol(ir_func_t *function)
{
    ir_annot_t *native = function ? ir_annot_get(&function->annots,
        CORE_STRING_STATIC("n00bb.abi.native")) : nullptr;
    return native && native->kind == IR_ANNOT_STR ? native->s
                                                  : (core_string_t){0};
}

static inline ir_func_t *
n00bb_cached_body(ir_func_t *function)
{
    if (!function || function->n_blocks) return function;
    ir_annot_t *body = ir_annot_get(&function->annots,
                                    CORE_STRING_STATIC("n00bb.cached_body"));
    return body && body->kind == IR_ANNOT_VALUE && body->v
        && body->v->kind == IR_VAL_FUNC ? body->v->owner : function;
}

static inline ir_func_t *
n00bb_direct_call_target(const ir_instr_t *instruction)
{
    return n00bb_cached_body(ir_call_function(instruction));
}

static inline ir_func_t *
n00bb_open_call(const ir_instr_t *call, core_string_t root)
{
    ir_func_t *target = call && call->n_operands == 2
        ? ir_call_function(call) : nullptr;
    return target && n00bb_names_function(root, target->name) ? target
                                                              : nullptr;
}

#endif
