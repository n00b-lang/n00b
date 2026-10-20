#ifndef N00BB_SEQUENCE_TRANSFER_H
#define N00BB_SEQUENCE_TRANSFER_H

#include "ir/ir.h"

static inline bool
n00bb_sequence_transfers_storage(const ir_value_t *source)
{
    if (!source) return false;
    if (source->kind == IR_VAL_PARAM && source->owner
        && source->index < th_func_nparams(source->owner->type)) {
        const th_param *formal = th_func_param(source->owner->type,
                                               source->index);
        return formal && formal->mode == TH_INOUT;
    }
    if (!source->def) return false;
    return source->def->op == IR_OP_AGGREGATE
        || source->def->op == IR_OP_ARRAY_NEW
        || source->def->op == IR_OP_CONSUME
        || source->def->op == IR_OP_CALL
        || source->def->op == IR_OP_CALL_OUTPUT;
}

static inline bool
n00bb_return_transfers_storage(const ir_func_t *function,
                               const ir_value_t *source)
{
    if (n00bb_sequence_transfers_storage(source)) return true;
    if (!function || !source || source->kind != IR_VAL_PARAM
        || source->index >= th_func_nparams(function->type)) return false;
    const th_param *formal = th_func_param(function->type, source->index);
    return formal && formal->mode == TH_CONSUME;
}

#endif
