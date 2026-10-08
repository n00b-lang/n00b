#include <assert.h>
#include <string.h>

#include <llvm-c/BitReader.h>
#include <llvm-c/Core.h>

#include "ir/ir.h"
#include "ir/layout.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "llvm_backend.h"

int
main(void)
{
    ir_ctx_t *context = ir_ctx_new();
    const ir_scalar_t scalars[] = {
        {.type_text = "nil", .class = IR_CLASS_VOID},
        {.type_text = "uint", .class = IR_CLASS_UINT, .width = 64},
        {.type_text = "i128", .class = IR_CLASS_SINT, .width = 128},
    };
    const ir_profile_t profile = {
        .name = "llvm-test", .scalars = scalars, .n_scalars = 3,
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
    core_diagnostics_t *diagnostics = core_diagnostics_new();
    n00bb_llvm_backend_t *backend = n00bb_llvm_backend_prepare(
        comptime, runtime, true, diagnostics);
    assert(backend);
    assert(n00bb_llvm_backend_bind_phase_arrays(backend, &phase_array, 1));
    assert(core_diagnostics_count(diagnostics) == 0);

    for (int phase = 0; phase < 2; phase++) {
        core_buffer_t bytes = {0};
        bool is_comptime = phase == 0;
        assert(n00bb_llvm_backend_write_bitcode(
            backend, is_comptime, &bytes, diagnostics));
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
        LLVMValueRef lowered = LLVMGetNamedFunction(parsed, "probe.entry");
        assert((!is_comptime && lowered) || (is_comptime && !lowered));
        LLVMValueRef lowered_helper = LLVMGetNamedFunction(parsed,
            "probe.helper");
        assert((!is_comptime && lowered_helper)
            || (is_comptime && !lowered_helper));
        if (lowered_helper)
            assert(LLVMCountParams(lowered_helper) == 1
                && LLVMCountBasicBlocks(lowered_helper) == 1);
        LLVMValueRef lowered_array = LLVMGetNamedFunction(parsed,
            "probe.array_literal");
        assert((!is_comptime && lowered_array)
            || (is_comptime && !lowered_array));
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
        if (lowered) {
            assert(LLVMCountBasicBlocks(lowered) == 3);
            LLVMValueRef terminator = LLVMGetBasicBlockTerminator(
                LLVMGetLastBasicBlock(lowered));
            assert(terminator && LLVMGetInstructionOpcode(terminator)
                == LLVMRet);
            assert(LLVMGetNamedFunction(parsed,
                "llvm.uadd.with.overflow.i64"));
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
    backend = n00bb_llvm_backend_prepare(comptime, runtime, true,
        diagnostics);
    assert(backend);
    core_buffer_t comptime_bytes = {0};
    assert(n00bb_llvm_backend_write_bitcode(backend, true,
        &comptime_bytes, diagnostics));
    assert(comptime_bytes.byte_len > 0);
    assert(core_diagnostics_count(diagnostics) == 0);
    core_buffer_release(&comptime_bytes);
    n00bb_llvm_backend_free(backend);

    core_diagnostics_free(diagnostics);
    ir_ctx_free(context);
    return 0;
}
