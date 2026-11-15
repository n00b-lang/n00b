#ifndef N00BB_SEQUENCE_TRANSFER_H
#define N00BB_SEQUENCE_TRANSFER_H

#include "ir/ir.h"
#include "ir/shape.h"
#include "lib/buffer.h"

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
// parameters as "index:extents" pairs, separated by commas, where extents
// are the dimensions' sizes joined by 'x' (`0:4,2:3x4`).
#define N00BB_FIXED_REFERENCES_KEY CORE_STRING_STATIC("n00bb.fixed-references")
#define N00BB_FIXED_REFERENCE_MAX_RANK 8

// The extents of parameter `index` of `function` when it is passed as its
// elements: stores up to N00BB_FIXED_REFERENCE_MAX_RANK of them in
// `extents` (which may be null) and returns the rank, else 0.
static inline uint32_t
n00bb_fixed_reference_shape(const ir_func_t *function, uint32_t index,
                            uint64_t *extents)
{
    ir_annot_t *list = function ? ir_annot_get(
        (ir_annots_t *)&function->annots, N00BB_FIXED_REFERENCES_KEY)
        : nullptr;
    if (!list || list->kind != IR_ANNOT_STR) return 0;
    const char *at = list->s.data;
    const char *end = at + list->s.u8_bytes;
    while (at < end) {
        uint64_t parameter = 0;
        while (at < end && *at >= '0' && *at <= '9')
            parameter = parameter * 10 + (uint64_t)(*at++ - '0');
        if (at >= end || *at++ != ':') return 0;
        uint32_t rank = 0;
        while (true) {
            uint64_t extent = 0;
            while (at < end && *at >= '0' && *at <= '9')
                extent = extent * 10 + (uint64_t)(*at++ - '0');
            if (rank == N00BB_FIXED_REFERENCE_MAX_RANK) return 0;
            if (parameter == index && extents) extents[rank] = extent;
            rank++;
            if (at < end && *at == 'x') {
                at++;
                continue;
            }
            break;
        }
        if (parameter == index) return rank;
        if (at < end && *at++ != ',') return 0;
    }
    return 0;
}

// Writes the sizes `extents` as text (`4`, `3x4`) to `out`.
static inline void
n00bb_extents_text(uint32_t rank, const uint64_t *extents, core_buffer_t *out)
{
    for (uint32_t d = 0; d < rank; d++)
        core_buffer_printf(out, "%s%llu", d ? "x" : "",
                           (unsigned long long)extents[d]);
}

// Writes the sizes of `shape` as text to `out` when it has a size for every
// dimension; returns whether it does.
static inline bool
n00bb_shape_size_text(const ir_shape_t *shape, core_buffer_t *out)
{
    uint32_t rank = shape ? ir_shape_rank(shape) : 0;
    uint64_t extents[N00BB_FIXED_REFERENCE_MAX_RANK];
    if (!rank || rank > N00BB_FIXED_REFERENCE_MAX_RANK) return false;
    for (uint32_t d = 0; d < rank; d++) {
        ir_shape_extent_t extent;
        if (!ir_shape_extent(shape, d, &extent)
            || extent.kind != IR_SHAPE_EXTENT_STATIC) return false;
        extents[d] = extent.static_value;
    }
    n00bb_extents_text(rank, extents, out);
    return true;
}

// The element count of parameter `index` of `function` when it is passed
// as its elements (every dimension's size multiplied), else 0.
static inline uint64_t
n00bb_fixed_reference_count(const ir_func_t *function, uint32_t index)
{
    uint64_t extents[N00BB_FIXED_REFERENCE_MAX_RANK];
    uint32_t rank = n00bb_fixed_reference_shape(function, index, extents);
    uint64_t total = rank ? 1 : 0;
    for (uint32_t d = 0; d < rank; d++) total *= extents[d];
    return total;
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
