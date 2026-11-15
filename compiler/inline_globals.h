#ifndef N00BB_INLINE_GLOBALS_H
#define N00BB_INLINE_GLOBALS_H

#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/shape.h"
#include "ir/typelang.h"

// A module variable declared as a fixed-size array keeps its elements
// inline: its storage is the array value (the data and descriptor
// pointers) followed by the elements, which the data pointer addresses.
// Its storage never changes, since whole assignments to a fixed-size array
// copy into it and `modify` and `out` parameters are given its elements, so
// the module that defines it can address the elements directly. The static
// storage (abi_storage) and the code (the backend) both decide here, from
// the variable alone.
//
// The elements start 16 bytes in, so they may need at most 16-byte
// alignment. Arrays of arrays, and of packed bits (`bool`, and integers of
// widths other than 8 to 128 bits), keep their elements apart.
//
// Returns the element count, or 0 for any other variable.
static inline uint64_t
n00bb_inline_global_count(ir_module_t *module, const ir_global_t *global)
{
    if (!global || global->is_extern
        || ir_global_storage_duration(global) != IR_STORAGE_DURATION_MODULE)
        return 0;
    th_type *array = th_resolve(global->type);
    th_type *element = array && th_kind_of(array) == TH_SEQUENCE
        ? th_resolve(th_seq_elem(array)) : nullptr;
    if (!element || th_kind_of(element) == TH_SEQUENCE
        || !th_is_closed(ir_module_types(module), element)) return 0;
    ir_machine_t machine = ir_machine_of(module, element);
    if (machine.class == IR_CLASS_BOOL
        || ((machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT)
            && machine.width != 8 && machine.width != 16
            && machine.width != 32 && machine.width != 64
            && machine.width != 128)) return 0;
    const ir_final_layout_t *layout = ir_layout_final_visible(module,
        element, ir_module_target(module));
    if (!layout || !layout->alignment_bytes || layout->alignment_bytes > 16)
        return 0;
    ir_shape_extent_t extent;
    if (!global->shape || ir_shape_rank(global->shape) != 1
        || !ir_shape_extent(global->shape, 0, &extent)
        || extent.kind != IR_SHAPE_EXTENT_STATIC || !extent.static_value)
        return 0;
    const ir_global_init_value_t *initializer =
        global->has_static_initializer ? global->static_initializer
                                       : nullptr;
    if (initializer && initializer->kind != IR_GLOBAL_INIT_PHASE_RESULT
        && initializer->kind != IR_GLOBAL_INIT_NIL
        && (initializer->kind != IR_GLOBAL_INIT_SEQUENCE
            || initializer->aggregate.count != extent.static_value))
        return 0;
    return extent.static_value;
}

#endif
