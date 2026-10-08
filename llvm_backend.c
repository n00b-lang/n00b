#include "llvm_backend.h"

#include "ir/layout.h"
#include "ir/typelang.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/string.h"

struct n00bb_llvm_backend_t {
    LLVMContextRef context;
    LLVMModuleRef comptime;
    LLVMModuleRef runtime;
    const ir_module_t *comptime_source;
    const ir_module_t *runtime_source;
    bool comptime_lowered;
    bool runtime_lowered;
    bool debug_enabled;
    const n00bb_llvm_phase_array_t *phase_arrays;
    size_t phase_array_count;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
};

typedef struct {
    const ir_value_t *source;
    LLVMValueRef target;
} lowered_value_t;

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    ir_func_t *function;
    LLVMValueRef target_function;
    LLVMBasicBlockRef *blocks;
    LLVMValueRef *locals;
    lowered_value_t *values;
    size_t value_count;
    size_t value_capacity;
    core_diagnostics_t *diagnostics;
    const n00bb_llvm_phase_array_t *phase_arrays;
    size_t phase_array_count;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
} lower_state_t;

static bool
backend_error(core_diagnostics_t *diagnostics, const char *message)
{
    if (diagnostics)
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0006"), core_string_from_cstr(message));
    return false;
}

static bool
unsupported(lower_state_t *state, const ir_instr_t *instruction)
{
    core_buffer_t message = {0};
    core_buffer_printf(&message, "LLVM backend cannot lower '%s' yet",
        ir_op_spelling(instruction->op));
    if (state->diagnostics) {
        core_diagnostics_report_at(state->diagnostics, nullptr,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0006"),
            core_string_from_cstr(message.data), (core_diag_location_t){
                .line = instruction->span.start_line,
                .column = instruction->span.start_col,
                .end_line = instruction->span.end_line,
                .end_column = instruction->span.end_col,
                .has_location = instruction->span.start_line != 0,
            });
    }
    core_buffer_release(&message);
    return false;
}

static LLVMTypeRef
scalar_type(ir_module_t *module, th_type *type, LLVMContextRef context)
{
    ir_machine_t machine = ir_machine_of(module, type);
    if (machine.class == IR_CLASS_VOID) return LLVMVoidTypeInContext(context);
    if ((machine.class == IR_CLASS_BOOL || machine.class == IR_CLASS_SINT
         || machine.class == IR_CLASS_UINT) && machine.width)
        return LLVMIntTypeInContext(context, machine.width);
    if (machine.class == IR_CLASS_PTR)
        return LLVMPointerTypeInContext(context, 0);
    return nullptr;
}

static LLVMTypeRef
value_type(ir_module_t *module, th_type *type, LLVMContextRef context)
{
    LLVMTypeRef scalar = scalar_type(module, type, context);
    if (scalar) return scalar;
    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE) {
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef fields[] = {pointer, pointer};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    const char *name = nullptr;
    size_t arguments = 0;
    if (!th_named_info(th_resolve(type), &name, nullptr, nullptr,
                       &arguments) || !name) return nullptr;
    if (!strcmp(name, "string") && !arguments) {
        LLVMTypeRef word = LLVMInt64TypeInContext(context);
        LLVMTypeRef fields[] = {word, word};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    return nullptr;
}

static LLVMTypeRef
native_result_type(ir_module_t *module, th_type *signature,
                   LLVMContextRef context)
{
    th_type *success = th_func_ret(signature);
    th_type *failure = th_func_error(signature);
    LLVMTypeRef result = success
        ? value_type(module, success, context) : nullptr;
    if (!result || !failure
        || ir_machine_of(module, failure).class == IR_CLASS_VOID)
        return result;
    if (!value_type(module, failure, context)) return nullptr;
    const char *target = ir_module_target(module);
    const ir_final_layout_t *success_layout = ir_layout_final(module,
        success, target);
    const ir_final_layout_t *failure_layout = ir_layout_final(module,
        failure, target);
    if (!success_layout || !failure_layout) return nullptr;
    uint64_t bits = success_layout->intrinsic_bits
        > failure_layout->intrinsic_bits ? success_layout->intrinsic_bits
                                         : failure_layout->intrinsic_bits;
    uint32_t alignment = success_layout->alignment_bytes
        > failure_layout->alignment_bytes ? success_layout->alignment_bytes
                                          : failure_layout->alignment_bytes;
    if (!alignment || alignment > 16 || bits > UINT32_MAX * 8ULL)
        return nullptr;
    uint64_t bytes = (bits + 7) / 8;
    uint64_t units = (bytes + alignment - 1) / alignment;
    if (!units || units > UINT32_MAX) return nullptr;
    LLVMTypeRef word = LLVMIntTypeInContext(context, alignment * 8);
    LLVMTypeRef fields[] = {
        LLVMArrayType(word, (unsigned)units),
        LLVMInt8TypeInContext(context),
    };
    return LLVMStructTypeInContext(context, fields, 2, false);
}

static LLVMValueRef
lowered_value(lower_state_t *state, const ir_value_t *value)
{
    if (!value) return nullptr;
    if (value->kind == IR_VAL_PARAM && value->owner == state->function
        && value->index < LLVMCountParams(state->target_function))
        return LLVMGetParam(state->target_function, value->index);
    if (value->kind == IR_VAL_FUNC && value->owner) {
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "%s.%s",
            ir_module_name(value->owner->owner), value->owner->name);
        LLVMValueRef function = LLVMGetNamedFunction(state->module,
            symbol.data);
        core_buffer_release(&symbol);
        return function;
    }
    if (value->kind == IR_VAL_LOCAL && value->local) {
        for (uint32_t i = 0; i < state->function->n_locals; i++)
            if (state->function->locals[i] == value->local) {
                LLVMValueRef slot = state->locals[i];
                return LLVMBuildLoad2(state->builder,
                    LLVMGetAllocatedType(slot), slot, "local");
            }
    }
    for (size_t i = 0; i < state->value_count; i++)
        if (state->values[i].source == value)
            return state->values[i].target;
    return nullptr;
}

static LLVMBasicBlockRef
lowered_block(lower_state_t *state, const ir_block_t *block)
{
    for (uint32_t i = 0; i < state->function->n_blocks; i++)
        if (state->function->blocks[i] == block) return state->blocks[i];
    return nullptr;
}

static bool
save_result(lower_state_t *state, const ir_instr_t *instruction,
            LLVMValueRef value)
{
    if (instruction->dest_kind == IR_DEST_NONE) return true;
    if (!value) return false;
    if (instruction->dest_kind == IR_DEST_LOCAL) {
        for (uint32_t i = 0; i < state->function->n_locals; i++)
            if (state->function->locals[i] == instruction->dest_local) {
                LLVMBuildStore(state->builder, value, state->locals[i]);
                return true;
            }
        return false;
    }
    if (instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || instruction->dest->kind != IR_VAL_INSTR
        || state->value_count == state->value_capacity) return false;
    state->values[state->value_count++] = (lowered_value_t){
        instruction->dest, value
    };
    return true;
}

static LLVMValueRef
trap_function(lower_state_t *state)
{
    unsigned id = LLVMLookupIntrinsicID("llvm.trap", sizeof("llvm.trap") - 1);
    return LLVMGetIntrinsicDeclaration(state->module, id, nullptr, 0);
}

static void
emit_trap(lower_state_t *state)
{
    LLVMValueRef function = trap_function(state);
    LLVMBuildCall2(state->builder, LLVMGlobalGetValueType(function), function,
                   nullptr, 0, "");
    LLVMBuildUnreachable(state->builder);
}

static bool
emit_outcome_return(lower_state_t *state, LLVMValueRef value,
                    bool failure)
{
    LLVMTypeRef type = LLVMGetReturnType(
        LLVMGlobalGetValueType(state->target_function));
    if (LLVMGetTypeKind(type) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(type) != 2) return false;
    LLVMValueRef storage = LLVMBuildAlloca(state->builder, type, "outcome");
    LLVMBuildStore(state->builder, LLVMConstNull(type), storage);
    if (value) {
        LLVMValueRef payload = LLVMBuildStructGEP2(state->builder, type,
            storage, 0, "outcome.payload");
        LLVMBuildStore(state->builder, value, payload);
    }
    LLVMValueRef tag = LLVMBuildStructGEP2(state->builder, type, storage,
        1, "outcome.tag");
    LLVMBuildStore(state->builder,
        LLVMConstInt(LLVMInt8TypeInContext(
            LLVMGetModuleContext(state->module)), failure ? 1 : 0, false),
        tag);
    LLVMBuildRet(state->builder, LLVMBuildLoad2(state->builder, type,
        storage, "outcome.value"));
    return true;
}

static LLVMValueRef
array_descriptor(lower_state_t *state, uint64_t count)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef descriptor_fields[] = {i64, i32, i32, i32, i32, i64};
    LLVMTypeRef descriptor_type = LLVMStructTypeInContext(context,
        descriptor_fields, 6, false);
    LLVMValueRef descriptor_fields_value[] = {
        LLVMConstInt(i64, count, false), LLVMConstInt(i32, 1, false),
        LLVMConstInt(i32, 1, false), LLVMConstNull(i32),
        LLVMConstNull(i32), LLVMConstInt(i64, count, false),
    };
    LLVMValueRef descriptor = LLVMAddGlobal(state->module,
        descriptor_type, "n00bb.literal.array.descriptor");
    LLVMSetInitializer(descriptor,
        LLVMConstStructInContext(context, descriptor_fields_value, 6, false));
    LLVMSetGlobalConstant(descriptor, true);
    LLVMSetLinkage(descriptor, LLVMPrivateLinkage);
    return descriptor;
}

static LLVMValueRef
array_value_with_descriptor(lower_state_t *state, LLVMValueRef data,
                            uint64_t count)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef descriptor = array_descriptor(state, count);
    LLVMValueRef array_fields[] = {data, descriptor};
    return LLVMConstStructInContext(context, array_fields, 2, false);
}

static LLVMValueRef
constant_array(lower_state_t *state, ir_instr_t *instruction)
{
    ir_aggregate_form_t form;
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    ir_machine_t machine = element
        ? ir_machine_of(state->function->owner, element)
        : (ir_machine_t){0};
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = element
        ? value_type(state->function->owner, element, context) : nullptr;
    LLVMTypeRef array_type = type
        ? value_type(state->function->owner, type, context) : nullptr;
    uint32_t count = instruction->n_operands;
    if ((ir_instr_aggregate_form(instruction, &form)
            && form != IR_AGGREGATE_ELEMENTS)
        || !element_type || !array_type
        || LLVMGetTypeKind(element_type) != LLVMIntegerTypeKind
        || machine.width < 8 || machine.width % 8
        || (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)
        || LLVMGetIntTypeWidth(element_type) != machine.width)
        return nullptr;

    LLVMValueRef *items = calloc(count ? count : 1, sizeof(*items));
    if (!items) return nullptr;
    bool valid = true;
    for (uint32_t i = 0; valid && i < count; i++) {
        ir_operand_t *operand = &instruction->operands[i];
        items[i] = operand->kind == IR_OPD_VALUE
            ? lowered_value(state, operand->value) : nullptr;
        valid = items[i] && LLVMTypeOf(items[i]) == element_type
            && LLVMIsConstant(items[i]);
    }
    if (!valid) {
        free(items);
        return nullptr;
    }

    LLVMTypeRef data_type = LLVMArrayType(element_type, count ? count : 1);
    LLVMValueRef data = LLVMAddGlobal(state->module, data_type,
        "n00bb.literal.array.data");
    if (!count) items[0] = LLVMConstNull(element_type);
    LLVMSetInitializer(data, LLVMConstArray(element_type, items,
        count ? count : 1));
    LLVMSetGlobalConstant(data, true);
    LLVMSetLinkage(data, LLVMPrivateLinkage);
    free(items);

    return array_value_with_descriptor(state, data, count);
}

static LLVMValueRef
materialized_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    const ir_final_layout_t *layout = element
        ? ir_layout_final(state->function->owner, element,
            ir_module_target(state->function->owner)) : nullptr;
    ir_machine_t machine = element
        ? ir_machine_of(state->function->owner, element)
        : (ir_machine_t){0};
    if (!identifier || !element || !layout || !layout->alignment_bytes
        || !layout->intrinsic_bits || layout->intrinsic_bits % 8
        || layout->intrinsic_bits != machine.width
        || (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)) return nullptr;
    uint64_t width = layout->intrinsic_bits / 8;
    uint64_t alignment = layout->alignment_bytes;
    if (width > UINT64_MAX - alignment + 1) return nullptr;
    uint64_t stride = ((width + alignment - 1) / alignment) * alignment;
    if (!stride || stride > UINT32_MAX) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *array = &state->phase_arrays[i];
        if (!array->identifier || strcmp(identifier, array->identifier))
            continue;
        if (array->element_stride != stride || array->count > UINT32_MAX
            || array->count > UINT32_MAX / stride || !array->bytes)
            return nullptr;
        unsigned byte_count = (unsigned)(array->count * stride);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMValueRef contents = LLVMConstStringInContext(context,
            byte_count ? (const char *)array->bytes : "\0",
            byte_count ? byte_count : 1, true);
        LLVMValueRef data = LLVMAddGlobal(state->module,
            LLVMTypeOf(contents), "n00bb.literal.array.cached");
        LLVMSetInitializer(data, contents);
        LLVMSetAlignment(data, (unsigned)alignment);
        LLVMSetGlobalConstant(data, true);
        LLVMSetLinkage(data, LLVMPrivateLinkage);
        return array_value_with_descriptor(state, data, array->count);
    }
    return nullptr;
}

static LLVMValueRef
dynamic_array(lower_state_t *state, ir_instr_t *instruction)
{
    ir_aggregate_form_t form;
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    if (!element || (ir_instr_aggregate_form(instruction, &form)
                     && form != IR_AGGREGATE_ELEMENTS)) return nullptr;
    ir_module_t *source = state->function->owner;
    ir_machine_t machine = ir_machine_of(source, element);
    const ir_final_layout_t *layout = ir_layout_final(source, element,
        ir_module_target(source));
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = value_type(source, element, context);
    LLVMTypeRef result_type = value_type(source, type, context);
    if (!layout || !layout->alignment_bytes || !element_type || !result_type
        || layout->intrinsic_bits != machine.width || machine.width < 8
        || machine.width % 8
        || (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)) return nullptr;
    uint64_t element_bytes = machine.width / 8;
    uint64_t element_align = layout->alignment_bytes;
    if (element_bytes > UINT64_MAX - element_align + 1) return nullptr;
    uint64_t stride = ((element_bytes + element_align - 1) / element_align)
        * element_align;
    uint64_t alignment = element_align > 8 ? element_align : 8;
    uint64_t descriptor_bytes = sizeof(n00b_abi_array_descriptor_header_v1)
        + sizeof(uint64_t);
    uint64_t data_offset = ((descriptor_bytes + alignment - 1) / alignment)
        * alignment;
    uint64_t count = instruction->n_operands;
    if (!stride || alignment > UINT32_MAX
        || count > (UINT64_MAX - data_offset) / stride) return nullptr;
    uint64_t byte_size = data_offset + (count ? count * stride : 1);
    uint64_t typehash = th_hash(ir_module_types(source), type);
    uint32_t type_index = 0;
    for (size_t i = 0; i < state->type_index_count; i++)
        if (state->type_indices[i].typehash == typehash) {
            type_index = state->type_indices[i].index;
            break;
        }
    if (!type_index) return nullptr;

    LLVMValueRef descriptor = array_descriptor(state, count);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
        allocator_params, 5, false);
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        "n00b_allocate");
    if (!allocator)
        allocator = LLVMAddFunction(state->module, "n00b_allocate",
                                    allocator_type);
    LLVMValueRef allocator_args[] = {
        LLVMConstNull(i32), LLVMConstInt(i32, type_index, false),
        LLVMConstInt(i64, byte_size, false),
        LLVMConstInt(i32, alignment, false), descriptor,
    };
    LLVMValueRef storage = LLVMBuildCall2(state->builder, allocator_type,
        allocator, allocator_args, 5, "array.storage");
    LLVMTypeRef descriptor_type = LLVMGlobalGetValueType(descriptor);
    LLVMValueRef descriptor_value = LLVMBuildLoad2(state->builder,
        descriptor_type, descriptor, "array.shape");
    LLVMBuildStore(state->builder, descriptor_value, storage);
    LLVMValueRef offset = LLVMConstInt(i64, data_offset, false);
    LLVMValueRef data = LLVMBuildGEP2(state->builder,
        LLVMInt8TypeInContext(context), storage, &offset, 1, "array.data");
    for (uint32_t i = 0; i < instruction->n_operands; i++) {
        ir_operand_t *operand = &instruction->operands[i];
        LLVMValueRef item = operand->kind == IR_OPD_VALUE
            ? lowered_value(state, operand->value) : nullptr;
        if (!item || LLVMTypeOf(item) != element_type) return nullptr;
        LLVMValueRef position = LLVMConstInt(i64, (uint64_t)i * stride, false);
        LLVMValueRef slot = LLVMBuildGEP2(state->builder,
            LLVMInt8TypeInContext(context), data, &position, 1,
            "array.element");
        LLVMBuildStore(state->builder, item, slot);
    }
    LLVMValueRef array = LLVMGetUndef(result_type);
    array = LLVMBuildInsertValue(state->builder, array, data, 0,
                                 "array.with.data");
    return LLVMBuildInsertValue(state->builder, array, storage, 1,
                                "array.with.descriptor");
}

static LLVMValueRef
string_value(lower_state_t *state, const uint8_t *bytes, size_t length,
             uint64_t encoding)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    if (!bytes || length > UINT32_MAX || encoding > N00B_ABI_STRING_UTF8)
        return nullptr;
    LLVMValueRef fields[2];
    if (length <= N00BB_ABI_STRING_SHORT_CAPACITY) {
        n00b_abi_string_v1 value = {0};
        if (length) memcpy(value.short_value.bytes, bytes, length);
        value.short_value.selector = (uint8_t)(length
            | (encoding << 4) | (N00B_ABI_STRING_SHORT << 6));
        fields[0] = LLVMConstInt(i64, value.words[0], false);
        fields[1] = LLVMConstInt(i64, value.words[1], false);
    } else {
        LLVMValueRef contents = LLVMConstStringInContext(context,
            (const char *)bytes, (unsigned)length, true);
        LLVMValueRef data = LLVMAddGlobal(state->module,
            LLVMTypeOf(contents), "n00bb.literal.string.data");
        LLVMSetInitializer(data, contents);
        LLVMSetGlobalConstant(data, true);
        LLVMSetLinkage(data, LLVMPrivateLinkage);
        fields[0] = LLVMConstPtrToInt(data, i64);
        fields[1] = LLVMConstInt(i64, length
            | (encoding << N00B_ABI_STRING_ENCODING_SHIFT)
            | ((uint64_t)N00B_ABI_STRING_LONG
                << N00B_ABI_STRING_TAG_SHIFT), false);
    }
    return LLVMConstStructInContext(context, fields, 2, false);
}

static bool
is_string_type(th_type *type)
{
    const char *name = nullptr;
    size_t length = 0, arguments = 0;
    return type && th_named_info(th_resolve(type), &name, &length,
                                 nullptr, &arguments)
        && length == 6 && !memcmp(name, "string", 6) && !arguments;
}

static LLVMValueRef
constant_string(lower_state_t *state, ir_instr_t *instruction)
{
    size_t length = 0;
    const uint8_t *bytes = ir_instr_const_bytes(instruction, &length);
    if (!instruction->dest || !is_string_type(instruction->dest->type)
        || ir_annot_get(&instruction->annots, "n00bb.rich.metadata"))
        return nullptr;
    bool ascii = true;
    for (size_t i = 0; i < length; i++)
        if (bytes[i] & 0x80) ascii = false;
    uint64_t encoding = ascii ? N00B_ABI_STRING_ASCII
                              : N00B_ABI_STRING_UTF8;
    return string_value(state, bytes, length, encoding);
}

static LLVMValueRef
materialized_string(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    if (!identifier || !instruction->dest
        || !is_string_type(instruction->dest->type)) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *value = &state->phase_arrays[i];
        if (value->kind == N00BB_LLVM_LITERAL_STRING
            && value->identifier && !strcmp(identifier, value->identifier))
            return string_value(state, value->bytes, value->count,
                                value->string_encoding);
    }
    return nullptr;
}

static bool
lower_instruction(lower_state_t *state, ir_instr_t *instruction)
{
    LLVMValueRef result = nullptr;
    ir_operand_t *operands = instruction->operands;
    uint32_t count = instruction->n_operands;
    switch (instruction->op) {
    case IR_OP_CONST: {
        if (ir_instr_const_bytes(instruction, nullptr)) {
            result = constant_string(state, instruction);
            return result ? save_result(state, instruction, result)
                          : unsupported(state, instruction);
        }
        if (count != 1 || operands[0].kind != IR_OPD_CONST
            || operands[0].lit.is_float || !instruction->dest) break;
        ir_machine_t machine = ir_machine_of(state->function->owner,
            instruction->dest->type);
        if (!machine.width || machine.width > 128
            || (machine.class != IR_CLASS_BOOL
                && machine.class != IR_CLASS_SINT
                && machine.class != IR_CLASS_UINT)) break;
        LLVMTypeRef type = scalar_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) != LLVMIntegerTypeKind) break;
        ir_lit_t literal = operands[0].lit.i;
        ir_uint_t maximum = machine.width == 128 ? ~(ir_uint_t)0
            : (((ir_uint_t)1 << machine.width) - 1);
        if (machine.class == IR_CLASS_SINT) {
            if (literal.mag > (literal.neg ? (maximum >> 1) + 1
                                           : maximum >> 1)) break;
        } else if (literal.neg || literal.mag > maximum) break;
        ir_uint_t bits = literal.neg ? (ir_uint_t)0 - literal.mag
                                     : literal.mag;
        if (machine.width <= 64)
            result = LLVMConstInt(type, (uint64_t)bits, false);
        else {
            uint64_t words[] = {(uint64_t)bits, (uint64_t)(bits >> 64)};
            result = LLVMConstIntOfArbitraryPrecision(type, 2, words);
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_COPY:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE) break;
        result = lowered_value(state, operands[0].value);
        return result && save_result(state, instruction, result);
    case IR_OP_AGGREGATE:
        result = constant_array(state, instruction);
        if (!result) result = dynamic_array(state, instruction);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    case IR_OP_PHASE_RESULT:
        result = materialized_array(state, instruction);
        if (!result) result = materialized_string(state, instruction);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    case IR_OP_ADD: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        if (instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap"))) break;
        if (ir_instr_has_mod(instruction, "trap")) {
            ir_machine_t machine = ir_machine_of(state->function->owner,
                operands[0].value->type);
            const char *intrinsic_name = machine.class == IR_CLASS_SINT
                ? "llvm.sadd.with.overflow"
                : machine.class == IR_CLASS_UINT
                    ? "llvm.uadd.with.overflow" : nullptr;
            if (!intrinsic_name) break;
            unsigned id = LLVMLookupIntrinsicID(intrinsic_name,
                strlen(intrinsic_name));
            LLVMTypeRef type = LLVMTypeOf(left);
            LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(state->module,
                id, &type, 1);
            LLVMValueRef args[] = {left, right};
            LLVMValueRef pair = LLVMBuildCall2(state->builder,
                LLVMGlobalGetValueType(intrinsic), intrinsic, args, 2, "add.checked");
            result = LLVMBuildExtractValue(state->builder, pair, 0, "sum");
            LLVMValueRef overflow = LLVMBuildExtractValue(state->builder, pair,
                1, "overflow");
            LLVMValueRef function = LLVMGetBasicBlockParent(
                LLVMGetInsertBlock(state->builder));
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMBasicBlockRef bad = LLVMAppendBasicBlockInContext(context,
                function, "overflow");
            LLVMBasicBlockRef good = LLVMAppendBasicBlockInContext(context,
                function, "add.cont");
            LLVMBuildCondBr(state->builder, overflow, bad, good);
            LLVMPositionBuilderAtEnd(state->builder, bad);
            emit_trap(state);
            LLVMPositionBuilderAtEnd(state->builder, good);
        } else {
            result = LLVMBuildAdd(state->builder, left, right, "sum");
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_EQ: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, left, right, "equal");
        return save_result(state, instruction, result);
    }
    case IR_OP_VARIANT_TAG: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !operands[0].value->def
            || operands[0].value->def->op != IR_OP_CALL
            || !instruction->dest) break;
        LLVMValueRef outcome = lowered_value(state, operands[0].value);
        LLVMTypeRef type = outcome ? LLVMTypeOf(outcome) : nullptr;
        LLVMTypeRef dest = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(type) != 2 || !dest
            || LLVMGetTypeKind(dest) != LLVMIntegerTypeKind) break;
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, outcome,
            1, "failure.tag");
        LLVMValueRef success = LLVMBuildICmp(state->builder, LLVMIntEQ,
            tag, LLVMConstInt(LLVMTypeOf(tag), 0, false),
            "variant.is.ok");
        result = LLVMBuildSelect(state->builder, success,
            LLVMConstInt(dest, th_arm_tag("ok"), false),
            LLVMConstInt(dest, th_arm_tag("error"), false),
            "variant.tag");
        return save_result(state, instruction, result);
    }
    case IR_OP_VARIANT_PAYLOAD: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME
            || !operands[0].value || !operands[0].value->def
            || operands[0].value->def->op != IR_OP_CALL
            || !instruction->dest) break;
        LLVMValueRef outcome = lowered_value(state, operands[0].value);
        LLVMTypeRef type = outcome ? LLVMTypeOf(outcome) : nullptr;
        LLVMTypeRef payload_type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(type) != 2
            || !payload_type) break;
        LLVMValueRef storage = LLVMBuildAlloca(state->builder, type,
            "outcome.storage");
        LLVMBuildStore(state->builder, outcome, storage);
        LLVMValueRef payload = LLVMBuildStructGEP2(state->builder, type,
            storage, 0, "outcome.payload");
        result = LLVMBuildLoad2(state->builder, payload_type, payload,
            "selected.payload");
        return save_result(state, instruction, result);
    }
    case IR_OP_CALL: {
        if (!count || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || operands[0].value->kind != IR_VAL_FUNC)
            break;
        LLVMValueRef callee = lowered_value(state, operands[0].value);
        if (!callee || LLVMGetTypeKind(LLVMGlobalGetValueType(callee))
                           != LLVMFunctionTypeKind
            || LLVMCountParamTypes(LLVMGlobalGetValueType(callee))
                           != count - 1) break;
        LLVMValueRef *arguments = count > 1
            ? calloc(count - 1, sizeof(*arguments)) : nullptr;
        if (count > 1 && !arguments) break;
        bool valid = true;
        for (uint32_t i = 1; valid && i < count; i++) {
            valid = operands[i].kind == IR_OPD_VALUE
                && (arguments[i - 1] = lowered_value(state,
                                             operands[i].value)) != nullptr;
        }
        if (valid)
            result = LLVMBuildCall2(state->builder,
                LLVMGlobalGetValueType(callee), callee, arguments,
                count - 1, instruction->dest ? "call" : "");
        free(arguments);
        if (!valid || !result) break;
        return instruction->dest ? save_result(state, instruction, result)
                                 : true;
    }
    case IR_OP_BRANCH: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_BLOCK
            || operands[2].kind != IR_OPD_BLOCK) break;
        LLVMValueRef condition = lowered_value(state, operands[0].value);
        LLVMBasicBlockRef yes = lowered_block(state, operands[1].block);
        LLVMBasicBlockRef no = lowered_block(state, operands[2].block);
        if (!condition || !yes || !no) break;
        LLVMBuildCondBr(state->builder, condition, yes, no);
        return true;
    }
    case IR_OP_JUMP: {
        if (count != 1 || operands[0].kind != IR_OPD_BLOCK) break;
        LLVMBasicBlockRef target = lowered_block(state, operands[0].block);
        if (!target) break;
        LLVMBuildBr(state->builder, target);
        return true;
    }
    case IR_OP_UNREACHABLE:
        if (count == 0) {
            LLVMBuildUnreachable(state->builder);
            return true;
        }
        break;
    case IR_OP_RETURN:
        if (th_func_error(state->function->type)
            && ir_machine_of(state->function->owner,
                th_func_error(state->function->type)).class != IR_CLASS_VOID) {
            if (count > 1 || (count && operands[0].kind != IR_OPD_VALUE))
                break;
            result = count ? lowered_value(state, operands[0].value)
                           : nullptr;
            if (count && !result) break;
            return emit_outcome_return(state, result, false);
        }
        if (count == 0) {
            LLVMBuildRetVoid(state->builder);
            return true;
        }
        if (count == 1 && operands[0].kind == IR_OPD_VALUE) {
            result = lowered_value(state, operands[0].value);
            if (!result || LLVMTypeOf(result) != LLVMGetReturnType(
                    LLVMGlobalGetValueType(state->target_function))) break;
            LLVMBuildRet(state->builder, result);
            return true;
        }
        break;
    case IR_OP_FAIL:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !th_func_error(state->function->type)
            || ir_machine_of(state->function->owner,
                th_func_error(state->function->type)).class == IR_CLASS_VOID)
            break;
        result = lowered_value(state, operands[0].value);
        return result && emit_outcome_return(state, result, true);
    case IR_OP_TRAP:
        if (count == 1 && operands[0].kind == IR_OPD_VALUE
            && operands[0].value
            && is_string_type(operands[0].value->type)) {
            LLVMValueRef message = lowered_value(state, operands[0].value);
            if (!message) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMTypeRef parameters[] = {i32, i64, i64};
            LLVMTypeRef writer_type = LLVMFunctionType(i32, parameters,
                3, false);
            LLVMValueRef writer = LLVMGetNamedFunction(state->module,
                "n00bb_comptime_emit_failure");
            if (!writer)
                writer = LLVMAddFunction(state->module,
                    "n00bb_comptime_emit_failure", writer_type);
            LLVMValueRef arguments[] = {
                LLVMConstInt(i32, N00BB_COMPTIME_RESULT_FD, false),
                LLVMBuildExtractValue(state->builder, message, 0,
                                      "failure.low"),
                LLVMBuildExtractValue(state->builder, message, 1,
                                      "failure.high"),
            };
            LLVMBuildCall2(state->builder, writer_type, writer, arguments,
                3, "failure.write");
        } else if (count != 0) break;
        emit_trap(state);
        return true;
    default:
        break;
    }
    return unsupported(state, instruction);
}

static LLVMTypeRef
function_type(LLVMModuleRef target, ir_func_t *function)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(function->owner, function->type,
        context);
    if (!result || th_func_nkw(function->type)
        || th_func_rest(function->type))
        return nullptr;
    size_t count = th_func_nparams(function->type);
    if (count > UINT32_MAX) return nullptr;
    LLVMTypeRef *parameters = count
        ? calloc(count, sizeof(*parameters)) : nullptr;
    if (count && !parameters) return nullptr;
    for (size_t i = 0; i < count; i++) {
        const th_param *parameter = th_func_param(function->type, i);
        parameters[i] = parameter
            ? value_type(function->owner, parameter->type, context)
            : nullptr;
        if (!parameters[i] || LLVMGetTypeKind(parameters[i])
                                == LLVMVoidTypeKind) {
            free(parameters);
            return nullptr;
        }
    }
    LLVMTypeRef signature = LLVMFunctionType(result, parameters,
        (unsigned)count, false);
    free(parameters);
    return signature;
}

static bool
lower_function(LLVMModuleRef target, ir_func_t *function,
               const n00bb_llvm_phase_array_t *phase_arrays,
               size_t phase_array_count,
               const n00bb_llvm_type_index_t *type_indices,
               size_t type_index_count,
               core_diagnostics_t *diagnostics)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    core_buffer_t symbol = {0};
    core_buffer_printf(&symbol, "%s.%s", ir_module_name(function->owner),
        function->name);
    LLVMValueRef target_function = LLVMGetNamedFunction(target, symbol.data);
    core_buffer_release(&symbol);
    if (!target_function || !function->n_blocks
        || function->n_params != LLVMCountParams(target_function)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "LLVM backend cannot lower function '%s': its signature or body is unavailable",
            function->name);
        if (diagnostics)
            core_diagnostics_report_at(diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0006"),
                core_string_from_cstr(message.data), (core_diag_location_t){
                    .line = function->span.start_line,
                    .column = function->span.start_col,
                    .end_line = function->span.end_line,
                    .end_column = function->span.end_col,
                    .has_location = function->span.start_line != 0,
                });
        core_buffer_release(&message);
        return false;
    }
    lower_state_t state = {.module = target, .function = function,
        .target_function = target_function, .diagnostics = diagnostics,
        .phase_arrays = phase_arrays,
        .phase_array_count = phase_array_count,
        .type_indices = type_indices,
        .type_index_count = type_index_count};
    state.blocks = calloc(function->n_blocks, sizeof(*state.blocks));
    state.locals = calloc(function->n_locals ? function->n_locals : 1,
        sizeof(*state.locals));
    for (uint32_t i = 0; i < function->n_blocks; i++)
        state.value_capacity += function->blocks[i]->n_instrs;
    state.values = calloc(state.value_capacity ? state.value_capacity : 1,
        sizeof(*state.values));
    state.builder = LLVMCreateBuilderInContext(context);
    bool ok = state.blocks && state.locals && state.values && state.builder;
    if (ok) {
        for (uint32_t i = 0; i < function->n_blocks; i++)
            state.blocks[i] = LLVMAppendBasicBlockInContext(context,
                target_function, function->blocks[i]->name);
        LLVMPositionBuilderAtEnd(state.builder, state.blocks[0]);
        for (uint32_t i = 0; i < function->n_locals; i++) {
            ir_local_t *local = function->locals[i];
            LLVMTypeRef type = value_type(function->owner, local->type, context);
            if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind
                || local->has_init) {
                ok = backend_error(diagnostics,
                    "LLVM backend cannot lower this local declaration yet");
                break;
            }
            state.locals[i] = LLVMBuildAlloca(state.builder, type, local->name);
        }
    }
    for (uint32_t i = 0; ok && i < function->n_blocks; i++) {
        LLVMPositionBuilderAtEnd(state.builder, state.blocks[i]);
        ir_block_t *block = function->blocks[i];
        for (uint32_t j = 0; ok && j < block->n_instrs; j++)
            ok = lower_instruction(&state, block->instrs[j]);
    }
    if (state.builder) LLVMDisposeBuilder(state.builder);
    free(state.blocks);
    free(state.locals);
    free(state.values);
    if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics)))
        backend_error(diagnostics, "could not lower LLVM function");
    return ok;
}

static bool
lower_module(LLVMModuleRef target, const ir_module_t *source,
             const n00bb_llvm_phase_array_t *phase_arrays,
             size_t phase_array_count,
             const n00bb_llvm_type_index_t *type_indices,
             size_t type_index_count,
             core_diagnostics_t *diagnostics)
{
    if (ir_global_count(source))
        return backend_error(diagnostics,
            "LLVM backend cannot lower module globals yet");
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        LLVMTypeRef signature = function_type(target, function);
        if (!signature) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "LLVM backend cannot lower signature for '%s.%s' yet",
                ir_module_name(source), function->name);
            backend_error(diagnostics, message.data);
            core_buffer_release(&message);
            return false;
        }
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "%s.%s", ir_module_name(source),
            function->name);
        LLVMValueRef declared = LLVMAddFunction(target, symbol.data,
            signature);
        core_buffer_release(&symbol);
        if (!declared)
            return backend_error(diagnostics,
                "LLVM backend could not declare function");
    }
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        if (function->n_blocks
            && !lower_function(target, function, phase_arrays,
                               phase_array_count, type_indices,
                               type_index_count, diagnostics)) return false;
    }
    return true;
}

n00bb_llvm_backend_t *
n00bb_llvm_backend_prepare(const ir_module_t *comptime_ir,
                           const ir_module_t *runtime_ir,
                           bool debug_enabled,
                           core_diagnostics_t *diagnostics)
{
    if (!comptime_ir || !runtime_ir
        || !ir_module_name(comptime_ir) || !ir_module_name(runtime_ir)
        || !ir_module_target(comptime_ir) || !ir_module_target(runtime_ir)) {
        backend_error(diagnostics, "LLVM backend needs both targeted phase modules");
        return nullptr;
    }
    n00bb_llvm_backend_t *backend = core_alloc(n00bb_llvm_backend_t);
    if (!backend) {
        backend_error(diagnostics, "could not allocate LLVM backend state");
        return nullptr;
    }
    backend->debug_enabled = debug_enabled;
    backend->comptime_source = comptime_ir;
    backend->runtime_source = runtime_ir;
    backend->context = LLVMContextCreate();
    if (!backend->context) goto fail;
    backend->comptime = LLVMModuleCreateWithNameInContext(
        ir_module_name(comptime_ir), backend->context);
    backend->runtime = LLVMModuleCreateWithNameInContext(
        ir_module_name(runtime_ir), backend->context);
    if (!backend->comptime || !backend->runtime) goto fail;
    LLVMSetTarget(backend->comptime, ir_module_target(comptime_ir));
    LLVMSetTarget(backend->runtime, ir_module_target(runtime_ir));
    return backend;

fail:
    n00bb_llvm_backend_free(backend);
    if (!diagnostics || !core_diagnostics_failed(diagnostics))
        backend_error(diagnostics, "could not initialize LLVM phase modules");
    return nullptr;
}

void
n00bb_llvm_backend_free(n00bb_llvm_backend_t *backend)
{
    if (!backend) return;
    if (backend->comptime) LLVMDisposeModule(backend->comptime);
    if (backend->runtime) LLVMDisposeModule(backend->runtime);
    if (backend->context) LLVMContextDispose(backend->context);
    core_free(backend);
}

bool
n00bb_llvm_backend_bind_phase_arrays(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_phase_array_t *arrays,
    size_t count)
{
    if (!backend || backend->runtime_lowered || (count && !arrays))
        return false;
    backend->phase_arrays = arrays;
    backend->phase_array_count = count;
    return true;
}

bool
n00bb_llvm_backend_bind_type_indices(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_type_index_t *types,
    size_t count)
{
    if (!backend || backend->runtime_lowered || (count && !types))
        return false;
    backend->type_indices = types;
    backend->type_index_count = count;
    return true;
}

static bool
lower_phase(n00bb_llvm_backend_t *backend, bool comptime,
            core_diagnostics_t *diagnostics)
{
    bool *lowered = comptime ? &backend->comptime_lowered
                             : &backend->runtime_lowered;
    if (*lowered) return true;
    if (!lower_module(comptime ? backend->comptime : backend->runtime,
                      comptime ? backend->comptime_source
                               : backend->runtime_source,
                      comptime ? nullptr : backend->phase_arrays,
                      comptime ? 0 : backend->phase_array_count,
                      comptime ? nullptr : backend->type_indices,
                      comptime ? 0 : backend->type_index_count,
                      diagnostics))
        return false;
    *lowered = true;
    return true;
}

bool
n00bb_llvm_backend_write_bitcode(n00bb_llvm_backend_t *backend,
                                  bool comptime, core_buffer_t *out,
                                  core_diagnostics_t *diagnostics)
{
    if (!backend || !out)
        return backend_error(diagnostics, "invalid LLVM bitcode probe arguments");
    if (!lower_phase(backend, comptime, diagnostics)) return false;
    LLVMModuleRef module = comptime ? backend->comptime : backend->runtime;
    char *error = nullptr;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error)) {
        backend_error(diagnostics, error ? error : "LLVM module verification failed");
        if (error) LLVMDisposeMessage(error);
        return false;
    }
    if (error) LLVMDisposeMessage(error);
    LLVMMemoryBufferRef bitcode = LLVMWriteBitcodeToMemoryBuffer(module);
    if (!bitcode)
        return backend_error(diagnostics, "could not serialize LLVM bitcode");
    core_buffer_append(out, LLVMGetBufferStart(bitcode), LLVMGetBufferSize(bitcode));
    LLVMDisposeMemoryBuffer(bitcode);
    return true;
}

bool
n00bb_llvm_backend_emit_object(n00bb_llvm_backend_t *backend,
                               bool comptime,
                               const char *const *entry_initializers,
                               size_t initializer_count,
                               const n00bb_llvm_array_producer_t *array_producers,
                               size_t array_producer_count,
                               size_t metadata_bytes,
                               core_buffer_t *out,
                               core_diagnostics_t *diagnostics)
{
    if (!backend || !out || (initializer_count && !entry_initializers)
        || (array_producer_count && (!comptime || !array_producers))
        || (!comptime && initializer_count && !metadata_bytes))
        return backend_error(diagnostics, "invalid LLVM object arguments");
    if (!lower_phase(backend, comptime, diagnostics)) return false;
    LLVMModuleRef module = comptime ? backend->comptime : backend->runtime;
    LLVMContextRef context = backend->context;
    if (initializer_count || array_producer_count) {
        LLVMTypeRef result = LLVMInt32TypeInContext(context);
        LLVMTypeRef signature = LLVMFunctionType(result, nullptr, 0, false);
        LLVMTypeRef initializer_type = LLVMFunctionType(
            LLVMVoidTypeInContext(context), nullptr, 0, false);
        LLVMValueRef entry = LLVMAddFunction(module, "main", signature);
        LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(context, entry,
            "entry");
        LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
        LLVMPositionBuilderAtEnd(builder, block);
        if (!comptime) {
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef fields[] = {
                result, result, pointer, LLVMInt64TypeInContext(context)
            };
            LLVMTypeRef descriptor_type = LLVMStructTypeInContext(context,
                fields, 4, false);
            LLVMValueRef metadata = LLVMAddGlobal(module,
                LLVMInt8TypeInContext(context), "n00b_abi_metadata");
            LLVMValueRef descriptor = LLVMAddGlobal(module, descriptor_type,
                "__n00bb_image_descriptor");
            LLVMValueRef values[] = {
                LLVMConstInt(result, 1, false), LLVMConstInt(result, 0, false),
                LLVMConstBitCast(metadata, pointer),
                LLVMConstInt(fields[3], metadata_bytes, false)
            };
            LLVMSetInitializer(descriptor,
                LLVMConstNamedStruct(descriptor_type, values, 4));
            LLVMSetLinkage(descriptor, LLVMInternalLinkage);
            LLVMSetGlobalConstant(descriptor, true);
            LLVMTypeRef register_params[] = {pointer, pointer, pointer};
            LLVMTypeRef register_type = LLVMFunctionType(result,
                register_params, 3, false);
            LLVMValueRef register_image = LLVMAddFunction(module,
                "n00b_abi_v1_register_image", register_type);
            LLVMValueRef register_args[] = {descriptor,
                LLVMConstPointerNull(pointer), LLVMConstPointerNull(pointer)};
            LLVMValueRef status = LLVMBuildCall2(builder, register_type,
                register_image, register_args, 3, "registration");
            LLVMValueRef failed = LLVMBuildICmp(builder, LLVMIntNE, status,
                LLVMConstInt(result, 0, false), "registration.failed");
            LLVMBasicBlockRef rejected = LLVMAppendBasicBlockInContext(context,
                entry, "metadata.invalid");
            LLVMBasicBlockRef ready = LLVMAppendBasicBlockInContext(context,
                entry, "run.initializers");
            LLVMBuildCondBr(builder, failed, rejected, ready);
            LLVMPositionBuilderAtEnd(builder, rejected);
            LLVMValueRef trap = LLVMGetIntrinsicDeclaration(module,
                LLVMLookupIntrinsicID("llvm.trap", sizeof("llvm.trap") - 1),
                nullptr, 0);
            LLVMBuildCall2(builder, LLVMGlobalGetValueType(trap), trap,
                nullptr, 0, "");
            LLVMBuildUnreachable(builder);
            LLVMPositionBuilderAtEnd(builder, ready);
        }
        if (array_producer_count) {
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef array_fields[] = {pointer, pointer};
            LLVMTypeRef array_type = LLVMStructTypeInContext(context,
                array_fields, 2, false);
            LLVMTypeRef writer_parameters[] = {result, pointer, pointer,
                                                result};
            LLVMTypeRef writer_type = LLVMFunctionType(result,
                writer_parameters, 4, false);
            LLVMValueRef writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_array", writer_type);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMTypeRef string_fields[] = {i64, i64};
            LLVMTypeRef string_type = LLVMStructTypeInContext(context,
                string_fields, 2, false);
            LLVMTypeRef string_writer_parameters[] = {result, i64, i64};
            LLVMTypeRef string_writer_type = LLVMFunctionType(result,
                string_writer_parameters, 3, false);
            LLVMValueRef string_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_string", string_writer_type);
            LLVMTypeRef effect_writer_parameters[] = {result};
            LLVMTypeRef effect_writer_type = LLVMFunctionType(result,
                effect_writer_parameters, 1, false);
            LLVMValueRef effect_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_effect", effect_writer_type);
            for (size_t i = 0; i < array_producer_count; i++) {
                const n00bb_llvm_array_producer_t *spec = &array_producers[i];
                LLVMTypeRef expected = spec->kind == N00BB_LLVM_LITERAL_EFFECT
                    ? LLVMVoidTypeInContext(context)
                    : spec->kind == N00BB_LLVM_LITERAL_STRING
                        ? string_type : array_type;
                LLVMValueRef producer = spec->symbol
                    ? LLVMGetNamedFunction(module, spec->symbol) : nullptr;
                if (!producer && spec->symbol)
                    producer = LLVMAddFunction(module, spec->symbol,
                        LLVMFunctionType(expected, nullptr, 0, false));
                if (!producer || (spec->kind == N00BB_LLVM_LITERAL_ARRAY
                        && !spec->element_stride)
                    || LLVMCountParams(producer)
                    || LLVMGetReturnType(LLVMGlobalGetValueType(producer))
                        != expected) {
                    LLVMDisposeBuilder(builder);
                    return backend_error(diagnostics,
                        "compile-time literal producer signature is invalid");
                }
                LLVMValueRef value = LLVMBuildCall2(builder,
                    LLVMGlobalGetValueType(producer), producer,
                    nullptr, 0, spec->kind == N00BB_LLVM_LITERAL_EFFECT
                        ? "" : "literal.result");
                LLVMValueRef status = nullptr;
                if (spec->kind == N00BB_LLVM_LITERAL_EFFECT) {
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                    };
                    status = LLVMBuildCall2(builder, effect_writer_type,
                        effect_writer, arguments, 1, "literal.write");
                } else if (spec->kind == N00BB_LLVM_LITERAL_STRING) {
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        LLVMBuildExtractValue(builder, value, 0,
                                              "string.low"),
                        LLVMBuildExtractValue(builder, value, 1,
                                              "string.high"),
                    };
                    status = LLVMBuildCall2(builder, string_writer_type,
                        string_writer, arguments, 3, "literal.write");
                } else {
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        LLVMBuildExtractValue(builder, value, 0,
                                              "array.data"),
                        LLVMBuildExtractValue(builder, value, 1,
                                              "array.descriptor"),
                        LLVMConstInt(result, spec->element_stride, false),
                    };
                    status = LLVMBuildCall2(builder, writer_type,
                        writer, arguments, 4, "literal.write");
                }
                LLVMValueRef failed = LLVMBuildICmp(builder, LLVMIntNE,
                    status, LLVMConstNull(result), "literal.write.failed");
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(
                    context, entry, "literal.write.failure");
                LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(
                    context, entry, "literal.write.next");
                LLVMBuildCondBr(builder, failed, failure, next);
                LLVMPositionBuilderAtEnd(builder, failure);
                LLVMBuildRet(builder, LLVMConstInt(result, 1, false));
                LLVMPositionBuilderAtEnd(builder, next);
            }
        }
        for (size_t i = 0; i < initializer_count; i++) {
            LLVMValueRef initializer = LLVMGetNamedFunction(module,
                entry_initializers[i]);
            if (!initializer)
                initializer = LLVMAddFunction(module, entry_initializers[i],
                    initializer_type);
            LLVMTypeRef initializer_result = LLVMGetReturnType(
                LLVMGlobalGetValueType(initializer));
            bool failable = comptime
                && LLVMGetTypeKind(initializer_result)
                    == LLVMStructTypeKind
                && LLVMCountStructElementTypes(initializer_result) == 2;
            if (LLVMCountParams(initializer)
                || (!failable && initializer_result
                    != LLVMVoidTypeInContext(context))) {
                LLVMDisposeBuilder(builder);
                return backend_error(diagnostics,
                    "phase initializer signature is invalid");
            }
            LLVMValueRef outcome = LLVMBuildCall2(builder,
                LLVMGlobalGetValueType(initializer),
                initializer, nullptr, 0, "");
            if (failable) {
                LLVMValueRef tag = LLVMBuildExtractValue(builder, outcome,
                    1, "initializer.failure.tag");
                LLVMValueRef failed = LLVMBuildICmp(builder, LLVMIntNE, tag,
                    LLVMConstInt(LLVMTypeOf(tag), 0, false),
                    "initializer.failed");
                LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(
                    context, entry, "initializer.failure");
                LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(
                    context, entry, "initializer.next");
                LLVMBuildCondBr(builder, failed, failure, next);
                LLVMPositionBuilderAtEnd(builder, failure);
                LLVMBuildRet(builder, LLVMConstInt(result, 1, false));
                LLVMPositionBuilderAtEnd(builder, next);
            }
        }
        LLVMBuildRet(builder, LLVMConstInt(result, 0, false));
        LLVMDisposeBuilder(builder);
    }

    char *message = nullptr;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) {
        backend_error(diagnostics, message ? message : "invalid LLVM module");
        if (message) LLVMDisposeMessage(message);
        return false;
    }
    if (message) LLVMDisposeMessage(message);
    message = nullptr;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter())
        return backend_error(diagnostics, "LLVM native target is unavailable");
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = nullptr;
    bool ok = triple && !LLVMGetTargetFromTriple(triple, &target, &message);
    if (!ok) {
        backend_error(diagnostics, message ? message : "LLVM target is unavailable");
        if (message) LLVMDisposeMessage(message);
        if (triple) LLVMDisposeMessage(triple);
        return false;
    }
    if (message) LLVMDisposeMessage(message);
    message = nullptr;
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple,
        "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
        LLVMCodeModelDefault);
    LLVMSetTarget(module, triple);
    LLVMDisposeMessage(triple);
    if (!machine)
        return backend_error(diagnostics, "could not create LLVM target machine");
    LLVMTargetDataRef layout = LLVMCreateTargetDataLayout(machine);
    char *layout_text = LLVMCopyStringRepOfTargetData(layout);
    LLVMSetDataLayout(module, layout_text);
    LLVMDisposeMessage(layout_text);
    LLVMDisposeTargetData(layout);
    LLVMMemoryBufferRef object = nullptr;
    ok = !LLVMTargetMachineEmitToMemoryBuffer(machine, module,
        LLVMObjectFile, &message, &object);
    if (ok) core_buffer_append(out, LLVMGetBufferStart(object),
                               LLVMGetBufferSize(object));
    else backend_error(diagnostics, message ? message : "LLVM object emission failed");
    if (object) LLVMDisposeMemoryBuffer(object);
    if (message) LLVMDisposeMessage(message);
    LLVMDisposeTargetMachine(machine);
    return ok;
}
