#include "ir/libir.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static void
test_shape_metadata(void)
{
    int source_expression;
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("shape"), &ir_profile_bce,
                                        S("test"));
    th_type *i32 = module ? ir_type_parse(module, S("i32")) : nullptr;
    th_type *array_type = module ? ir_type_parse(module, S("array[i32]"))
                                 : nullptr;
    ir_global_t *global = module
        ? ir_global_add(module, S("matrix"), array_type) : nullptr;
    ir_func_t *function = module
        ? ir_func_add(module, S("bounds"), ir_type_parse(module,
            S("(i64) -> nil")), IR_LINK_INTERNAL) : nullptr;
    ir_value_t *runtime_bound = function
        ? ir_param_add(function, S("runtime_bound"), ir_type_parse(module,
            S("i64"))) : nullptr;
    ir_shape_t *shape = ir_shape_new(ctx, i32, 2);
    uint64_t type_id_before = module && global
        ? ir_type_id(module, global->type) : 0;
    bool built = module && i32 && array_type && global && function
        && runtime_bound && shape
        && ir_shape_set_extent(shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = 5,
        })
        && ir_shape_set_extent(shape, 1, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_RUNTIME,
            .expression = &source_expression,
            .value = runtime_bound,
        })
        && ir_shape_set_element_count(shape, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
            .value = runtime_bound,
        })
        && ir_shape_set_stride(shape, 0, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
            .value = runtime_bound,
        })
        && ir_shape_set_storage(shape, 0, false)
        && ir_global_set_shape(global, shape);
    ir_shape_storage_t storage;
    ir_shape_extent_t first, second;
    ir_shape_measure_t count, stride;
    ir_value_t *global_address = module && global
        ? ir_global_address(module, global) : nullptr;
    bool retained = built && global_address
        && ir_value_shape(global_address) == shape
        && ir_shape_extent(shape, 0, &first)
        && first.kind == IR_SHAPE_EXTENT_STATIC
        && first.static_value == 5
        && ir_shape_extent(shape, 1, &second)
        && second.kind == IR_SHAPE_EXTENT_RUNTIME
        && second.expression == &source_expression
        && second.value == runtime_bound
        && ir_shape_element_count(shape, &count)
        && count.kind == IR_SHAPE_MEASURE_RUNTIME
        && count.value == runtime_bound
        && ir_shape_stride(shape, 0, &stride)
        && stride.kind == IR_SHAPE_MEASURE_RUNTIME
        && stride.value == runtime_bound
        && ir_shape_storage(shape, &storage)
        && storage.rank == 2 && !storage.classification_known
        && storage.flat_prefix_rank == 0;
    check("array shape retains phase-specific extent and measure references",
          retained);
    check("shape metadata does not alter the language type",
          built && ir_type_eq(module, global->type, array_type)
              && ir_type_id(module, global->type) == type_id_before
              && ir_shape_element_type(shape) == i32
              && ir_shape_rank(shape) == 2);

    ir_shape_t *empty = ir_shape_new(ctx, i32, 1);
    ir_shape_extent_t empty_extent;
    bool empty_shape = empty
        && ir_shape_set_extent(empty, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = 0,
        })
        && ir_shape_extent(empty, 0, &empty_extent)
        && empty_extent.kind == IR_SHAPE_EXTENT_STATIC
        && empty_extent.static_value == 0;
    check("shape descriptors can represent empty sequence values",
          empty_shape);

    ir_shape_t *materialized = ir_shape_new(ctx, i32, 1);
    bool static_value_retained = materialized
        && ir_shape_set_extent(materialized, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = 3,
            .value = runtime_bound,
        })
        && ir_shape_extent(materialized, 0, &empty_extent)
        && empty_extent.static_value == 3
        && empty_extent.value == runtime_bound;
    check("static extents can retain their materialized SSA value",
          static_value_retained);

    ir_func_t *locals = module
        ? ir_func_add(module, S("local"), ir_type_parse(module, S("() -> nil")),
                      IR_LINK_INTERNAL) : nullptr;
    ir_local_t *local = locals
        ? ir_local_add(locals, S("view"), array_type) : nullptr;
    ir_value_t *local_value = locals && local
        ? ir_local_value(locals, local) : nullptr;
    ir_instr_t *move = locals
        ? ir_instr_add(ir_block_add(locals, S("entry")), IR_OP_COPY) : nullptr;
    bool has_derived = move
        && ir_instr_set_dest(move, S("derived"), array_type);
    ir_value_t *derived = has_derived ? move->dest : nullptr;
    has_derived = has_derived && ir_instr_operand_value(move, local_value);
    bool projected = local && local_value && derived
        && ir_local_set_shape(local, shape)
        && ir_value_shape(local_value) == shape
        && ir_value_set_shape(derived, shape)
        && ir_value_shape(derived) == shape;
    check("local bindings and SSA values can project canonical shape metadata",
          projected);

    ir_shape_t *matrix = ir_shape_new(ctx, i32, 3);
    bool matrix_built = matrix
        && ir_shape_set_extent(matrix, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC, .static_value = 2})
        && ir_shape_set_extent(matrix, 1, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC, .static_value = 3})
        && ir_shape_set_extent(matrix, 2, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC, .static_value = 4})
        && ir_shape_set_storage(matrix, 3, true)
        && ir_shape_set_element_count(matrix, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_STATIC, .static_value = 24})
        && ir_shape_set_stride(matrix, 0, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_STATIC, .static_value = 12})
        && ir_shape_set_stride(matrix, 1, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_STATIC, .static_value = 4})
        && ir_shape_set_stride(matrix, 2, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_STATIC, .static_value = 1});
    ir_shape_t *row = matrix_built ? ir_shape_suffix(ctx, matrix, 1) : nullptr;
    ir_shape_extent_t row_first, row_second;
    ir_shape_measure_t row_count, row_stride;
    ir_shape_storage_t row_storage;
    bool rectangular_suffix = row
        && ir_shape_rank(row) == 2
        && ir_shape_extent(row, 0, &row_first)
        && row_first.kind == IR_SHAPE_EXTENT_STATIC
        && row_first.static_value == 3
        && ir_shape_extent(row, 1, &row_second)
        && row_second.static_value == 4
        && ir_shape_element_count(row, &row_count)
        && row_count.kind == IR_SHAPE_MEASURE_STATIC
        && row_count.static_value == 12
        && ir_shape_stride(row, 0, &row_stride)
        && row_stride.static_value == 4
        && ir_shape_storage(row, &row_storage)
        && row_storage.rank == 2 && row_storage.flat_prefix_rank == 2;
    check("rectangular shape projection retains suffix extents and stride count",
          rectangular_suffix);
    bool projection_independent = row
        && ir_shape_set_extent(row, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC, .static_value = 7})
        && ir_shape_extent(matrix, 1, &row_first)
        && row_first.kind == IR_SHAPE_EXTENT_STATIC
        && row_first.static_value == 3
        && !ir_shape_suffix(ctx, matrix, 3);
    check("shape suffix is independent and rejects an empty projection",
          projection_independent);

    ir_shape_t *ragged = ir_shape_new(ctx, i32, 2);
    bool ragged_built = ragged
        && ir_shape_set_extent(ragged, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC, .static_value = 2})
        && ir_shape_set_extent(ragged, 1, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_UNRESOLVED})
        && ir_shape_set_storage(ragged, 1, true);
    ir_shape_t *child = ragged_built
        ? ir_shape_suffix(ctx, ragged, 1) : nullptr;
    ir_shape_extent_t child_extent;
    ir_shape_measure_t child_count;
    ir_shape_storage_t child_storage;
    bool ragged_suffix = child
        && ir_shape_rank(child) == 1
        && ir_shape_extent(child, 0, &child_extent)
        && child_extent.kind == IR_SHAPE_EXTENT_UNRESOLVED
        && ir_shape_element_count(child, &child_count)
        && child_count.kind == IR_SHAPE_MEASURE_UNKNOWN
        && ir_shape_storage(child, &child_storage)
        && child_storage.classification_known
        && child_storage.flat_prefix_rank == 0;
    check("ragged shape projection leaves child extent and count unresolved",
          ragged_suffix);
    ir_ctx_free(ctx);
}

static void
test_invalid_shape_metadata(void)
{
    ir_ctx_t *ctx = ir_ctx_new();
    ir_module_t *module = ir_module_new(ctx, S("invalid_shape"),
                                        &ir_profile_bce, S("test"));
    th_type *i32 = module ? ir_type_parse(module, S("i32")) : nullptr;
    ir_shape_t *shape = ir_shape_new(ctx, i32, 1);
    ir_value_t *value = nullptr;
    ir_shape_t *zero_rank = ir_shape_new(ctx, i32, 0);
    bool zero_extent_valid = shape
        && ir_shape_set_extent(shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .static_value = 0,
        });
    int expression = 0;
    bool rejected = shape && zero_rank == nullptr
        && zero_extent_valid
        && !ir_shape_set_extent(shape, 0, (ir_shape_extent_t){
            .kind = IR_SHAPE_EXTENT_STATIC,
            .expression = &expression,
        })
        && !ir_shape_set_storage(shape, 2, true)
        && !ir_shape_set_element_count(shape, (ir_shape_measure_t){
            .kind = IR_SHAPE_MEASURE_RUNTIME,
        })
        && !ir_value_set_shape(value, shape);
    check("shape API rejects invalid rank, extent, measure, and attachment",
          rejected);
    ir_ctx_free(ctx);
}

int
main(void)
{
    test_shape_metadata();
    test_invalid_shape_metadata();
    return fails != 0;
}
