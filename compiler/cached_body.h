#ifndef N00BB_CACHED_BODY_H
#define N00BB_CACHED_BODY_H

#include "ir/ir.h"

static inline ir_func_t *
n00bb_cached_body(ir_func_t *function)
{
    if (!function || function->n_blocks) return function;
    ir_annot_t *body = ir_annot_get(&function->annots,
                                    CORE_STRING_STATIC("n00bb.cached_body"));
    return body && body->kind == IR_ANNOT_VALUE && body->v
        && body->v->kind == IR_VAL_FUNC ? body->v->owner : function;
}

#endif
