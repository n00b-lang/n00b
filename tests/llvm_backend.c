#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>

#include "ir/ir.h"
#include "ir/layout.h"
#include "ir/typelang.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "llvm_backend.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"

int
main(void)
{
    bool allocator_root = false;
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++) {
        const n00bb_runtime_native_root_t *root =
            &n00bb_runtime_native_roots[i];
        if (strcmp(root->qualified_name,
            "sys.internal.gc.alloc.allocate_runtime_known_on_mutator")) continue;
        assert(!root->c_alias && root->unmanaged_parameter_count == 1
            && root->unmanaged_parameters[0] == 0);
        allocator_root = true;
    }
    assert(allocator_root);

    ir_ctx_t *context = ir_ctx_new();
    const ir_scalar_t scalars[] = {
        {.type_text = "nil", .class = IR_CLASS_VOID},
        {.type_text = "uint", .class = IR_CLASS_UINT, .width = 64},
        {.type_text = "i128", .class = IR_CLASS_SINT, .width = 128},
        {.type_text = "bool", .class = IR_CLASS_BOOL, .width = 1},
        {.type_text = "f64", .class = IR_CLASS_FLOAT, .width = 64},
    };
    const ir_profile_t profile = {
        .name = "llvm-test", .scalars = scalars, .n_scalars = 5,
    };
    const char *target = "x86_64-unknown-linux-gnu";
    ir_module_t *comptime = ir_module_new(context, "probe$comptime",
                                         &profile, target);
    ir_module_t *runtime = ir_module_new(context, "probe", &profile, target);
    ir_func_t *function = ir_func_add(runtime, "entry",
        ir_type_parse(runtime, "() -> nil"), IR_LINK_INTERNAL);
    assert(function);
    ir_block_t *block = ir_block_add(function, "entry");
    assert(block);
    th_type *wide = ir_type_parse(runtime, "i128");
    th_type *unsigned_type = ir_type_parse(runtime, "uint");
    const ir_final_layout_t unsigned_layout = {
        .intrinsic_bits = 64, .alignment_bytes = 8,
    };
    assert(ir_layout_publish_final(runtime, unsigned_type, target,
                                   &unsigned_layout));
    ir_global_t *thread_counter = ir_global_add(runtime,
                                                "thread_counter", unsigned_type);
    assert(thread_counter && ir_global_set_storage_duration(context,
        thread_counter, IR_STORAGE_DURATION_THREAD));
    ir_global_t *static_global = ir_global_add(runtime,
                                              "static_global", unsigned_type);
    ir_global_t *dynamic_global = ir_global_add(runtime,
                                               "dynamic_global", unsigned_type);
    const ir_global_init_value_t static_value = {
        .kind = IR_GLOBAL_INIT_INT, .integer = {.mag = 42},
    };
    assert(static_global && dynamic_global
        && ir_global_set_static_initializer(runtime, static_global,
                                            &static_value));
    ir_global_t *read_globals[] = {static_global, dynamic_global};
    const char *reader_names[] = {"read_static", "read_dynamic"};
    for (size_t i = 0; i < 2; i++) {
        ir_func_t *reader = ir_func_add(runtime, reader_names[i],
            ir_type_parse(runtime, "() -> uint"), IR_LINK_INTERNAL);
        ir_block_t *entry = reader ? ir_block_add(reader, "entry") : nullptr;
        ir_instr_t *load = entry ? ir_instr_add(entry, IR_OP_COPY) : nullptr;
        ir_instr_t *ret = entry ? ir_instr_add(entry, IR_OP_RETURN) : nullptr;
        assert(load && ret
            && ir_instr_set_dest(load, "global.value", unsigned_type)
            && ir_instr_operand_value(load,
                ir_global_address(runtime, read_globals[i]))
            && ir_instr_operand_value(ret, load->dest));
    }
    ir_local_t *wide_local = ir_local_add(function, "wide", wide);
    ir_instr_t *wide_constant = ir_instr_add(block, IR_OP_CONST);
    assert(wide_local && wide_constant
        && ir_instr_set_dest(wide_constant, "wide.constant", wide)
        && ir_instr_operand_int_lit(wide_constant,
            (ir_lit_t){.mag = (ir_uint_t)1 << 100}));
    ir_instr_t *wide_copy = ir_instr_add(block, IR_OP_COPY);
    assert(wide_copy && ir_instr_set_dest_local(wide_copy, wide_local)
        && ir_instr_operand_value(wide_copy, wide_constant->dest));
    ir_instr_t *left = ir_instr_add(block, IR_OP_CONST);
    ir_instr_t *right = ir_instr_add(block, IR_OP_CONST);
    assert(left && right
        && ir_instr_set_dest(left, "left", unsigned_type)
        && ir_instr_operand_uint(left, UINT64_MAX)
        && ir_instr_set_dest(right, "right", unsigned_type)
        && ir_instr_operand_uint(right, 1));
    ir_instr_t *sum = ir_instr_add(block, IR_OP_ADD);
    assert(sum && ir_instr_set_dest(sum, "sum", unsigned_type)
        && ir_instr_add_mod(sum, "trap")
        && ir_instr_operand_value(sum, left->dest)
        && ir_instr_operand_value(sum, right->dest));
    ir_func_t *helper = ir_func_add(runtime, "helper",
        ir_type_parse(runtime, "(uint) -> uint"), IR_LINK_INTERNAL);
    assert(helper);
    ir_instr_t *call = ir_instr_add(block, IR_OP_CALL);
    assert(call && ir_instr_set_dest(call, "called", unsigned_type)
        && ir_instr_operand_value(call, ir_func_address(runtime, helper))
        && ir_instr_operand_value(call, right->dest)
        && ir_instr_set_call_signature(call, helper->type));
    assert(ir_instr_add(block, IR_OP_RETURN));
    ir_value_t *parameter = ir_param_add(helper, "value", unsigned_type);
    ir_block_t *helper_entry = ir_block_add(helper, "entry");
    ir_instr_t *helper_return = ir_instr_add(helper_entry, IR_OP_RETURN);
    assert(parameter && helper_entry && helper_return
        && ir_instr_operand_value(helper_return, parameter));
    th_type *array_type = ir_type_parse(runtime, "array[uint]");
    ir_func_t *array_function = ir_func_add(runtime, "array_literal",
        ir_type_parse(runtime, "() -> array[uint]"), IR_LINK_INTERNAL);
    ir_block_t *array_entry = array_function
        ? ir_block_add(array_function, "entry") : nullptr;
    ir_instr_t *array_item = array_entry
        ? ir_instr_add(array_entry, IR_OP_CONST) : nullptr;
    ir_instr_t *array = array_entry
        ? ir_instr_add(array_entry, IR_OP_AGGREGATE) : nullptr;
    ir_instr_t *array_return = array_entry
        ? ir_instr_add(array_entry, IR_OP_RETURN) : nullptr;
    assert(array_type && th_kind_of(array_type) == TH_SEQUENCE
        && array_item && ir_instr_set_dest(array_item, "item", unsigned_type)
        && ir_instr_operand_uint(array_item, 7)
        && array && ir_instr_set_dest(array, "array", array_type)
        && ir_instr_set_aggregate_form(array, IR_AGGREGATE_ELEMENTS)
        && ir_instr_operand_value(array, array_item->dest)
        && array_return && ir_instr_operand_value(array_return,
                                                  array->dest));
    th_type *array_reference = ir_type_parse(runtime, "ref[array[uint]]");
    ir_func_t *index_function = ir_func_add(runtime, "array_index",
        ir_type_parse(runtime, "(array[uint], uint) -> uint"),
        IR_LINK_INTERNAL);
    ir_value_t *index_array = index_function
        ? ir_param_add(index_function, "array", array_type) : nullptr;
    ir_value_t *index_position = index_function
        ? ir_param_add(index_function, "position", unsigned_type) : nullptr;
    ir_block_t *index_entry = index_function
        ? ir_block_add(index_function, "entry") : nullptr;
    ir_instr_t *index_storage = index_entry
        ? ir_instr_add(index_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *index_store = index_entry
        ? ir_instr_add(index_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *index_address = index_entry
        ? ir_instr_add(index_entry, IR_OP_INDEX) : nullptr;
    ir_instr_t *index_load = index_entry
        ? ir_instr_add(index_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *index_return = index_entry
        ? ir_instr_add(index_entry, IR_OP_RETURN) : nullptr;
    assert(array_reference && index_array && index_position
        && index_storage && index_store && index_address && index_load
        && index_return
        && ir_instr_set_dest(index_storage, "array.storage", array_reference)
        && ir_instr_operand_type(index_storage, array_type)
        && ir_instr_operand_value(index_store, index_storage->dest)
        && ir_instr_operand_value(index_store, index_array)
        && ir_instr_set_dest(index_address, "array.item.address",
            ir_type_parse(runtime, "ref[uint]"))
        && ir_instr_operand_value(index_address, index_storage->dest)
        && ir_instr_operand_value(index_address, index_position)
        && ir_instr_set_dest(index_load, "array.item", unsigned_type)
        && ir_instr_operand_value(index_load, index_address->dest)
        && ir_instr_operand_value(index_return, index_load->dest));
    ir_func_t *cached_function = ir_func_add(runtime, "cached_array",
        ir_type_parse(runtime, "() -> array[uint]"), IR_LINK_INTERNAL);
    ir_block_t *cached_entry = cached_function
        ? ir_block_add(cached_function, "entry") : nullptr;
    ir_instr_t *cached_value = cached_entry
        ? ir_instr_add(cached_entry, IR_OP_PHASE_RESULT) : nullptr;
    ir_instr_t *cached_return = cached_entry
        ? ir_instr_add(cached_entry, IR_OP_RETURN) : nullptr;
    assert(cached_value && ir_instr_set_dest(cached_value, "cached", array_type)
        && ir_instr_set_phase_result(context, cached_value, "sample:array")
        && cached_return && ir_instr_operand_value(cached_return,
                                                    cached_value->dest));
    const uint8_t cached_bytes[16] = {[0] = 5, [8] = 6};
    const n00bb_llvm_phase_array_t phase_array = {
        .identifier = "sample:array", .bytes = cached_bytes,
        .count = 2, .element_stride = 8,
    };
    th_type *string_type = ir_type_parse(runtime, "string");
    ir_func_t *binary_function = ir_func_add(runtime, "binary_literal",
        ir_type_parse(runtime, "() -> string"), IR_LINK_INTERNAL);
    ir_block_t *binary_entry = binary_function
        ? ir_block_add(binary_function, "entry") : nullptr;
    ir_instr_t *binary_value = binary_entry
        ? ir_instr_add(binary_entry, IR_OP_CONST) : nullptr;
    ir_instr_t *binary_return = binary_entry
        ? ir_instr_add(binary_entry, IR_OP_RETURN) : nullptr;
    assert(string_type && binary_value
        && ir_instr_set_dest(binary_value, "binary", string_type)
        && ir_instr_set_const_bytes(binary_value,
            (const uint8_t *)"abc", 3)
        && ir_annot_set(context, &binary_value->annots, (ir_annot_t){
            .key = "n00bb.literal.modifier", .kind = IR_ANNOT_STR,
            .s = "binary",
        })
        && binary_return
        && ir_instr_operand_value(binary_return, binary_value->dest));
    ir_func_t *string_function = ir_func_add(runtime, "cached_string",
        ir_type_parse(runtime, "() -> string"), IR_LINK_INTERNAL);
    ir_block_t *string_entry = string_function
        ? ir_block_add(string_function, "entry") : nullptr;
    ir_instr_t *string_value = string_entry
        ? ir_instr_add(string_entry, IR_OP_PHASE_RESULT) : nullptr;
    ir_instr_t *string_return = string_entry
        ? ir_instr_add(string_entry, IR_OP_RETURN) : nullptr;
    assert(string_type && string_value
        && ir_instr_set_dest(string_value, "cached.string", string_type)
        && ir_instr_set_phase_result(context, string_value, "sample:string")
        && string_return
        && ir_instr_operand_value(string_return, string_value->dest));
    th_type *reference_type = ir_type_parse(runtime, "ref[uint]");
    ir_func_t *ref_address_builtin = ir_func_add(runtime, "ref_address_builtin",
        ir_type_parse(runtime, "(ref[uint]) -> uint"), IR_LINK_EXTERN);
    ir_func_t *ref_address_function = ir_func_add(runtime, "ref_address",
        ir_type_parse(runtime, "(ref[uint]) -> uint"), IR_LINK_INTERNAL);
    ir_value_t *ref_address_parameter = ref_address_function
        ? ir_param_add(ref_address_function, "value", reference_type) : nullptr;
    ir_block_t *ref_address_entry = ref_address_function
        ? ir_block_add(ref_address_function, "entry") : nullptr;
    ir_instr_t *ref_address_call = ref_address_entry
        ? ir_instr_add(ref_address_entry, IR_OP_CALL) : nullptr;
    ir_instr_t *ref_address_return = ref_address_entry
        ? ir_instr_add(ref_address_entry, IR_OP_RETURN) : nullptr;
    assert(ref_address_builtin && ref_address_parameter && ref_address_call
        && ir_annot_set(context, &ref_address_builtin->annots, (ir_annot_t){
            .key = "n00bb.abi.native", .kind = IR_ANNOT_STR,
            .s = "n00b_builtin_abi_ref_address",
        })
        && ir_instr_set_dest(ref_address_call, "address", unsigned_type)
        && ir_instr_operand_value(ref_address_call,
            ir_func_address(runtime, ref_address_builtin))
        && ir_instr_operand_value(ref_address_call, ref_address_parameter)
        && ir_instr_set_call_signature(ref_address_call,
            ref_address_builtin->type)
        && ref_address_return
        && ir_instr_operand_value(ref_address_return,
            ref_address_call->dest));
    ir_func_t *barrier_function = ir_func_add(runtime, "barrier_store",
        ir_type_parse(runtime, "(ref[uint]) -> nil"), IR_LINK_INTERNAL);
    ir_value_t *barrier_parameter = barrier_function
        ? ir_param_add(barrier_function, "source", reference_type) : nullptr;
    ir_local_t *barrier_local = barrier_function
        ? ir_local_add(barrier_function, "target", reference_type) : nullptr;
    ir_block_t *barrier_entry = barrier_function
        ? ir_block_add(barrier_function, "entry") : nullptr;
    ir_instr_t *barrier_copy = barrier_entry
        ? ir_instr_add(barrier_entry, IR_OP_COPY) : nullptr;
    assert(barrier_parameter && barrier_local && barrier_copy
        && ir_instr_set_dest_local(barrier_copy, barrier_local)
        && ir_instr_operand_value(barrier_copy, barrier_parameter)
        && ir_instr_add(barrier_entry, IR_OP_RETURN));
    th_type *reference_storage = ir_type_parse(runtime, "ref[ref[uint]]");
    ir_func_t *heap_store_function = ir_func_add(runtime, "barrier_heap_store",
        ir_type_parse(runtime, "(ref[ref[uint]], ref[uint]) -> nil"),
        IR_LINK_INTERNAL);
    ir_value_t *heap_target = heap_store_function
        ? ir_param_add(heap_store_function, "target", reference_storage)
        : nullptr;
    ir_value_t *heap_source = heap_store_function
        ? ir_param_add(heap_store_function, "source", reference_type)
        : nullptr;
    ir_block_t *heap_store_entry = heap_store_function
        ? ir_block_add(heap_store_function, "entry") : nullptr;
    ir_instr_t *heap_store = heap_store_entry
        ? ir_instr_add(heap_store_entry, IR_OP_STORE) : nullptr;
    assert(reference_storage && heap_target && heap_source && heap_store
        && ir_instr_operand_value(heap_store, heap_target)
        && ir_instr_operand_value(heap_store, heap_source)
        && ir_instr_add(heap_store_entry, IR_OP_RETURN));
    ir_func_t *stack_store_function = ir_func_add(runtime,
        "stack_promoted_store", ir_type_parse(runtime,
            "(ref[uint]) -> nil"), IR_LINK_INTERNAL);
    ir_value_t *stack_source = stack_store_function
        ? ir_param_add(stack_store_function, "source", reference_type)
        : nullptr;
    ir_block_t *stack_store_entry = stack_store_function
        ? ir_block_add(stack_store_function, "entry") : nullptr;
    ir_instr_t *stack_allocation = stack_store_entry
        ? ir_instr_add(stack_store_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *stack_store = stack_store_entry
        ? ir_instr_add(stack_store_entry, IR_OP_STORE) : nullptr;
    assert(stack_source && stack_allocation && stack_store
        && ir_instr_set_dest(stack_allocation, "stack.promoted",
            reference_storage)
        && ir_instr_operand_type(stack_allocation, reference_type)
        && ir_instr_operand_value(stack_store, stack_allocation->dest)
        && ir_instr_operand_value(stack_store, stack_source)
        && ir_instr_add(stack_store_entry, IR_OP_RETURN));
    ir_func_t *allocation_function = ir_func_add(runtime, "heap_ref",
        ir_type_parse(runtime, "() -> ref[uint]"), IR_LINK_INTERNAL);
    ir_block_t *allocation_entry = allocation_function
        ? ir_block_add(allocation_function, "entry") : nullptr;
    ir_instr_t *allocation = allocation_entry
        ? ir_instr_add(allocation_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *allocation_return = allocation_entry
        ? ir_instr_add(allocation_entry, IR_OP_RETURN) : nullptr;
    assert(reference_type && allocation && allocation_return
        && ir_instr_set_dest(allocation, "heap.storage", reference_type)
        && ir_instr_operand_type(allocation, unsigned_type)
        && ir_instr_operand_value(allocation_return, allocation->dest));
    ir_func_t *local_callee = ir_func_add(runtime, "write_local",
        ir_type_parse(runtime, "(ref[uint]) -> nil"), IR_LINK_INTERNAL);
    ir_value_t *local_parameter = local_callee
        ? ir_param_add(local_callee, "address", reference_type) : nullptr;
    ir_block_t *local_callee_entry = local_callee
        ? ir_block_add(local_callee, "entry") : nullptr;
    ir_instr_t *local_alias = local_callee_entry
        ? ir_instr_add(local_callee_entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *local_constant = local_callee_entry
        ? ir_instr_add(local_callee_entry, IR_OP_CONST) : nullptr;
    ir_instr_t *local_store = local_callee_entry
        ? ir_instr_add(local_callee_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *local_callee_return = local_callee_entry
        ? ir_instr_add(local_callee_entry, IR_OP_RETURN) : nullptr;
    assert(local_parameter && local_alias && local_constant && local_store
        && local_callee_return
        && ir_instr_set_dest(local_alias, "derived.address", reference_type)
        && ir_instr_operand_value(local_alias, local_parameter)
        && ir_instr_set_dest(local_constant, "stored.value", unsigned_type)
        && ir_instr_operand_uint(local_constant, 17)
        && ir_instr_operand_value(local_store, local_alias->dest)
        && ir_instr_operand_value(local_store, local_constant->dest));
    ir_func_t *local_caller = ir_func_add(runtime, "stack_call",
        ir_type_parse(runtime, "() -> nil"), IR_LINK_INTERNAL);
    ir_block_t *local_caller_entry = local_caller
        ? ir_block_add(local_caller, "entry") : nullptr;
    ir_instr_t *local_storage = local_caller_entry
        ? ir_instr_add(local_caller_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *local_call = local_caller_entry
        ? ir_instr_add(local_caller_entry, IR_OP_CALL) : nullptr;
    ir_instr_t *local_caller_return = local_caller_entry
        ? ir_instr_add(local_caller_entry, IR_OP_RETURN) : nullptr;
    assert(local_storage && local_call && local_caller_return
        && ir_instr_set_dest(local_storage, "stack.storage", reference_type)
        && ir_instr_operand_type(local_storage, unsigned_type)
        && ir_instr_operand_value(local_call,
            ir_func_address(runtime, local_callee))
        && ir_instr_operand_value(local_call, local_storage->dest)
        && ir_instr_set_call_signature(local_call, local_callee->type));
    ir_global_t *escaped_address = ir_global_add(runtime, "escaped_address",
                                                 reference_type);
    ir_func_t *escaping_callee = ir_func_add(runtime, "capture_address",
        ir_type_parse(runtime, "(ref[uint]) -> nil"), IR_LINK_INTERNAL);
    ir_value_t *escaping_parameter = escaping_callee
        ? ir_param_add(escaping_callee, "address", reference_type) : nullptr;
    ir_block_t *escaping_callee_entry = escaping_callee
        ? ir_block_add(escaping_callee, "entry") : nullptr;
    ir_instr_t *escaped_root_address = escaping_callee_entry
        ? ir_instr_add(escaping_callee_entry, IR_OP_ADDRESS_OF) : nullptr;
    ir_instr_t *capture = escaping_callee_entry
        ? ir_instr_add(escaping_callee_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *capture_direct = escaping_callee_entry
        ? ir_instr_add(escaping_callee_entry, IR_OP_GLOBAL_STORE) : nullptr;
    ir_instr_t *escaping_callee_return = escaping_callee_entry
        ? ir_instr_add(escaping_callee_entry, IR_OP_RETURN) : nullptr;
    assert(escaped_address && escaping_parameter && escaped_root_address
        && capture && capture_direct
        && escaping_callee_return
        && ir_instr_set_dest(escaped_root_address, "escaped.root.address",
            reference_storage)
        && ir_instr_operand_value(escaped_root_address,
            ir_global_address(runtime, escaped_address))
        && ir_instr_operand_value(capture, escaped_root_address->dest)
        && ir_instr_operand_value(capture, escaping_parameter)
        && ir_instr_operand_value(capture_direct,
            ir_global_address(runtime, escaped_address))
        && ir_instr_operand_value(capture_direct, escaping_parameter));
    ir_global_t *thread_root = ir_global_add(runtime, "thread_root",
                                            reference_type);
    ir_func_t *thread_capture = ir_func_add(runtime, "capture_thread_root",
        ir_type_parse(runtime, "(ref[uint]) -> nil"), IR_LINK_INTERNAL);
    ir_value_t *thread_parameter = thread_capture
        ? ir_param_add(thread_capture, "address", reference_type) : nullptr;
    ir_block_t *thread_entry = thread_capture
        ? ir_block_add(thread_capture, "entry") : nullptr;
    ir_instr_t *thread_root_address = thread_entry
        ? ir_instr_add(thread_entry, IR_OP_ADDRESS_OF) : nullptr;
    ir_instr_t *thread_store = thread_entry
        ? ir_instr_add(thread_entry, IR_OP_STORE) : nullptr;
    assert(thread_root && ir_global_set_storage_duration(context,
        thread_root, IR_STORAGE_DURATION_THREAD) && thread_parameter
        && thread_root_address && thread_store
        && ir_instr_set_dest(thread_root_address, "thread.root.address",
            reference_storage)
        && ir_instr_operand_value(thread_root_address,
            ir_global_address(runtime, thread_root))
        && ir_instr_operand_value(thread_store, thread_root_address->dest)
        && ir_instr_operand_value(thread_store, thread_parameter)
        && ir_instr_add(thread_entry, IR_OP_RETURN));
    ir_func_t *escaping_caller = ir_func_add(runtime, "escaping_call",
        ir_type_parse(runtime, "() -> nil"), IR_LINK_INTERNAL);
    ir_block_t *escaping_caller_entry = escaping_caller
        ? ir_block_add(escaping_caller, "entry") : nullptr;
    ir_instr_t *escaping_storage = escaping_caller_entry
        ? ir_instr_add(escaping_caller_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *escaping_call = escaping_caller_entry
        ? ir_instr_add(escaping_caller_entry, IR_OP_CALL) : nullptr;
    ir_instr_t *escaping_caller_return = escaping_caller_entry
        ? ir_instr_add(escaping_caller_entry, IR_OP_RETURN) : nullptr;
    assert(escaping_storage && escaping_call && escaping_caller_return
        && ir_instr_set_dest(escaping_storage, "captured.storage",
            reference_type)
        && ir_instr_operand_type(escaping_storage, unsigned_type)
        && ir_instr_operand_value(escaping_call,
            ir_func_address(runtime, escaping_callee))
        && ir_instr_operand_value(escaping_call, escaping_storage->dest)
        && ir_instr_set_call_signature(escaping_call, escaping_callee->type));
    th_type *record_type = ir_type_parse(runtime, "Point");
    th_type *record_definition = ir_type_parse(runtime,
        "(x: uint, y: uint)");
    const ir_layout_bit_run_t first_run[] = {{.offset = 0, .width = 64}};
    const ir_layout_bit_run_t second_run[] = {{.offset = 64, .width = 64}};
    const ir_layout_bit_run_t record_occupied[] = {
        {.offset = 0, .width = 128},
    };
    const ir_final_member_layout_t record_members[] = {
        {.name = "x", .type = unsigned_type, .ordinal = 0,
         .bit_offset = 0, .occupied = first_run, .n_occupied = 1},
        {.name = "y", .type = unsigned_type, .ordinal = 1,
         .bit_offset = 64, .occupied = second_run, .n_occupied = 1},
    };
    const ir_final_layout_t record_layout = {
        .intrinsic_bits = 128, .alignment_bytes = 8,
        .occupied = record_occupied, .n_occupied = 1,
        .members = record_members, .n_members = 2,
    };
    assert(record_type && record_definition);
    assert(ir_type_define(runtime, record_type, record_definition));
    assert(ir_layout_publish_final(runtime, record_type, target,
                                   &record_layout));
    ir_func_t *field_function = ir_func_add(runtime, "record_field",
        ir_type_parse(runtime, "() -> uint"), IR_LINK_INTERNAL);
    ir_block_t *field_entry = field_function
        ? ir_block_add(field_function, "entry") : nullptr;
    ir_instr_t *record_storage = field_entry
        ? ir_instr_add(field_entry, IR_OP_ALLOC) : nullptr;
    ir_instr_t *field_address = field_entry
        ? ir_instr_add(field_entry, IR_OP_FIELD) : nullptr;
    ir_instr_t *field_constant = field_entry
        ? ir_instr_add(field_entry, IR_OP_CONST) : nullptr;
    ir_instr_t *field_store = field_entry
        ? ir_instr_add(field_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *field_load = field_entry
        ? ir_instr_add(field_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *record_value = field_entry
        ? ir_instr_add(field_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *field_extract = field_entry
        ? ir_instr_add(field_entry, IR_OP_EXTRACT) : nullptr;
    ir_instr_t *field_return = field_entry
        ? ir_instr_add(field_entry, IR_OP_RETURN) : nullptr;
    assert(record_storage && field_address && field_constant && field_store
        && field_load && record_value && field_extract && field_return
        && ir_instr_set_dest(record_storage, "point.storage",
            ir_type_parse(runtime, "ref[Point]"))
        && ir_instr_operand_type(record_storage, record_type)
        && ir_instr_set_dest(field_address, "point.y.address",
            ir_type_parse(runtime, "ref[uint]"))
        && ir_instr_operand_value(field_address, record_storage->dest)
        && ir_instr_operand_name(field_address, "y")
        && ir_instr_set_dest(field_constant, "point.y.value", unsigned_type)
        && ir_instr_operand_uint(field_constant, 42)
        && ir_instr_operand_value(field_store, field_address->dest)
        && ir_instr_operand_value(field_store, field_constant->dest)
        && ir_instr_set_dest(field_load, "point.y.loaded", unsigned_type)
        && ir_instr_operand_value(field_load, field_address->dest)
        && ir_instr_set_dest(record_value, "point.value", record_type)
        && ir_instr_operand_value(record_value, record_storage->dest)
        && ir_instr_set_dest(field_extract, "point.y.extracted", unsigned_type)
        && ir_instr_operand_value(field_extract, record_value->dest)
        && ir_instr_operand_name(field_extract, "y")
        && ir_instr_operand_value(field_return, field_extract->dest));
    th_type *indexable_uint = ir_type_parse(runtime, "ptr[uint]");
    ir_func_t *offset_function = ir_func_add(runtime, "offset_load",
        ir_type_parse(runtime, "(ptr[uint], uint) -> uint"),
        IR_LINK_INTERNAL);
    ir_value_t *base = offset_function
        ? ir_param_add(offset_function, "base", indexable_uint) : nullptr;
    ir_value_t *index = offset_function
        ? ir_param_add(offset_function, "index", unsigned_type) : nullptr;
    ir_block_t *offset_entry = offset_function
        ? ir_block_add(offset_function, "entry") : nullptr;
    ir_instr_t *offset_value = offset_entry
        ? ir_instr_add(offset_entry, IR_OP_OFFSET) : nullptr;
    ir_instr_t *cast_value = offset_entry
        ? ir_instr_add(offset_entry, IR_OP_BITCAST) : nullptr;
    ir_instr_t *loaded_value = offset_entry
        ? ir_instr_add(offset_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *offset_return = offset_entry
        ? ir_instr_add(offset_entry, IR_OP_RETURN) : nullptr;
    assert(base && index && offset_value && cast_value && loaded_value
        && offset_return
        && ir_instr_set_dest(offset_value, "element", indexable_uint)
        && ir_instr_operand_value(offset_value, base)
        && ir_instr_operand_value(offset_value, index)
        && ir_instr_set_dest(cast_value, "reference", reference_type)
        && ir_instr_operand_value(cast_value, offset_value->dest)
        && ir_instr_set_dest(loaded_value, "element.value", unsigned_type)
        && ir_instr_operand_value(loaded_value, cast_value->dest)
        && ir_instr_operand_value(offset_return, loaded_value->dest));
    th_type *atomic_uint = ir_type_parse(runtime, "atomic[uint]");
    th_type *atomic_ref = ir_type_parse(runtime, "ref[atomic[uint]]");
    ir_func_t *atomic_function = ir_func_add(runtime, "atomic_round_trip",
        ir_type_parse(runtime, "(ref[atomic[uint]]) -> uint"),
        IR_LINK_INTERNAL);
    ir_value_t *atomic_address = atomic_function
        ? ir_param_add(atomic_function, "address", atomic_ref) : nullptr;
    ir_block_t *atomic_entry = atomic_function
        ? ir_block_add(atomic_function, "entry") : nullptr;
    ir_instr_t *atomic_read = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *atomic_write = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *atomic_read_relaxed = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *atomic_read_acquire = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_LOAD) : nullptr;
    ir_instr_t *atomic_write_relaxed = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *atomic_write_release = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_STORE) : nullptr;
    ir_instr_t *atomic_or = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_ATOMIC_RMW) : nullptr;
    ir_instr_t *atomic_or_release = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_ATOMIC_RMW) : nullptr;
    ir_instr_t *atomic_or_relaxed = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_ATOMIC_RMW) : nullptr;
    ir_instr_t *atomic_return = atomic_entry
        ? ir_instr_add(atomic_entry, IR_OP_RETURN) : nullptr;
    assert(atomic_uint && atomic_ref && atomic_address && atomic_read
        && atomic_write && atomic_read_relaxed && atomic_read_acquire
        && atomic_write_relaxed && atomic_write_release
        && atomic_or && atomic_or_release
        && atomic_or_relaxed && atomic_return
        && ir_layout_publish_final(runtime, atomic_uint, target,
            &unsigned_layout)
        && ir_instr_set_dest(atomic_read, "atomic.value", unsigned_type)
        && ir_instr_operand_value(atomic_read, atomic_address)
        && ir_instr_add_mod(atomic_read, "atomic")
        && ir_instr_operand_value(atomic_write, atomic_address)
        && ir_instr_operand_value(atomic_write, atomic_read->dest)
        && ir_instr_add_mod(atomic_write, "atomic")
        && ir_instr_set_dest(atomic_read_relaxed, "atomic.relaxed.value",
                             unsigned_type)
        && ir_instr_operand_value(atomic_read_relaxed, atomic_address)
        && ir_instr_add_mod(atomic_read_relaxed, "atomic")
        && ir_instr_add_mod(atomic_read_relaxed, "relaxed")
        && ir_instr_set_dest(atomic_read_acquire, "atomic.acquire.value",
                             unsigned_type)
        && ir_instr_operand_value(atomic_read_acquire, atomic_address)
        && ir_instr_add_mod(atomic_read_acquire, "atomic")
        && ir_instr_add_mod(atomic_read_acquire, "acquire")
        && ir_instr_operand_value(atomic_write_relaxed, atomic_address)
        && ir_instr_operand_value(atomic_write_relaxed,
                                  atomic_read_relaxed->dest)
        && ir_instr_add_mod(atomic_write_relaxed, "atomic")
        && ir_instr_add_mod(atomic_write_relaxed, "relaxed")
        && ir_instr_operand_value(atomic_write_release, atomic_address)
        && ir_instr_operand_value(atomic_write_release,
                                  atomic_read_acquire->dest)
        && ir_instr_add_mod(atomic_write_release, "atomic")
        && ir_instr_add_mod(atomic_write_release, "release")
        && ir_instr_set_dest(atomic_or, "atomic.previous", unsigned_type)
        && ir_instr_operand_value(atomic_or, atomic_address)
        && ir_instr_operand_name(atomic_or, "or")
        && ir_instr_operand_value(atomic_or, atomic_read->dest)
        && ir_instr_set_dest(atomic_or_release, "atomic.release.previous",
                             unsigned_type)
        && ir_instr_operand_value(atomic_or_release, atomic_address)
        && ir_instr_operand_name(atomic_or_release, "or")
        && ir_instr_operand_value(atomic_or_release, atomic_read->dest)
        && ir_instr_add_mod(atomic_or_release, "release")
        && ir_instr_set_dest(atomic_or_relaxed, "atomic.relaxed.previous",
                             unsigned_type)
        && ir_instr_operand_value(atomic_or_relaxed, atomic_address)
        && ir_instr_operand_name(atomic_or_relaxed, "or")
        && ir_instr_operand_value(atomic_or_relaxed, atomic_read->dest)
        && ir_instr_add_mod(atomic_or_relaxed, "relaxed")
        && ir_instr_operand_value(atomic_return, atomic_or_relaxed->dest));
    th_type *boolean_type = ir_type_parse(runtime, "bool");
    ir_func_t *arithmetic_function = ir_func_add(runtime, "arithmetic",
        ir_type_parse(runtime, "(i128) -> uint"), IR_LINK_INTERNAL);
    ir_value_t *wide_parameter = arithmetic_function
        ? ir_param_add(arithmetic_function, "wide", wide) : nullptr;
    ir_block_t *arithmetic_block = arithmetic_function
        ? ir_block_add(arithmetic_function, "entry") : nullptr;
    ir_instr_t *nine = ir_instr_add(arithmetic_block, IR_OP_CONST);
    ir_instr_t *two = ir_instr_add(arithmetic_block, IR_OP_CONST);
    ir_instr_t *difference = ir_instr_add(arithmetic_block, IR_OP_SUB);
    ir_instr_t *product = ir_instr_add(arithmetic_block, IR_OP_MUL);
    ir_instr_t *quotient = ir_instr_add(arithmetic_block, IR_OP_DIV);
    ir_instr_t *remainder = ir_instr_add(arithmetic_block, IR_OP_REM);
    ir_instr_t *unequal = ir_instr_add(arithmetic_block, IR_OP_NE);
    ir_instr_t *greater = ir_instr_add(arithmetic_block, IR_OP_GE);
    ir_instr_t *narrowed = ir_instr_add(arithmetic_block, IR_OP_CAST);
    ir_instr_t *arithmetic_return = ir_instr_add(arithmetic_block,
                                                IR_OP_RETURN);
    assert(boolean_type && wide_parameter && nine && two && difference
        && product && quotient && remainder && unequal && greater && narrowed
        && arithmetic_return
        && ir_instr_set_dest(nine, "nine", unsigned_type)
        && ir_instr_operand_uint(nine, 9)
        && ir_instr_set_dest(two, "two", unsigned_type)
        && ir_instr_operand_uint(two, 2)
        && ir_instr_set_dest(difference, "difference", unsigned_type)
        && ir_instr_operand_value(difference, nine->dest)
        && ir_instr_operand_value(difference, two->dest)
        && ir_instr_add_mod(difference, "trap")
        && ir_instr_set_dest(product, "product", unsigned_type)
        && ir_instr_operand_value(product, difference->dest)
        && ir_instr_operand_value(product, two->dest)
        && ir_instr_add_mod(product, "trap")
        && ir_instr_set_dest(quotient, "quotient", unsigned_type)
        && ir_instr_operand_value(quotient, product->dest)
        && ir_instr_operand_value(quotient, two->dest)
        && ir_instr_add_mod(quotient, "trap")
        && ir_instr_set_dest(remainder, "remainder", unsigned_type)
        && ir_instr_operand_value(remainder, quotient->dest)
        && ir_instr_operand_value(remainder, two->dest)
        && ir_instr_add_mod(remainder, "trap")
        && ir_instr_set_dest(unequal, "unequal", boolean_type)
        && ir_instr_operand_value(unequal, quotient->dest)
        && ir_instr_operand_value(unequal, remainder->dest)
        && ir_instr_set_dest(greater, "greater", boolean_type)
        && ir_instr_operand_value(greater, quotient->dest)
        && ir_instr_operand_value(greater, remainder->dest)
        && ir_instr_set_dest(narrowed, "narrowed", unsigned_type)
        && ir_instr_operand_value(narrowed, wide_parameter)
        && ir_instr_add_mod(narrowed, "trap")
        && ir_instr_operand_value(arithmetic_return, narrowed->dest));
    th_type *float_type = ir_type_parse(runtime, "f64");
    const struct {
        const char *name;
        const char *signature;
        th_type *source;
        th_type *target;
        const char *rounding;
        bool checked;
    } casts[] = {
        {"uint_to_float", "(uint) -> f64", unsigned_type, float_type,
         "nearest", false},
        {"signed_to_float", "(i128) -> f64", wide, float_type,
         "nearest", false},
        {"float_to_uint", "(f64) -> uint", float_type, unsigned_type,
         "to_zero", true},
        {"float_to_signed", "(f64) -> i128", float_type, wide,
         "to_zero", true},
    };
    assert(float_type);
    for (size_t i = 0; i < sizeof(casts) / sizeof(casts[0]); i++) {
        ir_func_t *cast_function = ir_func_add(runtime, casts[i].name,
            ir_type_parse(runtime, casts[i].signature), IR_LINK_INTERNAL);
        ir_value_t *argument = cast_function
            ? ir_param_add(cast_function, "source", casts[i].source) : nullptr;
        ir_block_t *entry = cast_function
            ? ir_block_add(cast_function, "entry") : nullptr;
        ir_instr_t *conversion = entry
            ? ir_instr_add(entry, IR_OP_CAST) : nullptr;
        ir_instr_t *return_value = entry
            ? ir_instr_add(entry, IR_OP_RETURN) : nullptr;
        assert(argument && conversion && return_value
            && ir_instr_set_dest(conversion, "converted", casts[i].target)
            && ir_instr_operand_value(conversion, argument)
            && ir_instr_add_mod(conversion, casts[i].rounding)
            && (!casts[i].checked || ir_instr_add_mod(conversion, "trap"))
            && ir_instr_operand_value(return_value, conversion->dest));
    }
    static const uint8_t cached_string[] = "a longer static string";
    const n00bb_llvm_phase_array_t phase_string = {
        .identifier = "sample:string", .bytes = cached_string,
        .count = sizeof(cached_string) - 1,
        .kind = N00BB_LLVM_LITERAL_STRING,
        .string_encoding = N00B_ABI_STRING_BINARY,
    };
    const n00bb_llvm_phase_array_t phase_values[] = {
        phase_array, phase_string,
    };
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
        comptime, runtime, true, false, diagnostics);
    assert(backend);
    n00bb_llvm_type_index_t type_indices[] = {
        {.typehash = th_hash(ir_module_types(runtime), unsigned_type),
         .index = 7, .finalizable = true},
        {.typehash = th_hash(ir_module_types(runtime), reference_type),
         .index = 8},
        {.typehash = th_hash(ir_module_types(runtime), array_type),
         .index = 9},
    };
    assert(n00bb_llvm_backend_bind_type_indices(backend, type_indices, 3));
    assert(n00bb_llvm_backend_bind_phase_arrays(backend, phase_values, 2));
    assert(core_diagnostics_count(diagnostics) == 0);

    for (int phase = 0; phase < 2; phase++) {
        core_buffer_t bytes = {0};
        bool is_comptime = phase == 0;
        bool wrote = n00bb_llvm_backend_write_bitcode(
            backend, is_comptime, &bytes, diagnostics);
        if (!wrote)
            for (size_t i = 0; i < core_diagnostics_count(diagnostics); i++) {
                const core_diagnostic_t *diagnostic =
                    core_diagnostics_at(diagnostics, i);
                fprintf(stderr, "%s\n", diagnostic->message.data);
            }
        assert(wrote);
        assert(bytes.byte_len > 0);
        LLVMMemoryBufferRef source = LLVMCreateMemoryBufferWithMemoryRangeCopy(
            bytes.data, bytes.byte_len, "probe");
        assert(source);
        LLVMContextRef reader = LLVMContextCreate();
        assert(reader);
        LLVMModuleRef parsed = nullptr;
        assert(!LLVMParseBitcodeInContext2(reader, source, &parsed));
        assert(parsed);
        assert(strcmp(LLVMGetTarget(parsed), target) == 0);
        LLVMValueRef lowered_thread = LLVMGetNamedGlobal(parsed,
            "thread_counter");
        assert((!is_comptime && lowered_thread
                && LLVMIsThreadLocal(lowered_thread))
            || (is_comptime && !lowered_thread));
        if (!is_comptime) {
            for (size_t i = 0; i < 2; i++) {
                const char *flag_names[] = {
                    "__n00bb_initialized.static_global",
                    "__n00bb_initialized.dynamic_global",
                };
                const char *function_names[] = {
                    "probe.read_static", "probe.read_dynamic",
                };
                LLVMValueRef flag = LLVMGetNamedGlobal(parsed, flag_names[i]);
                LLVMValueRef reader = LLVMGetNamedFunction(parsed,
                                                           function_names[i]);
                assert(flag && reader
                    && LLVMConstIntGetZExtValue(LLVMGetInitializer(flag))
                        == (i == 0));
                bool guarded = false;
                for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(reader);
                     bb; bb = LLVMGetNextBasicBlock(bb))
                    for (LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
                         instruction;
                         instruction = LLVMGetNextInstruction(instruction))
                        if (LLVMGetInstructionOpcode(instruction) == LLVMLoad
                            && LLVMGetOperand(instruction, 0) == flag)
                            guarded = true;
                assert(guarded == (i == 1));
            }
        }
        LLVMValueRef lowered = LLVMGetNamedFunction(parsed, "probe.entry");
        assert((!is_comptime && lowered) || (is_comptime && !lowered));
        LLVMValueRef lowered_helper = LLVMGetNamedFunction(parsed,
            "probe.helper");
        assert((!is_comptime && lowered_helper)
            || (is_comptime && !lowered_helper));
        if (lowered_helper)
            assert(LLVMCountParams(lowered_helper) == 1
                && LLVMCountBasicBlocks(lowered_helper) == 1);
        LLVMValueRef lowered_ref_address = LLVMGetNamedFunction(parsed,
            "probe.ref_address");
        assert((!is_comptime && lowered_ref_address)
            || (is_comptime && !lowered_ref_address));
        if (lowered_ref_address) {
            bool converted = false;
            for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(lowered_ref_address);
                 bb; bb = LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
                     instruction; instruction = LLVMGetNextInstruction(instruction))
                    if (LLVMGetInstructionOpcode(instruction) == LLVMPtrToInt)
                        converted = true;
            assert(converted);
            assert(!LLVMGetNamedFunction(parsed,
                "n00b_builtin_abi_ref_address"));
        }
        LLVMValueRef lowered_barrier = LLVMGetNamedFunction(parsed,
            "probe.barrier_store");
        assert((!is_comptime && lowered_barrier)
            || (is_comptime && !lowered_barrier));
        if (lowered_barrier) {
            LLVMValueRef barrier = LLVMGetNamedFunction(parsed,
                "sys.internal.gc.alloc.barrier_pointer_runtime");
            bool found_barrier = false;
            bool found_store = false;
            for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(lowered_barrier);
                 bb; bb = LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
                     instruction; instruction = LLVMGetNextInstruction(instruction)) {
                    if (LLVMGetInstructionOpcode(instruction) == LLVMCall
                        && LLVMGetCalledValue(instruction) == barrier)
                        found_barrier = true;
                    if (LLVMGetInstructionOpcode(instruction) == LLVMStore)
                        found_store = true;
                }
            assert(!found_barrier && found_store);
        }
        LLVMValueRef lowered_heap_store = LLVMGetNamedFunction(parsed,
            "probe.barrier_heap_store");
        assert((!is_comptime && lowered_heap_store)
            || (is_comptime && !lowered_heap_store));
        if (lowered_heap_store) {
            LLVMValueRef barrier = LLVMGetNamedFunction(parsed,
                "sys.internal.gc.alloc.barrier_pointer_runtime");
            bool found_barrier = false;
            bool stored_after_barrier = false;
            for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(lowered_heap_store);
                 bb; bb = LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
                     instruction; instruction = LLVMGetNextInstruction(instruction)) {
                    if (LLVMGetInstructionOpcode(instruction) == LLVMCall
                        && LLVMGetCalledValue(instruction) == barrier)
                        found_barrier = true;
                    else if (found_barrier
                        && LLVMGetInstructionOpcode(instruction) == LLVMStore)
                        stored_after_barrier = true;
                }
            assert(barrier && found_barrier && stored_after_barrier);
        }
        const char *untracked_store_names[] = {
            "probe.capture_address", "probe.capture_thread_root",
            "probe.stack_promoted_store",
        };
        for (size_t i = 0; i < sizeof(untracked_store_names) / sizeof(untracked_store_names[0]); i++) {
            LLVMValueRef root_store = LLVMGetNamedFunction(parsed,
                untracked_store_names[i]);
            assert((!is_comptime && root_store)
                || (is_comptime && !root_store));
            if (!root_store) continue;
            LLVMValueRef barrier = LLVMGetNamedFunction(parsed,
                "sys.internal.gc.alloc.barrier_pointer_runtime");
            for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(root_store);
                 bb; bb = LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(bb);
                     instruction; instruction = LLVMGetNextInstruction(instruction))
                    assert(LLVMGetInstructionOpcode(instruction) != LLVMCall
                        || LLVMGetCalledValue(instruction) != barrier);
        }
        LLVMValueRef lowered_array = LLVMGetNamedFunction(parsed,
            "probe.array_literal");
        assert((!is_comptime && lowered_array)
            || (is_comptime && !lowered_array));
        LLVMValueRef lowered_index = LLVMGetNamedFunction(parsed,
            "probe.array_index");
        assert((!is_comptime && lowered_index)
            || (is_comptime && !lowered_index));
        if (lowered_array)
            assert(LLVMGetNamedGlobal(parsed,
                "n00bb.literal.array.descriptor"));
        LLVMValueRef cached_global = LLVMGetNamedGlobal(parsed,
            "n00bb.literal.array.cached");
        assert((!is_comptime && cached_global)
            || (is_comptime && !cached_global));
        if (cached_global) {
            size_t length = 0;
            const char *bytes = LLVMGetAsString(
                LLVMGetInitializer(cached_global), &length);
            assert(bytes && length == sizeof(cached_bytes)
                && (uint8_t)bytes[0] == 5 && (uint8_t)bytes[8] == 6);
        }
        LLVMValueRef string_global = LLVMGetNamedGlobal(parsed,
            "n00bb.literal.string.data");
        assert((!is_comptime && string_global)
            || (is_comptime && !string_global));
        if (string_global) {
            size_t length = 0;
            const char *data = LLVMGetAsString(
                LLVMGetInitializer(string_global), &length);
            assert(data && length == sizeof(cached_string) - 1
                && !memcmp(data, cached_string, length));
        }
        LLVMValueRef cached_string_function = LLVMGetNamedFunction(parsed,
            "probe.cached_string");
        assert((!is_comptime && cached_string_function)
            || (is_comptime && !cached_string_function));
        if (cached_string_function) {
            LLVMValueRef returned = LLVMGetBasicBlockTerminator(
                LLVMGetFirstBasicBlock(cached_string_function));
            LLVMValueRef value = returned ? LLVMGetOperand(returned, 0)
                                          : nullptr;
            uint64_t expected = sizeof(cached_string) - 1
                | ((uint64_t)N00B_ABI_STRING_BINARY
                    << N00B_ABI_STRING_ENCODING_SHIFT)
                | ((uint64_t)N00B_ABI_STRING_LONG
                    << N00B_ABI_STRING_TAG_SHIFT);
            assert(value && LLVMGetNumOperands(value) == 2
                && LLVMConstIntGetZExtValue(LLVMGetOperand(value, 1))
                    == expected);
        }
        LLVMValueRef binary = LLVMGetNamedFunction(parsed,
            "probe.binary_literal");
        assert((!is_comptime && binary) || (is_comptime && !binary));
        if (binary) {
            LLVMValueRef returned = LLVMGetBasicBlockTerminator(
                LLVMGetFirstBasicBlock(binary));
            LLVMValueRef value = returned ? LLVMGetOperand(returned, 0)
                                          : nullptr;
            n00b_abi_string_v1 expected = {0};
            memcpy(expected.short_value.bytes, "abc", 3);
            expected.short_value.selector = 3
                | (N00B_ABI_STRING_BINARY << 4)
                | (N00B_ABI_STRING_SHORT << 6);
            assert(value && LLVMGetNumOperands(value) == 2
                && LLVMConstIntGetZExtValue(LLVMGetOperand(value, 0))
                    == expected.words[0]
                && LLVMConstIntGetZExtValue(LLVMGetOperand(value, 1))
                    == expected.words[1]);
        }
        if (lowered) {
            assert(LLVMCountBasicBlocks(lowered) == 3);
            LLVMValueRef terminator = LLVMGetBasicBlockTerminator(
                LLVMGetLastBasicBlock(lowered));
            assert(terminator && LLVMGetInstructionOpcode(terminator)
                == LLVMRet);
            assert(LLVMGetNamedFunction(parsed,
                "llvm.uadd.with.overflow.i64"));
        }
        LLVMValueRef lowered_allocation = LLVMGetNamedFunction(parsed,
            "probe.heap_ref");
        assert((!is_comptime && lowered_allocation)
            || (is_comptime && !lowered_allocation));
        if (lowered_allocation) {
            LLVMValueRef wrapper = lowered_allocation;
            LLVMValueRef private_entry = LLVMGetNamedFunction(parsed,
                "probe.heap_ref$gc_mutator");
            assert(private_entry && LLVMCountParams(private_entry)
                == LLVMCountParams(lowered_allocation) + 1);
            lowered_allocation = private_entry;
            LLVMValueRef allocator = LLVMGetNamedFunction(parsed,
                "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator");
            LLVMValueRef getter = LLVMGetNamedFunction(parsed,
                "sys.internal.gc.alloc.mutator_for_native");
            bool found = false;
            bool found_getter = false;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(wrapper);
                 block; block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef value = LLVMGetFirstInstruction(block); value;
                 value = LLVMGetNextInstruction(value))
                if (LLVMGetInstructionOpcode(value) == LLVMCall
                    && LLVMGetCalledValue(value) == getter)
                    found_getter = true;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(
                     lowered_allocation); block;
                 block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef value = LLVMGetFirstInstruction(block); value;
                 value = LLVMGetNextInstruction(value)) {
                if (LLVMGetInstructionOpcode(value) == LLVMCall)
                    assert(LLVMGetCalledValue(value) != getter);
                if (LLVMGetInstructionOpcode(value) != LLVMCall
                    || LLVMGetCalledValue(value) != allocator) continue;
                assert(LLVMGetTypeKind(LLVMTypeOf(LLVMGetArgOperand(value, 0)))
                    == LLVMPointerTypeKind);
                assert(LLVMConstIntGetZExtValue(LLVMGetArgOperand(value, 1))
                    == type_indices[0].index);
                assert(LLVMConstIntGetZExtValue(LLVMGetArgOperand(value, 2))
                    == 8);
                assert(LLVMConstIntGetZExtValue(LLVMGetArgOperand(value, 3))
                    == 8);
                assert(LLVMConstIntGetZExtValue(LLVMGetArgOperand(value, 4))
                    == 1);
                assert(LLVMIsNull(LLVMGetArgOperand(value, 5)));
                assert(LLVMGetArgOperand(value, 6)
                    == LLVMGetArgOperand(value, 0));
                found = true;
            }
            assert(found && found_getter);
        }
        LLVMValueRef lowered_stack_call = LLVMGetNamedFunction(parsed,
            "probe.stack_call");
        assert((!is_comptime && lowered_stack_call)
            || (is_comptime && !lowered_stack_call));
        if (lowered_stack_call) {
            bool found_stack_storage = false;
            for (LLVMValueRef value = LLVMGetFirstInstruction(
                     LLVMGetFirstBasicBlock(lowered_stack_call)); value;
                 value = LLVMGetNextInstruction(value)) {
                if (LLVMGetInstructionOpcode(value) == LLVMAlloca)
                    found_stack_storage = true;
                if (LLVMGetInstructionOpcode(value) == LLVMCall)
                    assert(LLVMGetCalledValue(value) != LLVMGetNamedFunction(
                        parsed,
                        "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator"));
            }
            assert(found_stack_storage);
        }
        LLVMValueRef lowered_escaping_call = LLVMGetNamedFunction(parsed,
            "probe.escaping_call");
        assert((!is_comptime && lowered_escaping_call)
            || (is_comptime && !lowered_escaping_call));
        if (lowered_escaping_call) {
            LLVMValueRef private_entry = LLVMGetNamedFunction(parsed,
                "probe.escaping_call$gc_mutator");
            assert(private_entry);
            lowered_escaping_call = private_entry;
            bool found_dynamic_storage = false;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(
                     lowered_escaping_call); block;
                 block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef value = LLVMGetFirstInstruction(block); value;
                 value = LLVMGetNextInstruction(value))
                if (LLVMGetInstructionOpcode(value) == LLVMCall
                    && LLVMGetCalledValue(value) == LLVMGetNamedFunction(
                        parsed,
                        "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator"))
                    found_dynamic_storage = true;
            assert(found_dynamic_storage);
        }
        LLVMValueRef lowered_field = LLVMGetNamedFunction(parsed,
            "probe.record_field");
        assert((!is_comptime && lowered_field)
            || (is_comptime && !lowered_field));
        if (lowered_field) {
            bool found_address = false;
            bool found_extract = false;
            bool found_stack_storage = false;
            for (LLVMValueRef value = LLVMGetFirstInstruction(
                     LLVMGetFirstBasicBlock(lowered_field)); value;
                 value = LLVMGetNextInstruction(value)) {
                if (LLVMGetInstructionOpcode(value) == LLVMAlloca)
                    found_stack_storage = true;
                if (LLVMGetInstructionOpcode(value) == LLVMCall)
                    assert(LLVMGetCalledValue(value) != LLVMGetNamedFunction(
                        parsed,
                        "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator"));
                if (LLVMGetInstructionOpcode(value) == LLVMGetElementPtr)
                    found_address = true;
                if (LLVMGetInstructionOpcode(value) == LLVMExtractValue)
                    found_extract = true;
            }
            assert(found_stack_storage && found_address && found_extract);
        }
        LLVMValueRef lowered_offset = LLVMGetNamedFunction(parsed,
            "probe.offset_load");
        assert((!is_comptime && lowered_offset)
            || (is_comptime && !lowered_offset));
        if (lowered_offset) {
            bool found_offset = false;
            bool found_load = false;
            for (LLVMValueRef value = LLVMGetFirstInstruction(
                     LLVMGetFirstBasicBlock(lowered_offset)); value;
                 value = LLVMGetNextInstruction(value)) {
                if (LLVMGetInstructionOpcode(value) == LLVMGetElementPtr)
                    found_offset = true;
                if (LLVMGetInstructionOpcode(value) == LLVMLoad)
                    found_load = true;
            }
            assert(found_offset && found_load);
        }
        LLVMValueRef lowered_atomic = LLVMGetNamedFunction(parsed,
            "probe.atomic_round_trip");
        assert((!is_comptime && lowered_atomic)
            || (is_comptime && !lowered_atomic));
        if (lowered_atomic) {
            bool found_atomic_load = false;
            bool found_atomic_store = false;
            bool found_atomic_load_relaxed = false;
            bool found_atomic_load_acquire = false;
            bool found_atomic_store_relaxed = false;
            bool found_atomic_store_release = false;
            bool found_atomic_or = false;
            bool found_atomic_or_release = false;
            bool found_atomic_or_relaxed = false;
            for (LLVMValueRef value = LLVMGetFirstInstruction(
                     LLVMGetFirstBasicBlock(lowered_atomic)); value;
                 value = LLVMGetNextInstruction(value)) {
                LLVMOpcode op = LLVMGetInstructionOpcode(value);
                if (op == LLVMLoad) {
                    LLVMAtomicOrdering order = LLVMGetOrdering(value);
                    if (order == LLVMAtomicOrderingMonotonic)
                        found_atomic_load_relaxed = true;
                    else if (order == LLVMAtomicOrderingAcquire)
                        found_atomic_load_acquire = true;
                    else {
                        assert(order == LLVMAtomicOrderingSequentiallyConsistent);
                        found_atomic_load = true;
                    }
                }
                if (op == LLVMStore) {
                    LLVMAtomicOrdering order = LLVMGetOrdering(value);
                    if (order == LLVMAtomicOrderingMonotonic)
                        found_atomic_store_relaxed = true;
                    else if (order == LLVMAtomicOrderingRelease)
                        found_atomic_store_release = true;
                    else {
                        assert(order == LLVMAtomicOrderingSequentiallyConsistent);
                        found_atomic_store = true;
                    }
                }
                if (op == LLVMAtomicRMW) {
                    assert(LLVMGetAtomicRMWBinOp(value)
                        == LLVMAtomicRMWBinOpOr);
                    if (LLVMGetOrdering(value)
                        == LLVMAtomicOrderingRelease)
                        found_atomic_or_release = true;
                    else if (LLVMGetOrdering(value)
                             == LLVMAtomicOrderingMonotonic)
                        found_atomic_or_relaxed = true;
                    else {
                        assert(LLVMGetOrdering(value)
                            == LLVMAtomicOrderingSequentiallyConsistent);
                        found_atomic_or = true;
                    }
                }
            }
            assert(found_atomic_load && found_atomic_store && found_atomic_or
                && found_atomic_load_relaxed && found_atomic_load_acquire
                && found_atomic_store_relaxed && found_atomic_store_release
                && found_atomic_or_release && found_atomic_or_relaxed);
        }
        LLVMValueRef lowered_arithmetic = LLVMGetNamedFunction(parsed,
            "probe.arithmetic");
        assert((!is_comptime && lowered_arithmetic)
            || (is_comptime && !lowered_arithmetic));
        if (lowered_arithmetic) {
            assert(LLVMGetNamedFunction(parsed,
                "llvm.usub.with.overflow.i64"));
            assert(LLVMGetNamedFunction(parsed,
                "llvm.umul.with.overflow.i64"));
            bool found_division = false;
            bool found_remainder = false;
            bool found_cast = false;
            bool found_comparison = false;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(
                     lowered_arithmetic); block; block = LLVMGetNextBasicBlock(block))
                for (LLVMValueRef value = LLVMGetFirstInstruction(block);
                     value; value = LLVMGetNextInstruction(value)) {
                    LLVMOpcode op = LLVMGetInstructionOpcode(value);
                    found_division |= op == LLVMUDiv;
                    found_remainder |= op == LLVMURem;
                    found_cast |= op == LLVMTrunc;
                    found_comparison |= op == LLVMICmp;
                }
            assert(found_division && found_remainder && found_cast
                && found_comparison);
        }
        const char *cast_names[] = {
            "probe.uint_to_float", "probe.signed_to_float",
            "probe.float_to_uint", "probe.float_to_signed",
        };
        const LLVMOpcode cast_ops[] = {
            LLVMUIToFP, LLVMSIToFP, LLVMFPToUI, LLVMFPToSI,
        };
        for (size_t i = 0; i < sizeof(cast_names) / sizeof(cast_names[0]); i++) {
            LLVMValueRef cast_function = LLVMGetNamedFunction(parsed,
                cast_names[i]);
            assert((!is_comptime && cast_function)
                || (is_comptime && !cast_function));
            if (!cast_function) continue;
            bool found_conversion = false;
            unsigned range_checks = 0;
            for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(cast_function);
                 bb; bb = LLVMGetNextBasicBlock(bb))
                for (LLVMValueRef value = LLVMGetFirstInstruction(bb);
                     value; value = LLVMGetNextInstruction(value)) {
                    LLVMOpcode op = LLVMGetInstructionOpcode(value);
                    found_conversion |= op == cast_ops[i];
                    range_checks += op == LLVMFCmp;
                }
            assert(found_conversion && (i < 2 || range_checks == 2));
        }
        LLVMDisposeModule(parsed);
        LLVMContextDispose(reader);
        LLVMDisposeMemoryBuffer(source);
        core_buffer_release(&bytes);
    }

    assert(core_diagnostics_count(diagnostics) == 0);
    n00bb_llvm_backend_free(backend);

    assert(ir_global_add(runtime, "unsupported.runtime.global",
        unsigned_type));
    backend = n00bb_llvm_backend_prepare(comptime, runtime, true, false,
        diagnostics);
    assert(backend);
    core_buffer_t comptime_bytes = {0};
    assert(n00bb_llvm_backend_write_bitcode(backend, true,
        &comptime_bytes, diagnostics));
    assert(comptime_bytes.byte_len > 0);
    assert(core_diagnostics_count(diagnostics) == 0);
    core_buffer_release(&comptime_bytes);
    n00bb_llvm_backend_free(backend);

    ir_module_t *native_comptime = ir_module_new(context,
        "sys.internal.gc.alloc$comptime", &profile, target);
    ir_module_t *native_runtime = ir_module_new(context,
        "sys.internal.gc.alloc", &profile, target);
    ir_func_t *native_function = ir_func_add(native_runtime,
        "allocate_runtime", ir_type_parse(native_runtime, "() -> nil"),
        IR_LINK_INTERNAL);
    ir_block_t *native_entry = native_function
        ? ir_block_add(native_function, "entry") : nullptr;
    ir_func_t *native_mutator = ir_func_add(native_runtime,
        "mutator_for_native", ir_type_parse(native_runtime,
            "() -> nil"), IR_LINK_INTERNAL);
    ir_block_t *native_mutator_entry = native_mutator
        ? ir_block_add(native_mutator, "entry") : nullptr;
    assert(native_comptime && native_entry
        && native_mutator_entry
        && ir_instr_add(native_entry, IR_OP_RETURN)
        && ir_instr_add(native_mutator_entry, IR_OP_RETURN));
    backend = n00bb_llvm_backend_prepare(native_comptime, native_runtime,
        true, true, diagnostics);
    ir_func_t *native_roots[] = {native_function, native_mutator};
    assert(backend && n00bb_llvm_backend_bind_reachable_functions(
        backend, false, native_roots, 2));
    core_buffer_t native_bytes = {0};
    assert(n00bb_llvm_backend_write_bitcode(backend, false,
        &native_bytes, diagnostics));
    LLVMMemoryBufferRef native_source =
        LLVMCreateMemoryBufferWithMemoryRangeCopy(native_bytes.data,
            native_bytes.byte_len, "native-root");
    LLVMContextRef native_reader = LLVMContextCreate();
    LLVMModuleRef native_parsed = nullptr;
    assert(native_source && native_reader
        && !LLVMParseBitcodeInContext2(native_reader, native_source,
                                       &native_parsed));
    assert(LLVMGetNamedGlobalAlias(native_parsed, "n00b_gc_allocate",
                                   strlen("n00b_gc_allocate")));
    assert(LLVMGetNamedGlobalAlias(native_parsed, "n00b_gc_mutator",
                                   strlen("n00b_gc_mutator")));
    LLVMDisposeModule(native_parsed);
    LLVMContextDispose(native_reader);
    LLVMDisposeMemoryBuffer(native_source);
    core_buffer_release(&native_bytes);
    n00bb_llvm_backend_free(backend);

    core_diagnostics_free(diagnostics);
    ir_ctx_free(context);
    return 0;
}
