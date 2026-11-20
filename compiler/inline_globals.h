#ifndef N00BB_INLINE_GLOBALS_H
#define N00BB_INLINE_GLOBALS_H

#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/shape.h"
#include "ir/typelang.h"

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
    const ir_final_layout_t *layout = ir_layout_final_visible(module,
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

static inline th_type *
n00bb_inline_global_element(const ir_global_t *global)
{
    th_type *element = th_resolve(global->type);
    while (element && th_kind_of(element) == TH_SEQUENCE)
        element = th_resolve(th_seq_elem(element));
    return element;
}

#endif
