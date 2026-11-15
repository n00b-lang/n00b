#ifndef N00BB_SEQUENCE_TRANSFER_H
#define N00BB_SEQUENCE_TRANSFER_H

#include "ir/ir.h"

// A whole-array store into a fixed-size array (a variable, field or
// parameter declared with a size): it copies the elements into the
// existing storage after checking the shape, and never rebinds it.
#define N00BB_FIXED_ARRAY_STORE_KEY CORE_STRING_STATIC("n00bb.fixed-array-store")

static inline bool
n00bb_fixed_array_store(const ir_instr_t *store)
{
    return store && ir_annot_get((ir_annots_t *)&store->annots,
                                 N00BB_FIXED_ARRAY_STORE_KEY);
}

// A `modify` or `out` parameter declared as a fixed-size array is passed as
// the caller's elements, not as a reference to an array value: the callee
// writes into that storage and can't rebind it. A function lists such
// parameters as "index:count" pairs, separated by commas.
#define N00BB_FIXED_REFERENCES_KEY CORE_STRING_STATIC("n00bb.fixed-references")

// The element count of parameter `index` of `function` when it is passed
// as its elements, else 0.
static inline uint64_t
n00bb_fixed_reference_count(const ir_func_t *function, uint32_t index)
{
    ir_annot_t *list = function ? ir_annot_get(
        (ir_annots_t *)&function->annots, N00BB_FIXED_REFERENCES_KEY)
        : nullptr;
    if (!list || list->kind != IR_ANNOT_STR) return 0;
    const char *at = list->s.data;
    const char *end = at + list->s.u8_bytes;
    while (at < end) {
        uint64_t parameter = 0, count = 0;
        while (at < end && *at >= '0' && *at <= '9')
            parameter = parameter * 10 + (uint64_t)(*at++ - '0');
        if (at >= end || *at++ != ':') return 0;
        while (at < end && *at >= '0' && *at <= '9')
            count = count * 10 + (uint64_t)(*at++ - '0');
        if (parameter == index) return count;
        if (at < end && *at++ != ',') return 0;
    }
    return 0;
}

static inline bool
n00bb_sequence_transfers_storage(const ir_value_t *source)
{
    if (!source || !source->def) return false;
    return source->def->op == IR_OP_AGGREGATE
        || source->def->op == IR_OP_ARRAY_NEW
        || source->def->op == IR_OP_CONSUME
        || source->def->op == IR_OP_CALL;
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
