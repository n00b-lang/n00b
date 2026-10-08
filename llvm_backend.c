#include "llvm_backend.h"

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
lower_instruction(lower_state_t *state, ir_instr_t *instruction)
{
    LLVMValueRef result = nullptr;
    ir_operand_t *operands = instruction->operands;
    uint32_t count = instruction->n_operands;
    switch (instruction->op) {
    case IR_OP_CONST: {
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
    case IR_OP_RETURN:
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
    case IR_OP_TRAP:
        if (count == 0) {
            emit_trap(state);
            return true;
        }
        break;
    default:
        break;
    }
    return unsupported(state, instruction);
}

static LLVMTypeRef
function_type(LLVMModuleRef target, ir_func_t *function)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = th_func_ret(function->type)
        ? scalar_type(function->owner, th_func_ret(function->type), context)
        : nullptr;
    th_type *error = th_func_error(function->type);
    if (!result || (error && ir_machine_of(function->owner, error).class
                         != IR_CLASS_VOID)
        || th_func_nkw(function->type) || th_func_rest(function->type))
        return nullptr;
    size_t count = th_func_nparams(function->type);
    if (count > UINT32_MAX) return nullptr;
    LLVMTypeRef *parameters = count
        ? calloc(count, sizeof(*parameters)) : nullptr;
    if (count && !parameters) return nullptr;
    for (size_t i = 0; i < count; i++) {
        const th_param *parameter = th_func_param(function->type, i);
        parameters[i] = parameter
            ? scalar_type(function->owner, parameter->type, context)
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
        .target_function = target_function, .diagnostics = diagnostics};
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
            LLVMTypeRef type = scalar_type(function->owner, local->type, context);
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
             core_diagnostics_t *diagnostics)
{
    if (ir_global_count(source))
        return backend_error(diagnostics,
            "LLVM backend cannot lower module globals yet");
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        LLVMTypeRef signature = function_type(target, function);
        if (!signature)
            return backend_error(diagnostics,
                "LLVM backend cannot lower this function signature yet");
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
            && !lower_function(target, function, diagnostics)) return false;
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

static bool
lower_phase(n00bb_llvm_backend_t *backend, bool comptime,
            core_diagnostics_t *diagnostics)
{
    bool *lowered = comptime ? &backend->comptime_lowered
                             : &backend->runtime_lowered;
    if (*lowered) return true;
    if (!lower_module(comptime ? backend->comptime : backend->runtime,
                      comptime ? backend->comptime_source
                               : backend->runtime_source, diagnostics))
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
                               size_t metadata_bytes,
                               core_buffer_t *out,
                               core_diagnostics_t *diagnostics)
{
    if (!backend || !out || (initializer_count && !entry_initializers)
        || (!comptime && initializer_count && !metadata_bytes))
        return backend_error(diagnostics, "invalid LLVM object arguments");
    if (!lower_phase(backend, comptime, diagnostics)) return false;
    LLVMModuleRef module = comptime ? backend->comptime : backend->runtime;
    LLVMContextRef context = backend->context;
    if (initializer_count) {
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
        for (size_t i = 0; i < initializer_count; i++) {
            LLVMValueRef initializer = LLVMGetNamedFunction(module,
                entry_initializers[i]);
            if (!initializer)
                initializer = LLVMAddFunction(module, entry_initializers[i],
                    initializer_type);
            if (LLVMCountParams(initializer)
                || LLVMGetReturnType(LLVMGlobalGetValueType(initializer))
                    != LLVMVoidTypeInContext(context)) {
                LLVMDisposeBuilder(builder);
                return backend_error(diagnostics,
                    "phase initializer signature is invalid");
            }
            LLVMBuildCall2(builder, LLVMGlobalGetValueType(initializer),
                initializer, nullptr, 0, "");
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
