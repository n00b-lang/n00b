#include <string.h>

#include "ir/shape.h"

struct ir_shape_t {
    th_type *element_type;
    uint32_t rank;
    ir_shape_extent_t *extents;
    ir_shape_measure_t element_count;
    ir_shape_measure_t *strides;
    uint32_t flat_prefix_rank;
    bool classification_known;
};

bool
ir_instr_set_array_init(ir_ctx_t *ctx, ir_instr_t *instruction,
                        ir_array_init_t initialization)
{
    if (!ctx || !instruction
        || (instruction->op != IR_OP_ARRAY_NEW
            && instruction->op != IR_OP_ARRAY_NEW_USING))
        return false;
    const char *name = nullptr;
    switch (initialization) {
    case IR_ARRAY_INIT_LANGUAGE_ZERO:
        name = "language_zero";
        break;
    case IR_ARRAY_INIT_UNINITIALIZED:
        name = "uninitialized";
        break;
    default:
        return false;
    }
    return ir_annot_set(ctx, &instruction->annots, (ir_annot_t){
        .key = "ir.array.element_initialization",
        .kind = IR_ANNOT_STR,
        .s = name,
    });
}

bool
ir_instr_array_init(const ir_instr_t *instruction,
                    ir_array_init_t *initialization)
{
    if (!instruction
        || (instruction->op != IR_OP_ARRAY_NEW
            && instruction->op != IR_OP_ARRAY_NEW_USING)
        || !initialization) return false;
    ir_annot_t *annotation = ir_annot_get(
        (ir_annots_t *)&instruction->annots,
        "ir.array.element_initialization");
    if (!annotation || annotation->kind != IR_ANNOT_STR || !annotation->s)
        return false;
    if (!strcmp(annotation->s, "language_zero")) {
        *initialization = IR_ARRAY_INIT_LANGUAGE_ZERO;
        return true;
    }
    if (!strcmp(annotation->s, "uninitialized")) {
        *initialization = IR_ARRAY_INIT_UNINITIALIZED;
        return true;
    }
    return false;
}

static bool
shape_measure_valid(ir_shape_measure_t measure)
{
    switch (measure.kind) {
    case IR_SHAPE_MEASURE_UNKNOWN:
    case IR_SHAPE_MEASURE_STATIC:
        return measure.value == nullptr && measure.expression == nullptr
            && measure.producer == nullptr;
    case IR_SHAPE_MEASURE_COMPTIME:
        return (measure.value != nullptr || measure.expression != nullptr
                || measure.producer != nullptr)
            && (!measure.producer
                || ir_func_execution_phase(measure.producer)
                    == IR_PHASE_COMPTIME);
    case IR_SHAPE_MEASURE_RUNTIME:
    case IR_SHAPE_MEASURE_SYMBOLIC:
        return !measure.producer
            && (measure.value != nullptr || measure.expression != nullptr);
    }
    return false;
}

ir_shape_t *
ir_shape_new(ir_ctx_t *ctx, th_type *element_type, uint32_t rank)
{
    size_t extent_bytes, measure_bytes;
    if (!ctx || !element_type || !rank
        || __builtin_mul_overflow((size_t)rank,
                                  sizeof(ir_shape_extent_t), &extent_bytes)
        || __builtin_mul_overflow((size_t)rank,
                                  sizeof(ir_shape_measure_t), &measure_bytes))
        return nullptr;
    ir_shape_t *shape = ir_alloc(ctx, sizeof(*shape));
    if (!shape) return nullptr;
    shape->extents = ir_alloc(ctx, extent_bytes);
    shape->strides = ir_alloc(ctx, measure_bytes);
    if (!shape->extents || !shape->strides) return nullptr;
    shape->element_type = element_type;
    shape->rank = rank;
    return shape;
}

uint32_t
ir_shape_rank(const ir_shape_t *shape)
{
    return shape ? shape->rank : 0;
}

th_type *
ir_shape_element_type(const ir_shape_t *shape)
{
    return shape ? shape->element_type : nullptr;
}

bool
ir_shape_set_extent(ir_shape_t *shape, uint32_t dimension,
                    ir_shape_extent_t extent)
{
    if (!shape || dimension >= shape->rank) return false;
    switch (extent.kind) {
    case IR_SHAPE_EXTENT_UNRESOLVED:
        if (extent.value || extent.expression || extent.producer
            || extent.producer_identifier) return false;
        break;
    case IR_SHAPE_EXTENT_STATIC:
        if (extent.value || extent.expression || extent.producer
            || extent.producer_identifier)
            return false;
        break;
    case IR_SHAPE_EXTENT_COMPTIME:
        if (!extent.value && !extent.expression && !extent.producer
            && !extent.producer_identifier)
            return false;
        if (extent.producer
            && ir_func_execution_phase(extent.producer)
                != IR_PHASE_COMPTIME) return false;
        break;
    case IR_SHAPE_EXTENT_RUNTIME:
        if ((!extent.value && !extent.expression) || extent.producer
            || extent.producer_identifier)
            return false;
        break;
    default:
        return false;
    }
    shape->extents[dimension] = extent;
    return true;
}

bool
ir_shape_extent(const ir_shape_t *shape, uint32_t dimension,
                ir_shape_extent_t *out)
{
    if (!shape || dimension >= shape->rank || !out) return false;
    *out = shape->extents[dimension];
    return true;
}

bool
ir_shape_set_storage(ir_shape_t *shape, uint32_t flat_prefix_rank,
                     bool classification_known)
{
    if (!shape || flat_prefix_rank > shape->rank
        || (!classification_known && flat_prefix_rank != 0)) return false;
    shape->flat_prefix_rank = flat_prefix_rank;
    shape->classification_known = classification_known;
    return true;
}

bool
ir_shape_storage(const ir_shape_t *shape, ir_shape_storage_t *out)
{
    if (!shape || !out) return false;
    *out = (ir_shape_storage_t){
        .rank = shape->rank,
        .flat_prefix_rank = shape->flat_prefix_rank,
        .classification_known = shape->classification_known,
    };
    return true;
}

bool
ir_shape_set_element_count(ir_shape_t *shape, ir_shape_measure_t count)
{
    if (!shape || !shape_measure_valid(count)
        || (count.kind == IR_SHAPE_MEASURE_STATIC && !count.static_value))
        return false;
    shape->element_count = count;
    return true;
}

bool
ir_shape_element_count(const ir_shape_t *shape, ir_shape_measure_t *out)
{
    if (!shape || !out) return false;
    *out = shape->element_count;
    return true;
}

bool
ir_shape_set_stride(ir_shape_t *shape, uint32_t dimension,
                    ir_shape_measure_t stride)
{
    if (!shape || dimension >= shape->rank || !shape_measure_valid(stride)
        || (stride.kind == IR_SHAPE_MEASURE_STATIC && !stride.static_value))
        return false;
    shape->strides[dimension] = stride;
    return true;
}

bool
ir_shape_stride(const ir_shape_t *shape, uint32_t dimension,
                ir_shape_measure_t *out)
{
    if (!shape || dimension >= shape->rank || !out) return false;
    *out = shape->strides[dimension];
    return true;
}

ir_shape_t *
ir_shape_suffix(ir_ctx_t *ctx, const ir_shape_t *shape, uint32_t dimensions)
{
    if (!ctx || !shape || dimensions >= shape->rank) return nullptr;
    uint32_t rank = shape->rank - dimensions;
    ir_shape_t *suffix = ir_shape_new(ctx, shape->element_type, rank);
    if (!suffix) return nullptr;
    for (uint32_t i = 0; i < rank; i++) {
        suffix->extents[i] = shape->extents[dimensions + i];
        suffix->strides[i] = shape->strides[dimensions + i];
    }
    ir_shape_storage_t storage;
    if (!ir_shape_storage(shape, &storage)) return nullptr;
    uint32_t flat_prefix = storage.flat_prefix_rank > dimensions
        ? storage.flat_prefix_rank - dimensions : 0;
    if (!ir_shape_set_storage(suffix, flat_prefix,
                              storage.classification_known)) return nullptr;
    if (!dimensions) {
        suffix->element_count = shape->element_count;
    }
    else if (dimensions <= storage.flat_prefix_rank) {
        suffix->element_count = shape->strides[dimensions - 1];
    }
    return suffix;
}

bool
ir_value_set_shape(ir_value_t *value, ir_shape_t *shape)
{
    if (!value || !shape) return false;
    value->shape = shape;
    return true;
}

ir_shape_t *
ir_value_shape(const ir_value_t *value)
{
    if (!value) return nullptr;
    if (value->shape) return value->shape;
    if (value->global) return ir_global_shape(value->global);
    if (value->local) return ir_local_shape(value->local);
    return nullptr;
}

bool
ir_local_set_shape(ir_local_t *local, ir_shape_t *shape)
{
    if (!local || !shape) return false;
    local->shape = shape;
    if (local->value) local->value->shape = shape;
    return true;
}

ir_shape_t *
ir_local_shape(const ir_local_t *local)
{
    return local ? local->shape : nullptr;
}

bool
ir_global_set_shape(ir_global_t *global, ir_shape_t *shape)
{
    if (!global || !shape) return false;
    global->shape = shape;
    return true;
}

ir_shape_t *
ir_global_shape(const ir_global_t *global)
{
    return global ? global->shape : nullptr;
}
