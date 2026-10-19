#ifndef IR_SHAPE_H
#define IR_SHAPE_H

#include "ir/ir.h"
#include "lib/string.h"

typedef struct ir_shape_t ir_shape_t;

typedef enum : uint8_t {
    IR_ARRAY_INIT_LANGUAGE_ZERO,
    IR_ARRAY_INIT_UNINITIALIZED,
} ir_array_init_t;

bool ir_instr_set_array_init(ir_ctx_t *ctx, ir_instr_t *instruction,
                             ir_array_init_t initialization);
bool ir_instr_array_init(const ir_instr_t *instruction,
                         ir_array_init_t *initialization);

typedef enum : uint8_t {
    IR_SHAPE_EXTENT_UNRESOLVED,
    IR_SHAPE_EXTENT_STATIC,
    IR_SHAPE_EXTENT_COMPTIME,
    IR_SHAPE_EXTENT_RUNTIME,
} ir_shape_extent_kind_t;

typedef struct {
    ir_shape_extent_kind_t kind;
    uint64_t static_value;
    /* Borrowed source-language expression, retained before it has an IR value. */
    const void *expression;
    /* Borrowed result in the full-comptime or runtime IR graph. */
    ir_value_t *value;
    /* Borrowed full-comptime function that produces a deferred extent. */
    ir_func_t *producer;
    /* Stable producer identity when the function is not resident locally. */
    core_string_t producer_identifier;
} ir_shape_extent_t;

typedef enum : uint8_t {
    IR_SHAPE_MEASURE_UNKNOWN,
    IR_SHAPE_MEASURE_STATIC,
    IR_SHAPE_MEASURE_COMPTIME,
    IR_SHAPE_MEASURE_RUNTIME,
    IR_SHAPE_MEASURE_SYMBOLIC,
} ir_shape_measure_kind_t;

typedef struct {
    ir_shape_measure_kind_t kind;
    uint64_t static_value;
    /* Optional symbolic expression when no IR graph value exists yet. */
    const void *expression;
    ir_value_t *value;
    /* Borrowed full-comptime function producing this measure. */
    ir_func_t *producer;
} ir_shape_measure_t;

typedef struct {
    uint32_t rank;
    uint32_t flat_prefix_rank;
    bool classification_known;
} ir_shape_storage_t;

ir_shape_t *ir_shape_new(ir_ctx_t *ctx, th_type *element_type,
                         uint32_t rank);
uint32_t ir_shape_rank(const ir_shape_t *shape);
th_type *ir_shape_element_type(const ir_shape_t *shape);
bool ir_shape_set_extent(ir_shape_t *shape, uint32_t dimension,
                         ir_shape_extent_t extent);
bool ir_shape_extent(const ir_shape_t *shape, uint32_t dimension,
                     ir_shape_extent_t *out);
bool ir_shape_fold_comptime_extent(ir_shape_t *shape, uint32_t dimension,
                                   uint64_t value);
bool ir_shape_set_storage(ir_shape_t *shape, uint32_t flat_prefix_rank,
                          bool classification_known);
bool ir_shape_storage(const ir_shape_t *shape, ir_shape_storage_t *out);
bool ir_shape_set_element_count(ir_shape_t *shape,
                                ir_shape_measure_t element_count);
bool ir_shape_element_count(const ir_shape_t *shape,
                            ir_shape_measure_t *out);
bool ir_shape_set_stride(ir_shape_t *shape, uint32_t dimension,
                         ir_shape_measure_t stride);
bool ir_shape_stride(const ir_shape_t *shape, uint32_t dimension,
                     ir_shape_measure_t *out);
/* Project a nested sequence shape after consuming leading dimensions. */
ir_shape_t *ir_shape_suffix(ir_ctx_t *ctx, const ir_shape_t *shape,
                            uint32_t dimensions);

bool ir_value_set_shape(ir_value_t *value, ir_shape_t *shape);
ir_shape_t *ir_value_shape(const ir_value_t *value);
bool ir_local_set_shape(ir_local_t *local, ir_shape_t *shape);
ir_shape_t *ir_local_shape(const ir_local_t *local);
bool ir_global_set_shape(ir_global_t *global, ir_shape_t *shape);
ir_shape_t *ir_global_shape(const ir_global_t *global);

#endif
