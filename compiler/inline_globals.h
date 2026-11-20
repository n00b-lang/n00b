#ifndef N00BB_INLINE_GLOBALS_H
#define N00BB_INLINE_GLOBALS_H

#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/shape.h"
#include "ir/typelang.h"

// A module variable declared as a fixed-size array keeps its elements
// inline: its storage is the array value (the data and descriptor
// pointers) followed by the elements, which the data pointer addresses. A
// multi-dimensional one, with a size for every dimension, is one
// rectangular block of elements. Its storage never changes, since whole
// assignments to a fixed-size array copy into it and `modify` and `out`
// parameters are given its elements, so the module that defines it can
// address the elements directly. The static storage (abi_storage) and the
// code (the backend) both decide here, from the variable alone.
//
// The elements start 16 bytes in, so they may need at most 16-byte
// alignment; packed bits (`bool`, integers of odd widths) start at that
// byte's first bit. Arrays whose innermost elements are arrays (a dimension
// without a size) keep their elements apart.
//
// Returns the element count of the whole block, or 0 for any other
// variable.
static inline uint64_t
n00bb_inline_global_count(ir_module_t *module, const ir_global_t *global)
{
    if (!global || global->is_extern || !global->shape
        || ir_global_storage_duration(global) != IR_STORAGE_DURATION_MODULE)
        return 0;
    uint32_t rank = ir_shape_rank(global->shape);
    th_type *element = th_resolve(global->type);
    uint64_t total = 1, outer = 0;
    for (uint32_t d = 0; d < rank; d++) {
        ir_shape_extent_t extent;
        if (!element || th_kind_of(element) != TH_SEQUENCE
            || !ir_shape_extent(global->shape, d, &extent)
            || extent.kind != IR_SHAPE_EXTENT_STATIC || !extent.static_value
            || total > UINT64_MAX / extent.static_value) return 0;
        if (!d) outer = extent.static_value;
        total *= extent.static_value;
        element = th_resolve(th_seq_elem(element));
    }
    if (!rank || !element || th_kind_of(element) == TH_SEQUENCE
        || !th_is_closed(ir_module_types(module), element)) return 0;
    const ir_final_layout_t *layout = ir_layout_final(module,
        element, ir_module_target(module));
    if (!layout || !layout->alignment_bytes || layout->alignment_bytes > 16)
        return 0;
    const ir_global_init_value_t *initializer =
        global->has_static_initializer ? global->static_initializer
                                       : nullptr;
    if (initializer && initializer->kind != IR_GLOBAL_INIT_PHASE_RESULT
        && initializer->kind != IR_GLOBAL_INIT_NIL
        && (initializer->kind != IR_GLOBAL_INIT_SEQUENCE
            || initializer->aggregate.count != outer))
        return 0;
    return total;
}

// The innermost element type of an inline global's array.
static inline th_type *
n00bb_inline_global_element(const ir_global_t *global)
{
    th_type *element = th_resolve(global->type);
    while (element && th_kind_of(element) == TH_SEQUENCE)
        element = th_resolve(th_seq_elem(element));
    return element;
}

#endif
