#include "llvm_backend.h"

#include "ir/layout.h"
#include "ir/shape.h"
#include "ir/typelang.h"
#include "abi_string.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/list.h"
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
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    const n00bb_llvm_finalizer_binding_t *finalizers;
    size_t finalizer_count;
    ir_func_t *const *comptime_reachable;
    size_t comptime_reachable_count;
    bool comptime_reachable_bound;
    ir_func_t *const *runtime_reachable;
    size_t runtime_reachable_count;
    bool runtime_reachable_bound;
};

typedef struct {
    const ir_value_t *source;
    LLVMValueRef target;
} lowered_value_t;

typedef struct {
    const ir_instr_t *call;
    uint32_t ordinal;
    LLVMValueRef storage;
} lowered_call_output_t;

typedef struct {
    th_type *type;
    LLVMTypeRef native;
    bool matches;
} layout_match_t;
core_list_decl(layout_match_t);

typedef struct {
    th_type *type;
    const char *name;
    LLVMTypeRef native;
    unsigned index;
    bool found;
} member_match_t;
core_list_decl(member_match_t);

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    ir_func_t *function;
    LLVMValueRef target_function;
    LLVMBasicBlockRef *blocks;
    LLVMBasicBlockRef *block_tails;
    LLVMValueRef *locals;
    lowered_value_t *values;
    size_t value_count;
    size_t value_capacity;
    lowered_call_output_t *call_outputs;
    size_t call_output_count;
    size_t call_output_capacity;
    core_diagnostics_t *diagnostics;
    const n00bb_llvm_phase_array_t *phase_arrays;
    size_t phase_array_count;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    core_list_t(layout_match_t) *layout_matches;
    core_list_t(member_match_t) *member_matches;
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
    if (machine.class == IR_CLASS_FLOAT && machine.width == 64)
        return LLVMDoubleTypeInContext(context);
    if (machine.class == IR_CLASS_PTR)
        return LLVMPointerTypeInContext(context, 0);
    return nullptr;
}

static LLVMTypeRef
box_llvm_type(LLVMContextRef context)
{
    LLVMTypeRef fields[] = {
        LLVMInt16TypeInContext(context), LLVMInt16TypeInContext(context),
        LLVMInt32TypeInContext(context), LLVMInt64TypeInContext(context),
    };
    return LLVMStructTypeInContext(context, fields, 4, false);
}

static bool
is_pointer_option(th_type *type)
{
    type = th_resolve(type);
    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "option") || count != 1) return false;
    const th_arg *element = th_named_arg(type, 0);
    return element && element->kind == TH_ARG_TYPE
        && th_kind_of(th_resolve(element->type)) == TH_INDIRECTION;
}

static bool
is_atomic_bool(ir_module_t *module, th_type *type)
{
    const char *name = nullptr;
    size_t count = 0;
    type = th_resolve(type);
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "atomic") || count != 1) return false;
    const th_arg *element = th_named_arg(type, 0);
    return element && element->kind == TH_ARG_TYPE
        && ir_machine_of(module, element->type).class == IR_CLASS_BOOL;
}

static th_type *
atomic_element_type(th_type *type)
{
    const char *name = nullptr;
    size_t count = 0;
    type = th_resolve(type);
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "atomic") || count != 1) return nullptr;
    const th_arg *element = th_named_arg(type, 0);
    return element && element->kind == TH_ARG_TYPE
        ? element->type : nullptr;
}

static LLVMTypeRef value_type(ir_module_t *module, th_type *type,
                              LLVMContextRef context);

static LLVMTypeRef
inline_member_type(ir_module_t *module,
                   const ir_final_member_layout_t *member,
                   LLVMContextRef context)
{
    if (!member->inline_element_count || !member->inline_element_type
        || member->inline_element_count > UINT32_MAX
        || member->inline_element_stride_bits % 8) return nullptr;
    LLVMTypeRef element = value_type(module, member->inline_element_type,
                                    context);
    if (!element || LLVMGetTypeKind(element) == LLVMVoidTypeKind)
        return nullptr;
    return LLVMArrayType(element, (unsigned)member->inline_element_count);
}

static void
record_layout_order(const ir_final_layout_t *layout, size_t *order)
{
    for (size_t i = 0; i < layout->n_members; i++) order[i] = i;
    for (size_t i = 1; i < layout->n_members; i++) {
        size_t member = order[i], j = i;
        while (j && layout->members[order[j - 1]].bit_offset
                > layout->members[member].bit_offset) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = member;
    }
}

static LLVMTypeRef
aggregate_option_type(ir_module_t *module, th_type *type,
                      LLVMContextRef context)
{
    type = th_resolve(type);
    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "option") || count != 1
        || is_pointer_option(type)) return nullptr;
    const th_arg *argument = th_named_arg(type, 0);
    if (!argument || argument->kind != TH_ARG_TYPE) return nullptr;
    const char *target = ir_module_target(module);
    const ir_final_layout_t *option = ir_layout_final(module, type, target);
    const ir_final_layout_t *payload = ir_layout_final(module,
        argument->type, target);
    if (!option || !payload || !payload->alignment_bytes
        || payload->intrinsic_bits % (payload->alignment_bytes * 8)
        || option->alignment_bytes != payload->alignment_bytes
        || option->intrinsic_bits != payload->intrinsic_bits + 1
        || !option->has_discriminant || option->discriminant_is_niche
        || option->discriminant.unit_bit_offset != payload->intrinsic_bits
        || option->discriminant.unit_width_bits != 8
        || option->discriminant.value_width_bits != 1
        || option->discriminant.lsb) return nullptr;
    LLVMTypeRef element = value_type(module, argument->type, context);
    if (!element || LLVMGetTypeKind(element) == LLVMVoidTypeKind)
        return nullptr;
    LLVMTypeRef fields[] = {element, LLVMInt8TypeInContext(context)};
    return LLVMStructTypeInContext(context, fields, 2, false);
}

static LLVMTypeRef
value_type(ir_module_t *module, th_type *type, LLVMContextRef context)
{
    th_kind kind = th_kind_of(th_resolve(type));
    if (kind == TH_VAR || kind == TH_TRAIT) return box_llvm_type(context);
    th_type *definition = ir_type_definition(module, type);
    if (definition && th_kind_of(definition) == TH_BITINT) {
        const ir_final_layout_t *layout = ir_layout_final(module, type,
            ir_module_target(module));
        uint64_t bytes = 0, semantic_bits = 0;
        if (!layout || !ir_layout_final_byte_extent(layout, &bytes)
            || !bytes || bytes > 16
            || !th_bitint_info(definition, &semantic_bits, nullptr)
            || semantic_bits > bytes * 8) return nullptr;
        return LLVMIntTypeInContext(context, (unsigned)(bytes * 8));
    }
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
    if (!strcmp(name, "atomic") && arguments == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        return argument && argument->kind == TH_ARG_TYPE
            ? value_type(module, argument->type, context) : nullptr;
    }
    if (is_pointer_option(type))
        return LLVMPointerTypeInContext(context, 0);
    if (!strcmp(name, "option") && arguments == 1)
        return aggregate_option_type(module, type, context);
    if (!strcmp(name, TH_TUPLE) && arguments <= UINT32_MAX) {
        LLVMTypeRef *fields = arguments
            ? calloc(arguments, sizeof(*fields)) : nullptr;
        if (arguments && !fields) return nullptr;
        bool valid = true;
        for (size_t i = 0; valid && i < arguments; i++) {
            const th_arg *argument = th_named_arg(th_resolve(type), i);
            fields[i] = argument && argument->kind == TH_ARG_TYPE
                ? value_type(module, argument->type, context) : nullptr;
            valid = fields[i]
                && LLVMGetTypeKind(fields[i]) != LLVMVoidTypeKind;
        }
        LLVMTypeRef tuple = valid
            ? LLVMStructTypeInContext(context, fields, (unsigned)arguments,
                                      false) : nullptr;
        free(fields);
        return tuple;
    }
    if (!strcmp(name, "string") && !arguments) {
        LLVMTypeRef word = LLVMInt64TypeInContext(context);
        LLVMTypeRef fields[] = {word, word};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    if (definition && th_kind_of(definition) == TH_RECORD) {
        size_t count = th_record_len(definition);
        const ir_final_layout_t *layout = ir_layout_final(module, type,
            ir_module_target(module));
        if (!layout || count != layout->n_members || count > UINT32_MAX)
            return nullptr;
        LLVMTypeRef *fields = count
            ? calloc(count, sizeof(*fields)) : nullptr;
        size_t *order = count ? calloc(count, sizeof(*order)) : nullptr;
        if (count && (!fields || !order)) {
            free(fields);
            free(order);
            return nullptr;
        }
        record_layout_order(layout, order);
        bool valid = true;
        for (size_t i = 0; valid && i < count; i++) {
            const ir_final_member_layout_t *member =
                &layout->members[order[i]];
            th_type *field = member->name
                ? th_record_lookup(definition, member->name) : nullptr;
            fields[i] = member->inline_element_count
                ? inline_member_type(module, member, context)
                : field ? value_type(module, field, context) : nullptr;
            valid = fields[i]
                && LLVMGetTypeKind(fields[i]) != LLVMVoidTypeKind;
        }
        LLVMTypeRef record = valid
            ? LLVMStructTypeInContext(context, fields, (unsigned)count,
                                      false) : nullptr;
        free(order);
        free(fields);
        return record;
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

static LLVMTypeRef function_type(LLVMModuleRef target, ir_func_t *function);

static LLVMValueRef
integer_constant(ir_machine_t machine, LLVMTypeRef type, ir_lit_t literal)
{
    if (!type || LLVMGetTypeKind(type) != LLVMIntegerTypeKind
        || !machine.width || machine.width > 128
        || (machine.class != IR_CLASS_BOOL && machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT)) return nullptr;
    ir_uint_t maximum = machine.width == 128 ? ~(ir_uint_t)0
        : (((ir_uint_t)1 << machine.width) - 1);
    if (machine.class == IR_CLASS_SINT) {
        if (literal.mag > (literal.neg ? (maximum >> 1) + 1
                                       : maximum >> 1)) return nullptr;
    } else if (literal.neg || literal.mag > maximum) return nullptr;
    ir_uint_t bits = literal.neg ? (ir_uint_t)0 - literal.mag
                                 : literal.mag;
    if (machine.width <= 64)
        return LLVMConstInt(type, (uint64_t)bits, false);
    uint64_t words[] = {(uint64_t)bits, (uint64_t)(bits >> 64)};
    return LLVMConstIntOfArbitraryPrecision(type, 2, words);
}

static LLVMValueRef
global_ref(LLVMModuleRef module, const ir_global_t *global,
           const n00bb_llvm_global_binding_t *bindings, size_t count)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    const char *name = global->name;
    for (size_t i = 0; i < count; i++)
        if (bindings[i].global == identity) {
            name = bindings[i].native_name;
            break;
        }
    LLVMValueRef target = LLVMGetNamedGlobal(module, name);
    if (target) return target;
    LLVMTypeRef type = value_type(global->owner, global->type,
                                 LLVMGetModuleContext(module));
    return type ? LLVMAddGlobal(module, type, name) : nullptr;
}

static LLVMValueRef
lowered_value(lower_state_t *state, const ir_value_t *value)
{
    if (!value) return nullptr;
    if (value->kind == IR_VAL_PARAM && value->owner == state->function
        && value->index < state->function->n_params) {
        size_t input_index = 0, formal_index = 0;
        const th_param *formal = nullptr;
        for (; formal_index < th_func_nparams(state->function->type);
             formal_index++) {
            const th_param *candidate = th_func_param(state->function->type,
                                                        formal_index);
            if (!candidate || !th_mode_accepts_input(candidate->mode))
                continue;
            if (input_index++ == value->index) {
                formal = candidate;
                break;
            }
        }
        if (!formal || formal_index >= LLVMCountParams(state->target_function))
            return nullptr;
        LLVMValueRef parameter = LLVMGetParam(state->target_function,
                                               (unsigned)formal_index);
        if (!formal || !th_mode_produces_output(formal->mode))
            return parameter;
        LLVMTypeRef type = value_type(state->function->owner, value->type,
            LLVMGetModuleContext(state->module));
        return type ? LLVMBuildLoad2(state->builder, type, parameter,
                                     "copyin") : nullptr;
    }
    if (value->kind == IR_VAL_FUNC && value->owner) {
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "%s.%s",
            ir_module_name(value->owner->owner), value->owner->name);
        LLVMValueRef function = LLVMGetNamedFunction(state->module,
            symbol.data);
        if (!function && value->owner->owner != state->function->owner) {
            LLVMTypeRef signature = function_type(state->module,
                                                  value->owner);
            if (signature)
                function = LLVMAddFunction(state->module, symbol.data,
                                           signature);
        }
        core_buffer_release(&symbol);
        return function;
    }
    if (value->kind == IR_VAL_GLOBAL && value->global) {
        LLVMValueRef global = global_ref(state->module, value->global,
            state->global_bindings, state->global_binding_count);
        if (!global) return nullptr;
        return LLVMBuildLoad2(state->builder, LLVMGlobalGetValueType(global),
                              global, "global");
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

static bool store_gc_barrier(lower_state_t *state, LLVMValueRef value,
                             th_type *type);

static bool
save_result(lower_state_t *state, const ir_instr_t *instruction,
            LLVMValueRef value)
{
    if (instruction->dest_kind == IR_DEST_NONE) return true;
    if (!value) return false;
    if (instruction->dest_kind == IR_DEST_LOCAL) {
        for (uint32_t i = 0; i < state->function->n_locals; i++)
            if (state->function->locals[i] == instruction->dest_local) {
                if (!store_gc_barrier(state, value,
                        instruction->dest_local->type)) return false;
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

static bool
save_call_output(lower_state_t *state, const ir_instr_t *call,
                 uint32_t ordinal, LLVMValueRef storage)
{
    if (state->call_output_count == state->call_output_capacity) {
        size_t capacity = state->call_output_capacity
            ? state->call_output_capacity * 2 : 4;
        if (capacity < state->call_output_capacity
            || capacity > SIZE_MAX / sizeof(*state->call_outputs)) return false;
        lowered_call_output_t *items = realloc(state->call_outputs,
            capacity * sizeof(*items));
        if (!items) return false;
        state->call_outputs = items;
        state->call_output_capacity = capacity;
    }
    state->call_outputs[state->call_output_count++] =
        (lowered_call_output_t){call, ordinal, storage};
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

static void
emit_trap_if(lower_state_t *state, LLVMValueRef condition)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef function = LLVMGetBasicBlockParent(
        LLVMGetInsertBlock(state->builder));
    LLVMBasicBlockRef bad = LLVMAppendBasicBlockInContext(context,
        function, "arithmetic.trap");
    LLVMBasicBlockRef good = LLVMAppendBasicBlockInContext(context,
        function, "arithmetic.cont");
    LLVMBuildCondBr(state->builder, condition, bad, good);
    LLVMPositionBuilderAtEnd(state->builder, bad);
    emit_trap(state);
    LLVMPositionBuilderAtEnd(state->builder, good);
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

static bool
literal_boolean_field_matches(ir_module_t *module, th_type *type,
                              LLVMTypeRef native, LLVMTargetDataRef target)
{
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    uint64_t stride_bits = 0;
    return layout && native && target
        && ir_machine_of(module, type).class == IR_CLASS_BOOL
        && layout->intrinsic_bits == sizeof(uint8_t) * 8
        && layout->alignment_bytes == 1
        && ir_layout_final_stride_bits(layout, &stride_bits)
        && stride_bits == layout->intrinsic_bits
        && LLVMGetTypeKind(native) == LLVMIntegerTypeKind
        && LLVMGetIntTypeWidth(native) == 1
        && LLVMABISizeOfType(target, native) == sizeof(uint8_t)
        && LLVMABIAlignmentOfType(target, native) == 1;
}

static bool
literal_element_layout_matches(ir_module_t *module, th_type *type,
                               LLVMTypeRef native, LLVMTargetDataRef target)
{
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    uint64_t stride_bits = 0;
    if (!layout || !native || !target
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8
        || LLVMABISizeOfType(target, native) != stride_bits / 8
        || LLVMABIAlignmentOfType(target, native)
            != layout->alignment_bytes) return false;
    ir_machine_t machine = ir_machine_of(module, type);
    th_type *definition = ir_type_definition(module, type);
    if (definition && th_kind_of(definition) == TH_BITINT) {
        uint64_t semantic_bits = 0;
        return th_bitint_info(definition, &semantic_bits, nullptr)
            && semantic_bits && semantic_bits <= layout->intrinsic_bits
            && layout->intrinsic_bits <= 128
            && LLVMGetTypeKind(native) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(native) == layout->intrinsic_bits;
    }
    if (machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT) {
        const char *name = nullptr;
        bool padded_char = machine.class == IR_CLASS_UINT
            && th_named_info(th_resolve(type), &name, nullptr,
                             nullptr, nullptr)
            && name && !strcmp(name, "char")
            && layout->intrinsic_bits >= machine.width;
        if ((padded_char || (machine.width >= 8 && !(machine.width % 8)
                && layout->intrinsic_bits == machine.width))
            && LLVMGetTypeKind(native) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(native) == machine.width) return true;
    }
    if (machine.class == IR_CLASS_FLOAT && machine.width == 64)
        return LLVMGetTypeKind(native) == LLVMDoubleTypeKind
            && layout->intrinsic_bits == 64;
    if (machine.class == IR_CLASS_PTR)
        return LLVMGetTypeKind(native) == LLVMPointerTypeKind
            && layout->intrinsic_bits == sizeof(void *) * 8;

    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE)
        return layout->intrinsic_bits == sizeof(n00b_abi_array_value_v1) * 8
            && LLVMGetTypeKind(native) == LLVMStructTypeKind
            && LLVMCountStructElementTypes(native) == 2
            && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(native, 0))
                == LLVMPointerTypeKind
            && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(native, 1))
                == LLVMPointerTypeKind;

    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(th_resolve(type), &name, nullptr, nullptr, &count)
        || !name) return false;
    if (!strcmp(name, "atomic") && count == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        return argument && argument->kind == TH_ARG_TYPE
            && (literal_boolean_field_matches(module, argument->type,
                                              native, target)
                || literal_element_layout_matches(module, argument->type,
                                                  native, target));
    }
    if (is_pointer_option(type))
        return LLVMGetTypeKind(native) == LLVMPointerTypeKind
            && layout->intrinsic_bits == sizeof(void *) * 8
            && layout->has_discriminant
            && layout->discriminant_is_niche
            && layout->discriminant.unit_bit_offset == 0
            && layout->discriminant.unit_width_bits == sizeof(void *) * 8;
    if (!strcmp(name, "option") && count == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        LLVMTypeRef tag = LLVMGetTypeKind(native) == LLVMStructTypeKind
            && LLVMCountStructElementTypes(native) == 2
            ? LLVMStructGetTypeAtIndex(native, 1) : nullptr;
        return argument && argument->kind == TH_ARG_TYPE
            && native == aggregate_option_type(module, type,
                LLVMGetTypeContext(native))
            && LLVMOffsetOfElement(target, native, 0) == 0
            && LLVMOffsetOfElement(target, native, 1) * 8
                == layout->discriminant.unit_bit_offset
            && tag && LLVMGetTypeKind(tag) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(tag) == 8
            && literal_element_layout_matches(module, argument->type,
                LLVMStructGetTypeAtIndex(native, 0), target);
    }
    if (!strcmp(name, "string") && !count)
        return layout->intrinsic_bits == 128
            && LLVMGetTypeKind(native) == LLVMStructTypeKind
            && LLVMCountStructElementTypes(native) == 2
            && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(native, 0))
                == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(native, 0)) == 64
            && LLVMGetTypeKind(LLVMStructGetTypeAtIndex(native, 1))
                == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(native, 1)) == 64;
    if (definition && th_kind_of(definition) == TH_RECORD) {
        size_t fields = th_record_len(definition);
        if (LLVMGetTypeKind(native) != LLVMStructTypeKind
            || fields != layout->n_members
            || fields != LLVMCountStructElementTypes(native)) return false;
        size_t *order = fields ? calloc(fields, sizeof(*order)) : nullptr;
        if (fields && !order) return false;
        record_layout_order(layout, order);
        bool valid = true;
        for (size_t i = 0; valid && i < fields; i++) {
            const ir_final_member_layout_t *member =
                &layout->members[order[i]];
            th_type *field = member->name
                ? th_record_lookup(definition, member->name) : nullptr;
            LLVMTypeRef child = LLVMStructGetTypeAtIndex(native, (unsigned)i);
            uint64_t offset = LLVMOffsetOfElement(target, native,
                                                   (unsigned)i);
            bool inline_match = member->inline_element_count
                && child == inline_member_type(module, member,
                    LLVMGetTypeContext(native))
                && LLVMABISizeOfType(target, LLVMGetElementType(child))
                    == member->inline_element_stride_bits / 8
                && literal_element_layout_matches(module,
                    member->inline_element_type,
                    LLVMGetElementType(child), target);
            if (!field || offset > UINT64_MAX / 8
                || member->bit_offset != offset * 8
                || (!inline_match
                    && !literal_boolean_field_matches(module, field,
                        child, target)
                    && !literal_element_layout_matches(module, field,
                        child, target))) valid = false;
        }
        free(order);
        return valid;
    }
    if (strcmp(name, TH_TUPLE)
        || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != count
        || layout->n_members != count) return false;
    for (size_t i = 0; i < count; i++) {
        const th_arg *argument = th_named_arg(th_resolve(type), i);
        uint64_t offset = LLVMOffsetOfElement(target, native, (unsigned)i);
        if (!argument || argument->kind != TH_ARG_TYPE
            || offset > UINT64_MAX / 8
            || layout->members[i].bit_offset != offset * 8
            || (!literal_boolean_field_matches(module, argument->type,
                LLVMStructGetTypeAtIndex(native, (unsigned)i), target)
                && !literal_element_layout_matches(module, argument->type,
                    LLVMStructGetTypeAtIndex(native, (unsigned)i), target)))
            return false;
    }
    return true;
}

static bool
cached_literal_element_layout_matches(lower_state_t *state, th_type *type,
                                      LLVMTypeRef native)
{
    core_list_t(layout_match_t) *cache = state->layout_matches;
    for (size_t i = 0; cache && i < cache->len; i++)
        if (cache->data[i].type == type && cache->data[i].native == native)
            return cache->data[i].matches;
    bool matches = literal_element_layout_matches(state->function->owner,
        type, native, LLVMGetModuleDataLayout(state->module));
    if (cache) core_list_push(*cache, ((layout_match_t){type, native,
                                                       matches}));
    return matches;
}

static LLVMValueRef
tuple_aggregate(lower_state_t *state, ir_instr_t *instruction)
{
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    const char *name = nullptr;
    size_t count = 0;
    if (!type || !th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, TH_TUPLE)
        || count != instruction->n_operands) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(state->function->owner, type, context);
    if (!cached_literal_element_layout_matches(state, type, native))
        return nullptr;
    LLVMValueRef *fields = count ? calloc(count, sizeof(*fields)) : nullptr;
    if (count && !fields) return nullptr;
    bool constant = true;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        ir_operand_t *operand = &instruction->operands[i];
        fields[i] = operand->kind == IR_OPD_VALUE
            ? lowered_value(state, operand->value) : nullptr;
        valid = fields[i] && LLVMTypeOf(fields[i])
            == LLVMStructGetTypeAtIndex(native, (unsigned)i);
        constant = constant && valid && LLVMIsConstant(fields[i]);
    }
    LLVMValueRef result = nullptr;
    if (valid && constant)
        result = LLVMConstStructInContext(context, fields, (unsigned)count,
                                          false);
    else if (valid) {
        result = LLVMGetUndef(native);
        for (size_t i = 0; i < count; i++)
            result = LLVMBuildInsertValue(state->builder, result, fields[i],
                (unsigned)i, "tuple.field");
    }
    free(fields);
    return result;
}

static bool
record_member_index(lower_state_t *state, th_type *record_type,
                    const char *name, LLVMTypeRef *native_out,
                    unsigned *index_out)
{
    core_list_t(member_match_t) *cache = state->member_matches;
    for (size_t i = 0; cache && i < cache->len; i++) {
        const member_match_t *match = &cache->data[i];
        if (match->type != record_type || strcmp(match->name, name)) continue;
        if (match->found) {
            *native_out = match->native;
            *index_out = match->index;
        }
        return match->found;
    }
    ir_module_t *module = state->function->owner;
    th_type *definition = ir_type_definition(module, record_type);
    const ir_final_layout_t *layout = ir_layout_final(module, record_type,
        ir_module_target(module));
    LLVMTypeRef native = value_type(module, record_type,
        LLVMGetModuleContext(state->module));
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    bool valid = definition && th_kind_of(definition) == TH_RECORD
        && layout && native && target
        && cached_literal_element_layout_matches(state, record_type, native);
    if (valid) for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *field = &layout->members[i];
        if (!field->name || strcmp(field->name, name)) continue;
        size_t physical_index = 0;
        for (size_t j = 0; j < layout->n_members; j++)
            if (layout->members[j].bit_offset < field->bit_offset
                || (layout->members[j].bit_offset == field->bit_offset
                    && j < i)) physical_index++;
        *native_out = native;
        *index_out = (unsigned)physical_index;
        if (cache) core_list_push(*cache, ((member_match_t){
            record_type, name, native, (unsigned)physical_index, true}));
        return true;
    }
    if (cache) core_list_push(*cache, ((member_match_t){
        record_type, name, nullptr, 0, false}));
    return false;
}

static LLVMTypeRef
inline_index_base_type(lower_state_t *state, const ir_value_t *base,
                       uint64_t *count_out)
{
    const ir_instr_t *field = base ? base->def : nullptr;
    if (!field || field->op != IR_OP_FIELD || field->n_operands != 2
        || field->operands[0].kind != IR_OPD_VALUE
        || field->operands[1].kind != IR_OPD_NAME
        || !field->operands[0].value || !field->operands[1].name)
        return nullptr;
    ir_module_t *module = state->function->owner;
    th_type *record = th_ind_referent(field->operands[0].value->type);
    const ir_final_layout_t *layout = record
        ? ir_layout_final(module, record, ir_module_target(module)) : nullptr;
    if (!layout) return nullptr;
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        if (!member->name || strcmp(member->name, field->operands[1].name))
            continue;
        LLVMTypeRef native = inline_member_type(module, member,
            LLVMGetModuleContext(state->module));
        if (native) *count_out = member->inline_element_count;
        return native;
    }
    return nullptr;
}

static bool
array_requires_flat_storage(const ir_instr_t *instruction)
{
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    ir_shape_t *shape = instruction->dest
        ? ir_value_shape(instruction->dest) : nullptr;
    ir_shape_extent_t extent = {0};
    return element && th_kind_of(th_resolve(element)) == TH_SEQUENCE
        && shape && ir_shape_rank(shape) > 1
        && ir_shape_extent(shape, 1, &extent)
        && extent.kind == IR_SHAPE_EXTENT_STATIC;
}

static LLVMValueRef
constant_array(lower_state_t *state, ir_instr_t *instruction)
{
    ir_aggregate_form_t form;
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = element
        ? value_type(state->function->owner, element, context) : nullptr;
    LLVMTypeRef array_type = type
        ? value_type(state->function->owner, type, context) : nullptr;
    uint32_t count = instruction->n_operands;
    if (array_requires_flat_storage(instruction)
        || (ir_instr_aggregate_form(instruction, &form)
            && form != IR_AGGREGATE_ELEMENTS)
        || !element_type || !array_type
        || !literal_element_layout_matches(state->function->owner, element,
            element_type, LLVMGetModuleDataLayout(state->module)))
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
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = element
        ? value_type(state->function->owner, element, context) : nullptr;
    uint64_t stride_bits = 0;
    if (!identifier || !element || !layout || !layout->alignment_bytes
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || !literal_element_layout_matches(state->function->owner, element,
            element_type, LLVMGetModuleDataLayout(state->module)))
        return nullptr;
    uint64_t alignment = layout->alignment_bytes;
    uint64_t stride = stride_bits / 8;
    if (!stride || stride > UINT32_MAX) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *array = &state->phase_arrays[i];
        if (array->kind != N00BB_LLVM_LITERAL_ARRAY
            || !array->identifier || strcmp(identifier, array->identifier))
            continue;
        if (array->element_stride != stride || array->count > UINT32_MAX
            || array->count > UINT32_MAX / stride || !array->bytes)
            return nullptr;
        unsigned byte_count = (unsigned)(array->count * stride);
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
    if (!element || array_requires_flat_storage(instruction)
        || (ir_instr_aggregate_form(instruction, &form)
                     && form != IR_AGGREGATE_ELEMENTS)) return nullptr;
    ir_module_t *source = state->function->owner;
    const ir_final_layout_t *layout = ir_layout_final(source, element,
        ir_module_target(source));
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = value_type(source, element, context);
    LLVMTypeRef result_type = value_type(source, type, context);
    uint64_t stride_bits = 0;
    if (!layout || !layout->alignment_bytes || !element_type || !result_type
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || !literal_element_layout_matches(source, element, element_type,
            LLVMGetModuleDataLayout(state->module))) return nullptr;
    uint64_t element_align = layout->alignment_bytes;
    uint64_t stride = stride_bits / 8;
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
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        LLVMConstInt(i32, type_index, false),
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
        if (!store_gc_barrier(state, item, element)) return nullptr;
        LLVMBuildStore(state->builder, item, slot);
    }
    LLVMValueRef array = LLVMGetUndef(result_type);
    array = LLVMBuildInsertValue(state->builder, array, data, 0,
                                 "array.with.data");
    return LLVMBuildInsertValue(state->builder, array, storage, 1,
                                "array.with.descriptor");
}

static LLVMValueRef
string_value(LLVMModuleRef module, const uint8_t *bytes, size_t length,
             uint64_t encoding)
{
    LLVMContextRef context = LLVMGetModuleContext(module);
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
        LLVMValueRef data = LLVMAddGlobal(module,
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

static uint64_t
default_string_encoding(const char *bytes, size_t length)
{
    for (size_t i = 0; i < length; i++)
        if ((unsigned char)bytes[i] & 0x80)
            return N00B_ABI_STRING_UTF8;
    return N00B_ABI_STRING_ASCII;
}

static LLVMValueRef
constant_string(lower_state_t *state, ir_instr_t *instruction)
{
    size_t length = 0;
    const uint8_t *bytes = ir_instr_const_bytes(instruction, &length);
    if (!instruction->dest || !is_string_type(instruction->dest->type)
        || ir_annot_get(&instruction->annots, "n00bb.rich.metadata"))
        return nullptr;
    ir_annot_t *modifier = ir_annot_get(&instruction->annots,
                                        "n00bb.literal.modifier");
    uint64_t encoding = modifier && modifier->kind == IR_ANNOT_STR
        && modifier->s && !strcmp(modifier->s, "binary")
        ? N00B_ABI_STRING_BINARY
        : default_string_encoding((const char *)bytes, length);
    return string_value(state->module, bytes, length, encoding);
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
            return string_value(state->module, value->bytes, value->count,
                                value->string_encoding);
    }
    return nullptr;
}

static LLVMValueRef
cached_reloc_element(lower_state_t *state,
                     const n00bb_llvm_phase_array_t *cached,
                     size_t *position)
{
    if (!cached || !position || *position > cached->byte_len
        || cached->element_stride > cached->byte_len - *position
        || !cached->string_offsets || !cached->string_offset_count
        || cached->string_offset_count > (UINT32_MAX - 1) / 2)
        return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    const uint8_t *raw = cached->bytes + *position;
    *position += cached->element_stride;
    LLVMValueRef *fields = calloc(2 * cached->string_offset_count + 1,
                                   sizeof(*fields));
    if (!fields) return nullptr;
    unsigned n_fields = 0;
    uint32_t cursor = 0;
    bool valid = true;
    for (size_t f = 0; valid && f < cached->string_offset_count; f++) {
        uint32_t at = cached->string_offsets[f];
        if (at < cursor || at > cached->element_stride
            || sizeof(n00b_abi_string_v1)
                > cached->element_stride - at
            || cached->byte_len - *position
                < sizeof(uint64_t) + sizeof(uint8_t)) {
            valid = false;
            break;
        }
        if (at > cursor)
            fields[n_fields++] = LLVMConstStringInContext(context,
                (const char *)raw + cursor, at - cursor, true);
        uint64_t length;
        memcpy(&length, cached->bytes + *position, sizeof(length));
        *position += sizeof(length);
        uint8_t encoding = cached->bytes[(*position)++];
        valid = encoding <= N00B_ABI_STRING_UTF8
            && length <= cached->byte_len - *position;
        if (!valid) break;
        fields[n_fields] = string_value(state->module, cached->bytes + *position,
                                        (size_t)length, encoding);
        valid = fields[n_fields] != nullptr;
        n_fields++;
        *position += (size_t)length;
        cursor = at + sizeof(n00b_abi_string_v1);
    }
    if (valid && cursor < cached->element_stride)
        fields[n_fields++] = LLVMConstStringInContext(context,
            (const char *)raw + cursor, cached->element_stride - cursor,
            true);
    LLVMValueRef value = valid
        ? LLVMConstStructInContext(context, fields, n_fields, true) : nullptr;
    free(fields);
    return value && LLVMABISizeOfType(
        LLVMGetModuleDataLayout(state->module), LLVMTypeOf(value))
        == cached->element_stride ? value : nullptr;
}

static LLVMValueRef
materialized_reloc_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    LLVMTypeRef native = element
        ? value_type(state->function->owner, element, context) : nullptr;
    const ir_final_layout_t *layout = element
        ? ir_layout_final(state->function->owner, element,
            ir_module_target(state->function->owner)) : nullptr;
    if (!identifier || !native || !layout || !layout->alignment_bytes
        || !literal_element_layout_matches(state->function->owner,
            element, native, target)) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (cached->kind != N00BB_LLVM_LITERAL_RELOC
            || !cached->identifier
            || strcmp(identifier, cached->identifier)
            || !cached->bytes || !cached->element_stride
            || !cached->string_offsets || !cached->string_offset_count
            || cached->string_offset_count > (UINT32_MAX - 1) / 2
            || cached->count > UINT32_MAX
            || LLVMABISizeOfType(target, native)
                != cached->element_stride) continue;
        unsigned count = (unsigned)cached->count;
        LLVMValueRef *items = calloc(count ? count : 1, sizeof(*items));
        if (!items) return nullptr;
        size_t position = 0;
        bool valid = true;
        for (unsigned j = 0; valid && j < count; j++)
            valid = (items[j] = cached_reloc_element(state, cached,
                                                     &position)) != nullptr;
        valid = valid && position == cached->byte_len;
        if (!valid) {
            free(items);
            return nullptr;
        }
        LLVMTypeRef storage_type = count ? LLVMTypeOf(items[0]) : native;
        if (!count) items[0] = LLVMConstNull(native);
        LLVMValueRef values = LLVMConstArray(storage_type, items,
                                              count ? count : 1);
        free(items);
        LLVMValueRef data = LLVMAddGlobal(state->module,
            LLVMTypeOf(values), "n00bb.literal.reloc.array.cached");
        LLVMSetInitializer(data, values);
        LLVMSetAlignment(data, (unsigned)layout->alignment_bytes);
        LLVMSetGlobalConstant(data, true);
        LLVMSetLinkage(data, LLVMPrivateLinkage);
        return array_value_with_descriptor(state, data, count);
    }
    return nullptr;
}

static LLVMValueRef
materialized_reloc_value(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    if (!identifier || !type || th_kind_of(type) == TH_SEQUENCE)
        return nullptr;
    ir_module_t *source = state->function->owner;
    const ir_final_layout_t *layout = ir_layout_final(source, type,
        ir_module_target(source));
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(source, type, context);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    if (!layout || !layout->alignment_bytes || !native
        || !literal_element_layout_matches(source, type, native, target))
        return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (cached->kind != N00BB_LLVM_LITERAL_RELOC
            || !cached->identifier || strcmp(identifier, cached->identifier)
            || !cached->bytes || cached->count != 1
            || cached->element_stride != LLVMABISizeOfType(target, native))
            continue;
        size_t position = 0;
        LLVMValueRef value = cached_reloc_element(state, cached, &position);
        if (!value || position != cached->byte_len) return nullptr;
        LLVMValueRef storage = LLVMAddGlobal(state->module,
            LLVMTypeOf(value), "n00bb.literal.reloc.value.cached");
        LLVMSetInitializer(storage, value);
        LLVMSetAlignment(storage, layout->alignment_bytes);
        LLVMSetGlobalConstant(storage, true);
        LLVMSetLinkage(storage, LLVMPrivateLinkage);
        return LLVMBuildLoad2(state->builder, native, storage,
                              "literal.cached");
    }
    return nullptr;
}

static LLVMValueRef
materialized_nested_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *inner = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    th_type *element = inner && th_kind_of(th_resolve(inner)) == TH_SEQUENCE
        ? th_seq_elem(inner) : nullptr;
    ir_module_t *source = state->function->owner;
    const char *target_name = ir_module_target(source);
    const ir_final_layout_t *outer_layout = inner
        ? ir_layout_final(source, inner, target_name) : nullptr;
    const ir_final_layout_t *inner_layout = element
        ? ir_layout_final(source, element, target_name) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    LLVMTypeRef outer_native = inner ? value_type(source, inner, context)
                                     : nullptr;
    LLVMTypeRef inner_native = element
        ? value_type(source, element, context) : nullptr;
    uint64_t inner_stride_bits = 0;
    if (!identifier || !outer_layout || !outer_layout->alignment_bytes
        || !inner_layout || !inner_layout->alignment_bytes
        || !literal_element_layout_matches(source, inner, outer_native,
                                           target)
        || !literal_element_layout_matches(source, element, inner_native,
                                           target)
        || !ir_layout_final_stride_bits(inner_layout, &inner_stride_bits)
        || inner_stride_bits % 8 || !inner_stride_bits
        || inner_stride_bits / 8 > UINT32_MAX) return nullptr;
    uint32_t inner_stride = (uint32_t)(inner_stride_bits / 8);
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        bool reloc = cached->kind == N00BB_LLVM_LITERAL_NESTED_RELOC;
        if ((cached->kind != N00BB_LLVM_LITERAL_NESTED_ARRAY && !reloc)
            || !cached->identifier || strcmp(identifier, cached->identifier)
            || !cached->bytes || cached->count > UINT32_MAX
            || cached->element_stride != sizeof(n00b_abi_array_value_v1)
            || cached->inner_stride != inner_stride
            || (reloc && (!cached->string_offsets
                || !cached->string_offset_count))) continue;
        unsigned count = (unsigned)cached->count;
        LLVMValueRef *items = calloc(count ? count : 1, sizeof(*items));
        if (!items) return nullptr;
        size_t position = 0;
        bool valid = true;
        for (unsigned j = 0; valid && j < count; j++) {
            uint64_t child_count;
            if (position > cached->byte_len
                || cached->byte_len - position < sizeof(child_count)) {
                valid = false;
                break;
            }
            memcpy(&child_count, cached->bytes + position,
                   sizeof(child_count));
            position += sizeof(child_count);
            if (child_count > UINT32_MAX / inner_stride
                || child_count * inner_stride
                    > cached->byte_len - position) {
                valid = false;
                break;
            }
            unsigned byte_count = (unsigned)(child_count * inner_stride);
            LLVMValueRef contents = nullptr;
            if (reloc) {
                n00bb_llvm_phase_array_t inner = *cached;
                inner.element_stride = inner_stride;
                LLVMValueRef *elements = calloc(child_count
                    ? (size_t)child_count : 1, sizeof(*elements));
                valid = elements != nullptr;
                for (uint64_t k = 0; valid && k < child_count; k++)
                    valid = (elements[k] = cached_reloc_element(state,
                        &inner, &position)) != nullptr;
                if (valid && !child_count)
                    elements[0] = LLVMConstNull(inner_native);
                if (valid) contents = LLVMConstArray(child_count
                    ? LLVMTypeOf(elements[0]) : inner_native,
                    elements, child_count ? (unsigned)child_count : 1);
                free(elements);
            }
            else {
                contents = LLVMConstStringInContext(context,
                    byte_count ? (const char *)cached->bytes + position : "\0",
                    byte_count ? byte_count : 1, true);
                position += byte_count;
            }
            if (!valid || !contents) break;
            LLVMValueRef data = LLVMAddGlobal(state->module,
                LLVMTypeOf(contents), "n00bb.literal.nested.data.cached");
            LLVMSetInitializer(data, contents);
            LLVMSetAlignment(data, inner_layout->alignment_bytes);
            LLVMSetGlobalConstant(data, true);
            LLVMSetLinkage(data, LLVMPrivateLinkage);
            items[j] = array_value_with_descriptor(state, data, child_count);
            valid = items[j] && LLVMTypeOf(items[j]) == outer_native;
        }
        valid = valid && position == cached->byte_len;
        if (valid && !count) items[0] = LLVMConstNull(outer_native);
        LLVMValueRef values = valid
            ? LLVMConstArray(outer_native, items, count ? count : 1)
            : nullptr;
        free(items);
        if (!values) return nullptr;
        LLVMValueRef data = LLVMAddGlobal(state->module,
            LLVMTypeOf(values), "n00bb.literal.nested.array.cached");
        LLVMSetInitializer(data, values);
        LLVMSetAlignment(data, outer_layout->alignment_bytes);
        LLVMSetGlobalConstant(data, true);
        LLVMSetLinkage(data, LLVMPrivateLinkage);
        return array_value_with_descriptor(state, data, count);
    }
    return nullptr;
}

static LLVMValueRef
materialized_raw_value(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    const ir_final_layout_t *layout = type
        ? ir_layout_final(state->function->owner, type,
            ir_module_target(state->function->owner)) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef value = type
        ? value_type(state->function->owner, type, context) : nullptr;
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    bool bool_value = type && value
        && ir_machine_of(state->function->owner, type).class == IR_CLASS_BOOL
        && LLVMGetTypeKind(value) == LLVMIntegerTypeKind
        && LLVMGetIntTypeWidth(value) == 1;
    if (!identifier || !layout || !layout->alignment_bytes || !value
        || (!bool_value && !literal_element_layout_matches(
            state->function->owner, type, value, target))) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (cached->kind != N00BB_LLVM_LITERAL_RAW_VALUE
            || !cached->identifier
            || strcmp(identifier, cached->identifier)
            || cached->count != 1 || !cached->bytes
            || LLVMABISizeOfType(target, value) != cached->element_stride)
            continue;
        LLVMValueRef bytes = LLVMConstStringInContext(context,
            (const char *)cached->bytes, cached->element_stride, true);
        LLVMValueRef storage = LLVMAddGlobal(state->module,
            LLVMTypeOf(bytes), "n00bb.literal.value.cached");
        LLVMSetInitializer(storage, bytes);
        LLVMSetAlignment(storage, (unsigned)layout->alignment_bytes);
        LLVMSetGlobalConstant(storage, true);
        LLVMSetLinkage(storage, LLVMPrivateLinkage);
        return LLVMBuildLoad2(state->builder, value, storage,
                              "literal.cached");
    }
    return nullptr;
}

static LLVMValueRef
phase_cache_global(LLVMModuleRef module, const char *identifier,
                   LLVMTypeRef type, bool define)
{
    if (!identifier || !type) return nullptr;
    uint64_t hash = core_xxh3_64bits_raw(identifier, strlen(identifier));
    core_buffer_t name = {0};
    core_buffer_printf(&name, "__n00bb_comptime_literal_%016llx",
                       (unsigned long long)hash);
    LLVMValueRef global = name.data
        ? LLVMGetNamedGlobal(module, name.data) : nullptr;
    if (!global && name.data)
        global = LLVMAddGlobal(module, type, name.data);
    core_buffer_release(&name);
    if (!global || LLVMGlobalGetValueType(global) != type)
        return nullptr;
    LLVMSetVisibility(global, LLVMHiddenVisibility);
    if (define && !LLVMGetInitializer(global))
        LLVMSetInitializer(global, LLVMConstNull(type));
    return global;
}

typedef struct noescape_probe_t {
    const ir_func_t *function;
    const ir_value_t *address;
    const struct noescape_probe_t *previous;
} noescape_probe_t;

static bool address_has_only_local_uses(const ir_func_t *function,
                                        const ir_value_t *address,
                                        const noescape_probe_t *previous);

static bool
call_argument_is_noescape(const ir_instr_t *call, uint32_t operand_index,
                          const noescape_probe_t *previous)
{
    if (call->op != IR_OP_CALL || !operand_index
        || operand_index >= call->n_operands
        || call->operands[0].kind != IR_OPD_VALUE
        || !call->operands[0].value
        || call->operands[0].value->kind != IR_VAL_FUNC)
        return false;
    ir_func_t *callee = call->operands[0].value->owner;
    if (!callee || operand_index > callee->n_params)
        return false;
    for (uint32_t i = 1; i <= operand_index; i++)
        if (call->operands[i].kind != IR_OPD_VALUE)
            return false;
    ir_value_t *parameter = callee->params[operand_index - 1];
    ir_annot_t *escapes = parameter
        ? ir_annot_get(&parameter->annots, "ir.lifetime_escapes") : nullptr;
    ir_annot_t *resolved = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_unresolved") : nullptr;
    ir_annot_t *complete = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_analysis_complete") : nullptr;
    ir_annot_t *aliases = parameter
        ? ir_annot_get(&parameter->annots, "ir.alias_count") : nullptr;
    ir_annot_t *aliased_by = parameter
        ? ir_annot_get(&parameter->annots, "ir.aliased_by_count") : nullptr;
    bool no_aliases = aliases && aliases->kind == IR_ANNOT_INT
        && !aliases->i.neg && !aliases->i.mag && aliased_by
        && aliased_by->kind == IR_ANNOT_INT && !aliased_by->i.neg
        && !aliased_by->i.mag;
    bool summarized = escapes && escapes->kind == IR_ANNOT_BOOL && !escapes->b
        && resolved && resolved->kind == IR_ANNOT_BOOL && !resolved->b
        && ((complete && complete->kind == IR_ANNOT_BOOL && complete->b)
            || no_aliases);
    return summarized || (callee->n_blocks
        && address_has_only_local_uses(callee, parameter, previous));
}

static bool
address_has_only_local_uses(const ir_func_t *function,
                            const ir_value_t *address,
                            const noescape_probe_t *previous)
{
    if (!function || !address) return false;
    for (const noescape_probe_t *at = previous; at; at = at->previous)
        if (at->function == function && at->address == address)
            return false;
    const noescape_probe_t probe = {
        .function = function, .address = address, .previous = previous,
    };
    size_t count = 1;
    size_t capacity = 4;
    const ir_value_t **addresses = malloc(capacity * sizeof(*addresses));
    if (!addresses) return false;
    addresses[0] = address;
    bool local = true;
    for (size_t cursor = 0; local && cursor < count; cursor++) {
        for (uint32_t i = 0; local && i < function->n_blocks; i++) {
            const ir_block_t *block = function->blocks[i];
            for (uint32_t j = 0; local && j < block->n_instrs; j++) {
                const ir_instr_t *use = block->instrs[j];
                for (uint32_t k = 0; local && k < use->n_operands; k++) {
                    const ir_operand_t *operand = &use->operands[k];
                    const ir_value_t *referenced = nullptr;
                    switch (operand->kind) {
                    case IR_OPD_VALUE:
                    case IR_OPD_SPREAD:
                    case IR_OPD_ARG:
                    case IR_OPD_OUT:
                    case IR_OPD_FRESH:
                        referenced = operand->value;
                        break;
                    case IR_OPD_PHI_ARM:
                        referenced = operand->phi.value;
                        break;
                    case IR_OPD_NAMED_ARG:
                        referenced = operand->named.value;
                        break;
                    default:
                        break;
                    }
                    if (referenced != addresses[cursor]) continue;
                    if (operand->kind == IR_OPD_VALUE
                        && call_argument_is_noescape(use, k, &probe))
                        continue;
                    if (k != 0 || operand->kind != IR_OPD_VALUE) {
                        local = false;
                        break;
                    }
                    if (use->op == IR_OP_LOAD || use->op == IR_OP_STORE)
                        continue;
                    bool projection = use->op == IR_OP_FIELD
                        || use->op == IR_OP_INDEX
                        || use->op == IR_OP_OFFSET
                        || use->op == IR_OP_BITCAST
                        || (use->op == IR_OP_COPY && !use->dest_local);
                    if (!projection
                        || !use->dest || !use->dest->type
                        || th_kind_of(use->dest->type) != TH_INDIRECTION) {
                        local = false;
                        break;
                    }
                    bool seen = false;
                    for (size_t n = 0; n < count; n++)
                        if (addresses[n] == use->dest) seen = true;
                    if (seen) continue;
                    if (count == capacity) {
                        size_t next = capacity * 2;
                        const ir_value_t **grown = realloc(addresses,
                            next * sizeof(*addresses));
                        if (!grown) { local = false; break; }
                        addresses = grown;
                        capacity = next;
                    }
                    addresses[count++] = use->dest;
                }
            }
        }
    }
    free(addresses);
    return local;
}

static bool
allocation_has_only_local_uses(const ir_func_t *function,
                               const ir_instr_t *allocation)
{
    bool required = false;
    if (!allocation->dest
        || ir_instr_storage_class(allocation, &required) || required)
        return false;
    return address_has_only_local_uses(function, allocation->dest, nullptr);
}

static LLVMValueRef
entry_alloca(lower_state_t *state, LLVMTypeRef type)
{
    LLVMBasicBlockRef current = LLVMGetInsertBlock(state->builder);
    LLVMBasicBlockRef entry = LLVMGetFirstBasicBlock(state->target_function);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    if (first) LLVMPositionBuilderBefore(state->builder, first);
    else LLVMPositionBuilderAtEnd(state->builder, entry);
    LLVMValueRef slot = LLVMBuildAlloca(state->builder, type, "storage");
    LLVMPositionBuilderAtEnd(state->builder, current);
    return slot;
}

static uint32_t
runtime_type_index(lower_state_t *state, th_type *type)
{
    uint64_t hash = th_hash(ir_module_types(state->function->owner), type);
    for (size_t i = 0; i < state->type_index_count; i++)
        if (state->type_indices[i].typehash == hash)
            return state->type_indices[i].index;
    return 0;
}

static bool
store_gc_barrier(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    if (ir_func_execution_phase(state->function) == IR_PHASE_COMPTIME)
        return true;
    if (!type) return backend_error(state->diagnostics,
        "GC barrier store has no value type");
    th_type *atomic_element = atomic_element_type(type);
    ir_class_t category = ir_machine_of(state->function->owner,
                                       atomic_element ? atomic_element : type)
                              .class;
    if (category == IR_CLASS_VOID || category == IR_CLASS_BOOL
        || category == IR_CLASS_SINT || category == IR_CLASS_UINT
        || category == IR_CLASS_FLOAT) return true;
    uint32_t index = runtime_type_index(state, type);
    if (!index) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "GC barrier store in '%s' has no ABI type index for %s",
            state->function->name,
            th_print(ir_module_types(state->function->owner), type));
        bool reported = backend_error(state->diagnostics, message.data);
        core_buffer_release(&message);
        return reported;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef params[] = {pointer, i32};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                             params, 2, false);
    LLVMValueRef barrier = LLVMGetNamedFunction(state->module,
        "builtin.gc.alloc.barrier_runtime");
    if (!barrier) barrier = LLVMAddFunction(state->module,
        "builtin.gc.alloc.barrier_runtime", signature);
    LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(value));
    if (!slot) return false;
    LLVMBuildStore(state->builder, value, slot);
    LLVMValueRef args[] = {slot, LLVMConstInt(i32, index, false)};
    return LLVMBuildCall2(state->builder, signature, barrier, args, 2,
                          "") != nullptr;
}

static LLVMValueRef
box_value(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box_type = box_llvm_type(context);
    LLVMTypeRef source_type = LLVMTypeOf(value);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    uint32_t index = runtime_type_index(state, type);
    if (!box_type || !layout || !index || index >= (1u << 31)
        || LLVMGetTypeKind(source_type) == LLVMVoidTypeKind) return nullptr;
    uint64_t size = LLVMABISizeOfType(layout, source_type);
    LLVMTypeRef word_type = LLVMInt64TypeInContext(context);
    LLVMValueRef bits = nullptr;
    if (size <= sizeof(uint64_t)) {
        LLVMValueRef slot = LLVMBuildAlloca(state->builder, word_type,
                                            "box.inline");
        LLVMBuildStore(state->builder, LLVMConstNull(word_type), slot);
        LLVMBuildStore(state->builder, value, slot);
        bits = LLVMBuildLoad2(state->builder, word_type, slot, "box.bits");
    }
    else {
        LLVMValueRef slot = LLVMBuildAlloca(state->builder, source_type,
                                            "box.storage");
        LLVMBuildStore(state->builder, value, slot);
        bits = LLVMBuildPtrToInt(state->builder, slot, word_type,
                                 "box.pointer");
    }
    LLVMValueRef box = LLVMGetUndef(box_type);
    box = LLVMBuildInsertValue(state->builder, box,
        LLVMConstNull(LLVMInt16TypeInContext(context)), 0, "box.extent");
    box = LLVMBuildInsertValue(state->builder, box,
        LLVMConstNull(LLVMInt16TypeInContext(context)), 1, "box.stride");
    box = LLVMBuildInsertValue(state->builder, box,
        LLVMConstInt(LLVMInt32TypeInContext(context), index
            | (size > sizeof(uint64_t) ? UINT32_C(0x80000000) : 0), false),
        2, "box.type");
    return LLVMBuildInsertValue(state->builder, box, bits, 3, "box.value");
}

static LLVMValueRef
unbox_value(lower_state_t *state, LLVMValueRef box, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef target = value_type(state->function->owner, type, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!target || !layout || LLVMGetTypeKind(target) == LLVMVoidTypeKind)
        return nullptr;
    LLVMValueRef bits = LLVMBuildExtractValue(state->builder, box, 3,
                                              "unbox.bits");
    LLVMValueRef slot = LLVMABISizeOfType(layout, target) <= sizeof(uint64_t)
        ? LLVMBuildAlloca(state->builder,
            LLVMInt64TypeInContext(context), "unbox.inline")
        : LLVMBuildIntToPtr(state->builder, bits,
            LLVMPointerTypeInContext(context, 0), "unbox.pointer");
    if (LLVMABISizeOfType(layout, target) <= sizeof(uint64_t))
        LLVMBuildStore(state->builder, bits, slot);
    return LLVMBuildLoad2(state->builder, target, slot, "unboxed");
}

static LLVMValueRef
repr_dispatch_bridge(LLVMModuleRef module)
{
    const char *name = "__n00bb_repr_dispatch_bridge";
    LLVMValueRef bridge = LLVMGetNamedFunction(module, name);
    if (bridge) return bridge;
    LLVMValueRef dispatch = LLVMGetNamedFunction(module,
        "builtin.io.raw.__raw_repr_dispatch");
    if (!dispatch) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef dispatch_type = LLVMGlobalGetValueType(dispatch);
    if (LLVMCountParamTypes(dispatch_type) != 2
        || LLVMTypeOf(LLVMGetParam(dispatch, 0)) != box_llvm_type(context)
        || LLVMTypeOf(LLVMGetParam(dispatch, 1))
            != LLVMInt32TypeInContext(context)) return nullptr;
    LLVMTypeRef parameters[] = {
        LLVMInt16TypeInContext(context), LLVMInt16TypeInContext(context),
        LLVMInt32TypeInContext(context), LLVMInt64TypeInContext(context),
        LLVMInt32TypeInContext(context),
    };
    LLVMTypeRef signature = LLVMFunctionType(LLVMInt32TypeInContext(context),
                                             parameters, 5, false);
    bridge = LLVMAddFunction(module, name, signature);
    if (!bridge) return nullptr;
    LLVMSetLinkage(bridge, LLVMPrivateLinkage);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    if (!builder) return nullptr;
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, bridge,
                                                              "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef box = LLVMGetUndef(box_llvm_type(context));
    for (unsigned i = 0; i < 4; i++)
        box = LLVMBuildInsertValue(builder, box, LLVMGetParam(bridge, i),
                                   i, "repr.box");
    LLVMValueRef arguments[] = {box, LLVMGetParam(bridge, 4)};
    LLVMValueRef result = LLVMBuildCall2(builder, dispatch_type, dispatch,
                                         arguments, 2, "repr.dispatch");
    LLVMBuildRet(builder, result);
    LLVMDisposeBuilder(builder);
    return bridge;
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
            || !instruction->dest) break;
        ir_machine_t machine = ir_machine_of(state->function->owner,
            instruction->dest->type);
        if (operands[0].lit.is_float) {
            if (machine.class != IR_CLASS_FLOAT || machine.width != 64)
                break;
            result = LLVMConstReal(LLVMDoubleTypeInContext(
                LLVMGetModuleContext(state->module)), operands[0].lit.f);
            return save_result(state, instruction, result);
        }
        LLVMTypeRef type = scalar_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        result = integer_constant(machine, type, operands[0].lit.i);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    }
    case IR_OP_ENUM_CONST: {
        if (count != 2 || operands[0].kind != IR_OPD_NAME
            || operands[1].kind != IR_OPD_CONST
            || operands[1].lit.is_float || !instruction->dest) break;
        ir_module_t *source = state->function->owner;
        th_type *representation = ir_type_definition(source,
            instruction->dest->type);
        ir_machine_t machine = ir_machine_of(source, representation);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef semantic = scalar_type(source, representation, context);
        LLVMTypeRef storage = value_type(source, instruction->dest->type,
                                         context);
        LLVMValueRef value = integer_constant(machine, semantic,
                                               operands[1].lit.i);
        if (!value || !storage
            || LLVMGetTypeKind(storage) != LLVMIntegerTypeKind) break;
        unsigned source_bits = LLVMGetIntTypeWidth(semantic);
        unsigned target_bits = LLVMGetIntTypeWidth(storage);
        if (source_bits > target_bits) break;
        result = source_bits == target_bits ? value
            : machine.class == IR_CLASS_SINT
                ? LLVMBuildSExt(state->builder, value, storage, "enum.const")
                : LLVMBuildZExt(state->builder, value, storage, "enum.const");
        return save_result(state, instruction, result);
    }
    case IR_OP_ENUM_VALUE: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        ir_module_t *source = state->function->owner;
        th_type *representation = ir_type_definition(source,
            operands[0].value->type);
        uint64_t semantic_bits = 0;
        th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(source, instruction->dest->type,
                                        LLVMGetModuleContext(state->module));
        if (!representation || !th_bitint_info(representation,
                &semantic_bits, &flavor) || !value || !target
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(target) != LLVMIntegerTypeKind) break;
        unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(value));
        unsigned target_bits = LLVMGetIntTypeWidth(target);
        if (target_bits < semantic_bits) break;
        result = target_bits == source_bits ? value
            : target_bits < source_bits
                ? LLVMBuildTrunc(state->builder, value, target, "enum.value")
                : flavor == TH_BITINT_SIGNED
                    ? LLVMBuildSExt(state->builder, value, target,
                                   "enum.value")
                    : LLVMBuildZExt(state->builder, value, target,
                                   "enum.value");
        return save_result(state, instruction, result);
    }
    case IR_OP_COPY:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE) break;
        result = lowered_value(state, operands[0].value);
        return result && save_result(state, instruction, result);
    case IR_OP_NOT: {
        if (count != 1 || instruction->n_mods
            || operands[0].kind != IR_OPD_VALUE || !instruction->dest)
            break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
            break;
        LLVMTypeRef storage = LLVMTypeOf(value);
        th_type *definition = ir_type_definition(state->function->owner,
            operands[0].value->type);
        uint64_t semantic_bits = 0;
        th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
        if (definition && th_kind_of(definition) == TH_BITINT
            && th_bitint_info(definition, &semantic_bits, &flavor)
            && semantic_bits < LLVMGetIntTypeWidth(storage)) {
            LLVMTypeRef narrow = LLVMIntTypeInContext(
                LLVMGetModuleContext(state->module), (unsigned)semantic_bits);
            LLVMValueRef clipped = LLVMBuildTrunc(state->builder, value,
                                                   narrow, "not.narrow");
            LLVMValueRef inverted = LLVMBuildNot(state->builder, clipped,
                                                 "not.bits");
            result = flavor == TH_BITINT_SIGNED
                ? LLVMBuildSExt(state->builder, inverted, storage, "not.signed")
                : LLVMBuildZExt(state->builder, inverted, storage, "not.unsigned");
        } else {
            result = LLVMBuildNot(state->builder, value, "not.bits");
        }
        return result && save_result(state, instruction, result);
    }
    case IR_OP_CLZ:
    case IR_OP_CTZ: {
        if (count != 1 || instruction->n_mods
            || operands[0].kind != IR_OPD_VALUE || !instruction->dest)
            break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!value || !target
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(target) != LLVMIntegerTypeKind) break;
        const char *name = instruction->op == IR_OP_CLZ
            ? "llvm.ctlz" : "llvm.cttz";
        unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
        LLVMTypeRef source = LLVMTypeOf(value);
        LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(state->module,
            id, &source, 1);
        LLVMValueRef arguments[] = {
            value,
            LLVMConstInt(LLVMInt1TypeInContext(
                LLVMGetModuleContext(state->module)), 0, false),
        };
        LLVMValueRef counted = intrinsic ? LLVMBuildCall2(state->builder,
            LLVMGlobalGetValueType(intrinsic), intrinsic, arguments, 2,
            "bit.count") : nullptr;
        if (!counted || LLVMGetIntTypeWidth(target)
                > LLVMGetIntTypeWidth(LLVMTypeOf(counted))) break;
        result = target == LLVMTypeOf(counted) ? counted
            : LLVMBuildTrunc(state->builder, counted, target, "bit.count.u32");
        return result && save_result(state, instruction, result);
    }
    case IR_OP_UNDEF: {
        if (count || !instruction->dest) break;
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) break;
        return save_result(state, instruction, LLVMGetUndef(type));
    }
    case IR_OP_NIL:
        return count == 0 && instruction->dest
            && ir_machine_of(state->function->owner,
                             instruction->dest->type).class == IR_CLASS_VOID;
    case IR_OP_OPTION_NONE:
        if (count || !instruction->dest) break;
        {
            LLVMTypeRef type = value_type(state->function->owner,
                instruction->dest->type,
                LLVMGetModuleContext(state->module));
            if (!type || (LLVMGetTypeKind(type) != LLVMPointerTypeKind
                && LLVMGetTypeKind(type) != LLVMStructTypeKind)) break;
            result = LLVMConstNull(type);
        }
        return save_result(state, instruction, result);
    case IR_OP_OPTION_SOME:
    case IR_OP_OPTION_GET:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest) break;
        result = lowered_value(state, operands[0].value);
        if (!result) break;
        if (instruction->op == IR_OP_OPTION_SOME) {
            LLVMTypeRef option = value_type(state->function->owner,
                instruction->dest->type,
                LLVMGetModuleContext(state->module));
            if (option && LLVMGetTypeKind(option) == LLVMStructTypeKind
                && LLVMStructGetTypeAtIndex(option, 0) == LLVMTypeOf(result)) {
                LLVMValueRef stored = LLVMBuildInsertValue(state->builder,
                    LLVMGetUndef(option), result, 0, "option.payload");
                result = LLVMBuildInsertValue(state->builder, stored,
                    LLVMConstInt(LLVMInt8TypeInContext(
                        LLVMGetModuleContext(state->module)), 1, false),
                    1, "option.some");
            }
            else if (!option || LLVMGetTypeKind(option)
                    != LLVMPointerTypeKind) break;
        }
        else if (LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMStructTypeKind)
            result = LLVMBuildExtractValue(state->builder, result, 0,
                                           "option.get");
        return save_result(state, instruction, result);
    case IR_OP_OPTION_IS_NIL: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value) break;
        LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(value));
        if (kind == LLVMStructTypeKind)
            value = LLVMBuildExtractValue(state->builder, value, 1,
                                          "option.tag");
        else if (kind != LLVMPointerTypeKind) break;
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, value,
            LLVMConstNull(LLVMTypeOf(value)), "option.is_nil");
        return save_result(state, instruction, result);
    }
    case IR_OP_OPTION_GET_OR_ELSE: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMValueRef fallback = lowered_value(state, operands[1].value);
        if (!value || !fallback) break;
        LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(value));
        LLVMValueRef tag = value;
        if (kind == LLVMStructTypeKind) {
            if (LLVMStructGetTypeAtIndex(LLVMTypeOf(value), 0)
                != LLVMTypeOf(fallback)) break;
            tag = LLVMBuildExtractValue(state->builder, value, 1,
                                        "option.tag");
            value = LLVMBuildExtractValue(state->builder, value, 0,
                                          "option.payload");
        }
        else if (kind != LLVMPointerTypeKind
            || LLVMTypeOf(value) != LLVMTypeOf(fallback)) break;
        LLVMValueRef is_nil = LLVMBuildICmp(state->builder, LLVMIntEQ,
            tag, LLVMConstNull(LLVMTypeOf(tag)), "option.is_nil");
        result = LLVMBuildSelect(state->builder, is_nil, fallback,
                                  value, "option.get_or_else");
        return save_result(state, instruction, result);
    }
    case IR_OP_ADDRESS_OF: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        ir_value_t *binding = operands[0].value;
        if (binding->kind == IR_VAL_LOCAL && binding->local) {
            for (uint32_t i = 0; i < state->function->n_locals; i++) {
                if (state->function->locals[i] == binding->local) {
                    result = state->locals[i];
                    break;
                }
            }
        } else if (binding->kind == IR_VAL_GLOBAL && binding->global) {
            result = global_ref(state->module, binding->global,
                state->global_bindings, state->global_binding_count);
        }
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    }
    case IR_OP_FIELD: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME || !operands[1].name
            || !instruction->dest) break;
        th_type *record_type = operands[0].value
            ? th_ind_referent(operands[0].value->type) : nullptr;
        LLVMTypeRef native = nullptr;
        unsigned member_index = 0;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        if (!record_type || !address) break;
        if (!record_member_index(state, record_type, operands[1].name,
                &native, &member_index)) {
            core_buffer_t message = {0};
            ir_module_t *source = state->function->owner;
            const ir_final_layout_t *layout = ir_layout_final(source,
                record_type, ir_module_target(source));
            LLVMTypeRef candidate = value_type(source, record_type,
                LLVMGetModuleContext(state->module));
            LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
            uint64_t extent = 0;
            if (layout) ir_layout_final_byte_extent(layout, &extent);
            core_buffer_printf(&message,
                "LLVM backend cannot address field '%s' in %s "
                "(layout %llu bytes, native %llu bytes)",
                operands[1].name,
                th_print(ir_module_types(state->function->owner),
                         record_type),
                (unsigned long long)extent,
                (unsigned long long)(candidate && target
                    ? LLVMABISizeOfType(target, candidate) : 0));
            backend_error(state->diagnostics, message.data);
            core_buffer_release(&message);
            return false;
        }
        result = LLVMBuildStructGEP2(state->builder, native, address,
                                     member_index, "field.address");
        return save_result(state, instruction, result);
    }
    case IR_OP_INDEX: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE || !instruction->dest)
            break;
        uint64_t extent = 0;
        LLVMTypeRef array = inline_index_base_type(state,
            operands[0].value, &extent);
        LLVMValueRef base = lowered_value(state, operands[0].value);
        LLVMValueRef index = lowered_value(state, operands[1].value);
        if (!array || !base || !index || !extent
            || LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind
            || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind)
            break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        unsigned width = LLVMGetIntTypeWidth(LLVMTypeOf(index));
        LLVMTypeRef arithmetic = LLVMIntTypeInContext(context,
            width > 64 ? width : 64);
        bool signed_index = ir_machine_of(state->function->owner,
            operands[1].value->type).class == IR_CLASS_SINT;
        LLVMValueRef position = LLVMBuildIntCast2(state->builder, index,
            arithmetic, signed_index, "index.extended");
        LLVMValueRef limit = LLVMConstInt(arithmetic, extent, false);
        if (signed_index) {
            LLVMValueRef negative = LLVMBuildICmp(state->builder, LLVMIntSLT,
                position, LLVMConstNull(arithmetic), "index.negative");
            LLVMValueRef from_end = LLVMBuildAdd(state->builder, position,
                limit, "index.from.end");
            position = LLVMBuildSelect(state->builder, negative, from_end,
                position, "index.position");
        }
        LLVMValueRef invalid = LLVMBuildICmp(state->builder, LLVMIntUGE,
            position, limit, "index.out.of.bounds");
        emit_trap_if(state, invalid);
        LLVMValueRef indices[] = {
            LLVMConstInt(arithmetic, 0, false), position,
        };
        result = LLVMBuildInBoundsGEP2(state->builder, array, base,
            indices, 2, "index.address");
        return save_result(state, instruction, result);
    }
    case IR_OP_OFFSET: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE || !instruction->dest
            || !operands[0].value) break;
        th_type *referent = th_ind_referent(operands[0].value->type);
        LLVMTypeRef element = referent ? value_type(state->function->owner,
            referent, LLVMGetModuleContext(state->module)) : nullptr;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef offset = lowered_value(state, operands[1].value);
        if (!element || LLVMGetTypeKind(element) == LLVMVoidTypeKind
            || !address || LLVMGetTypeKind(LLVMTypeOf(address))
                != LLVMPointerTypeKind
            || !offset || LLVMGetTypeKind(LLVMTypeOf(offset))
                != LLVMIntegerTypeKind) break;
        result = LLVMBuildGEP2(state->builder, element, address,
                               &offset, 1, "offset.address");
        return save_result(state, instruction, result);
    }
    case IR_OP_DIFF: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE || !instruction->dest)
            break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right
            || LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMPointerTypeKind
            || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        result = LLVMBuildPtrDiff2(state->builder,
            LLVMInt8TypeInContext(LLVMGetModuleContext(state->module)),
            left, right, "byte.diff");
        return save_result(state, instruction, result);
    }
    case IR_OP_MEMCPY:
    case IR_OP_MEMSET: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || operands[2].kind != IR_OPD_VALUE || instruction->dest)
            break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef source = lowered_value(state, operands[1].value);
        LLVMValueRef length = lowered_value(state, operands[2].value);
        if (!address || !source || !length
            || LLVMGetTypeKind(LLVMTypeOf(address)) != LLVMPointerTypeKind
            || LLVMGetTypeKind(LLVMTypeOf(length)) != LLVMIntegerTypeKind)
            break;
        if (instruction->op == IR_OP_MEMCPY) {
            if (LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMPointerTypeKind)
                break;
            return LLVMBuildMemCpy(state->builder, address, 1, source, 1,
                                   length) != nullptr;
        }
        if (LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMIntegerTypeKind
            || LLVMGetIntTypeWidth(LLVMTypeOf(source)) != 8) break;
        return LLVMBuildMemSet(state->builder, address, source, length, 1)
            != nullptr;
    }
    case IR_OP_CAST: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        LLVMValueRef source = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!source || !target) break;
        ir_machine_t from = ir_machine_of(state->function->owner,
                                          operands[0].value->type);
        ir_machine_t to = ir_machine_of(state->function->owner,
                                        instruction->dest->type);
        bool from_integer = from.class == IR_CLASS_SINT
            || from.class == IR_CLASS_UINT;
        bool to_integer = to.class == IR_CLASS_SINT
            || to.class == IR_CLASS_UINT;
        if (from_integer && to.class == IR_CLASS_FLOAT) {
            if (instruction->n_mods != 1
                || !ir_instr_has_mod(instruction, "nearest")
                || LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMIntegerTypeKind
                || LLVMGetTypeKind(target) != LLVMDoubleTypeKind) break;
            result = from.class == IR_CLASS_SINT
                ? LLVMBuildSIToFP(state->builder, source, target, "cast.sitofp")
                : LLVMBuildUIToFP(state->builder, source, target, "cast.uitofp");
            return result && save_result(state, instruction, result);
        }
        if (from.class == IR_CLASS_FLOAT && to_integer) {
            if (instruction->n_mods != 2
                || !ir_instr_has_mod(instruction, "trap")
                || !ir_instr_has_mod(instruction, "to_zero")
                || LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMDoubleTypeKind
                || LLVMGetTypeKind(target) != LLVMIntegerTypeKind) break;
            unsigned id = LLVMLookupIntrinsicID("llvm.trunc",
                                               sizeof("llvm.trunc") - 1);
            LLVMTypeRef source_type = LLVMTypeOf(source);
            LLVMValueRef truncation = LLVMGetIntrinsicDeclaration(
                state->module, id, &source_type, 1);
            LLVMValueRef truncated = truncation ? LLVMBuildCall2(
                state->builder, LLVMGlobalGetValueType(truncation), truncation,
                &source, 1, "cast.trunc_float") : nullptr;
            if (!truncated) break;
            unsigned width = LLVMGetIntTypeWidth(target);
            double upper = 1.0;
            for (unsigned bit = to.class == IR_CLASS_SINT ? 1 : 0;
                 bit < width; bit++) upper *= 2.0;
            LLVMValueRef lower_bound = LLVMConstReal(source_type,
                to.class == IR_CLASS_SINT ? -upper : 0.0);
            LLVMValueRef upper_bound = LLVMConstReal(source_type, upper);
            LLVMValueRef above_lower = LLVMBuildFCmp(state->builder,
                LLVMRealOGE, truncated, lower_bound, "cast.above_lower");
            LLVMValueRef below_upper = LLVMBuildFCmp(state->builder,
                LLVMRealOLT, truncated, upper_bound, "cast.below_upper");
            LLVMValueRef in_range = LLVMBuildAnd(state->builder, above_lower,
                below_upper, "cast.in_range");
            emit_trap_if(state, LLVMBuildNot(state->builder, in_range,
                                            "cast.invalid_float"));
            result = to.class == IR_CLASS_SINT
                ? LLVMBuildFPToSI(state->builder, source, target, "cast.fptosi")
                : LLVMBuildFPToUI(state->builder, source, target, "cast.fptoui");
            return result && save_result(state, instruction, result);
        }
        if (LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(target) != LLVMIntegerTypeKind
            || instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap")
                && !ir_instr_has_mod(instruction, "wrap"))) break;
        if ((from.class != IR_CLASS_SINT && from.class != IR_CLASS_UINT)
            || (to.class != IR_CLASS_SINT && to.class != IR_CLASS_UINT)) break;
        unsigned from_width = LLVMGetIntTypeWidth(LLVMTypeOf(source));
        unsigned to_width = LLVMGetIntTypeWidth(target);
        bool checked = ir_instr_has_mod(instruction, "trap");
        if (checked && from.class == IR_CLASS_SINT
            && to.class == IR_CLASS_UINT)
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntSLT,
                source, LLVMConstNull(LLVMTypeOf(source)), "cast.negative"));
        result = to_width == from_width ? source
            : to_width < from_width
                ? LLVMBuildTrunc(state->builder, source, target, "cast.trunc")
                : from.class == IR_CLASS_SINT
                    ? LLVMBuildSExt(state->builder, source, target, "cast.sext")
                    : LLVMBuildZExt(state->builder, source, target, "cast.zext");
        if (checked && to_width < from_width) {
            LLVMValueRef restored = to.class == IR_CLASS_SINT
                ? LLVMBuildSExt(state->builder, result, LLVMTypeOf(source),
                                "cast.restore")
                : LLVMBuildZExt(state->builder, result, LLVMTypeOf(source),
                                "cast.restore");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                restored, source, "cast.out_of_range"));
        }
        if (checked && from.class == IR_CLASS_UINT
            && to.class == IR_CLASS_SINT)
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntSLT,
                result, LLVMConstNull(target), "cast.to_signed_overflow"));
        return save_result(state, instruction, result);
    }
    case IR_OP_BITCAST: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest) break;
        LLVMValueRef source = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!source || !target
            || LLVMGetTypeKind(target) == LLVMVoidTypeKind) break;
        result = LLVMTypeOf(source) == target ? source
            : LLVMBuildBitCast(state->builder, source, target, "bitcast");
        return save_result(state, instruction, result);
    }
    case IR_OP_EXTRACT: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME || !operands[1].name
            || !instruction->dest || !operands[0].value) break;
        th_type *record_type = operands[0].value->type;
        LLVMTypeRef native = nullptr;
        unsigned member_index = 0;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value || !record_member_index(state, record_type,
                operands[1].name, &native, &member_index)
            || LLVMTypeOf(value) != native) break;
        result = LLVMBuildExtractValue(state->builder, value, member_index,
                                       "field.value");
        return result && LLVMTypeOf(result) == value_type(
            state->function->owner, instruction->dest->type,
            LLVMGetModuleContext(state->module))
            && save_result(state, instruction, result);
    }
    case IR_OP_PHI: {
        if (!instruction->dest) break;
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) break;
        result = LLVMBuildPhi(state->builder, type, "merge");
        return save_result(state, instruction, result);
    }
    case IR_OP_ALLOCA:
    case IR_OP_ALLOC: {
        if (count != 1 || operands[0].kind != IR_OPD_TYPE
            || !instruction->dest) break;
        LLVMTypeRef type = value_type(state->function->owner,
            operands[0].type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) break;
        if (instruction->op == IR_OP_ALLOCA
            || allocation_has_only_local_uses(state->function,
                                              instruction)) {
            result = entry_alloca(state, type);
        } else {
            bool required = false;
            const char *storage_class = ir_instr_storage_class(instruction,
                                                               &required);
            if (storage_class && strcmp(storage_class, "gc")) break;
            const ir_final_layout_t *layout = ir_layout_final(
                state->function->owner, operands[0].type,
                ir_module_target(state->function->owner));
            uint64_t bytes = 0;
            LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
            uint32_t type_index = runtime_type_index(state, operands[0].type);
            if (!layout || !target || !type_index
                || !layout->alignment_bytes
                || !ir_layout_final_byte_extent(layout, &bytes)
                || LLVMABISizeOfType(target, type) != bytes
                || LLVMABIAlignmentOfType(target, type)
                    != layout->alignment_bytes) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "allocation layout mismatch (type index %u, IR size %llu, LLVM size %llu, IR alignment %u, LLVM alignment %u)",
                    type_index, (unsigned long long)bytes,
                    (unsigned long long)(target
                        ? LLVMABISizeOfType(target, type) : 0),
                    layout ? layout->alignment_bytes : 0,
                    target ? LLVMABIAlignmentOfType(target, type) : 0);
                bool reported = backend_error(state->diagnostics,
                                              message.data);
                core_buffer_release(&message);
                return reported;
            }
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMTypeRef params[] = {i32, i32, i64, i32, pointer};
            LLVMTypeRef allocator_type = LLVMFunctionType(pointer, params,
                                                          5, false);
            LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                          "n00b_allocate");
            if (!allocator)
                allocator = LLVMAddFunction(state->module, "n00b_allocate",
                                            allocator_type);
            LLVMValueRef args[] = {
                LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
                LLVMConstInt(i32, type_index, false),
                LLVMConstInt(i64, bytes, false),
                LLVMConstInt(i32, layout->alignment_bytes, false),
                LLVMConstNull(pointer),
            };
            result = LLVMBuildCall2(state->builder, allocator_type, allocator,
                                    args, 5, "storage");
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_GLOBAL_STORE: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || !operands[0].value
            || operands[0].value->kind != IR_VAL_GLOBAL
            || !operands[0].value->global) break;
        LLVMValueRef global = global_ref(state->module,
            operands[0].value->global, state->global_bindings,
            state->global_binding_count);
        LLVMValueRef value = lowered_value(state, operands[1].value);
        if (!global || !value
            || LLVMGlobalGetValueType(global) != LLVMTypeOf(value)) break;
        if (!store_gc_barrier(state, value,
                operands[0].value->global->type)) return false;
        LLVMBuildStore(state->builder, value, global);
        return true;
    }
    case IR_OP_LOAD: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest
            || (instruction->n_mods != 0
                && (instruction->n_mods != 1
                    || (!ir_instr_has_mod(instruction, "atomic")
                        && !ir_instr_has_mod(instruction, "volatile")))))
            break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!address || !type || LLVMGetTypeKind(type) == LLVMVoidTypeKind)
            break;
        th_type *storage = th_ind_referent(operands[0].value->type);
        bool atomic = ir_instr_has_mod(instruction, "atomic");
        bool atomic_bool = atomic
            && is_atomic_bool(state->function->owner, storage);
        LLVMTypeRef memory_type = atomic_bool
            ? LLVMInt8TypeInContext(LLVMGetModuleContext(state->module))
            : type;
        LLVMValueRef loaded = LLVMBuildLoad2(state->builder, memory_type,
                                             address, "loaded");
        if (atomic) {
            const ir_final_layout_t *layout = storage
                ? ir_layout_final(state->function->owner, storage,
                    ir_module_target(state->function->owner)) : nullptr;
            if (!layout || !layout->alignment_bytes
                || (atomic_bool && layout->intrinsic_bits != 8)) break;
            LLVMSetAlignment(loaded, layout->alignment_bytes);
            LLVMSetOrdering(loaded, LLVMAtomicOrderingSequentiallyConsistent);
        }
        if (ir_instr_has_mod(instruction, "volatile"))
            LLVMSetVolatile(loaded, true);
        result = atomic_bool
            ? LLVMBuildTrunc(state->builder, loaded, type, "atomic.bool")
            : loaded;
        return save_result(state, instruction, result);
    }
    case IR_OP_STORE: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || (instruction->n_mods != 0
                && (instruction->n_mods != 1
                    || !ir_instr_has_mod(instruction, "atomic"))))
            break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef value = lowered_value(state, operands[1].value);
        if (!address || !value) break;
        th_type *storage = th_ind_referent(operands[0].value->type);
        bool atomic_bool = instruction->n_mods
            && is_atomic_bool(state->function->owner, storage);
        if (atomic_bool) {
            if (LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
                || LLVMGetIntTypeWidth(LLVMTypeOf(value)) != 1) break;
            value = LLVMBuildZExt(state->builder, value,
                LLVMInt8TypeInContext(LLVMGetModuleContext(state->module)),
                "atomic.bool.byte");
        }
        if (!store_gc_barrier(state, value, storage)) return false;
        result = LLVMBuildStore(state->builder, value, address);
        if (instruction->n_mods) {
            const ir_final_layout_t *layout = storage
                ? ir_layout_final(state->function->owner, storage,
                    ir_module_target(state->function->owner)) : nullptr;
            if (!layout || !layout->alignment_bytes
                || (atomic_bool && layout->intrinsic_bits != 8)) break;
            LLVMSetAlignment(result, layout->alignment_bytes);
            LLVMSetOrdering(result, LLVMAtomicOrderingSequentiallyConsistent);
        }
        return true;
    }
    case IR_OP_ATOMIC_RMW: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME
            || !operands[1].name
            || (strcmp(operands[1].name, "or")
                && strcmp(operands[1].name, "and")
                && strcmp(operands[1].name, "xchg")
                && strcmp(operands[1].name, "add")
                && strcmp(operands[1].name, "sub"))
            || operands[2].kind != IR_OPD_VALUE || !instruction->dest
            || instruction->n_mods) break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef value = lowered_value(state, operands[2].value);
        th_type *storage = th_ind_referent(operands[0].value->type);
        bool atomic_bool = is_atomic_bool(state->function->owner, storage);
        if (atomic_bool && value
            && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMTypeOf(value)) == 1)
            value = LLVMBuildZExt(state->builder, value,
                LLVMInt8TypeInContext(LLVMGetModuleContext(state->module)),
                "atomic.bool.byte");
        const ir_final_layout_t *layout = storage
            ? ir_layout_final(state->function->owner, storage,
                ir_module_target(state->function->owner)) : nullptr;
        bool pointer = value
            && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMPointerTypeKind;
        bool integer = value
            && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMIntegerTypeKind;
        unsigned width = pointer ? sizeof(void *) * 8
            : integer ? LLVMGetIntTypeWidth(LLVMTypeOf(value)) : 0;
        if (!address || !value || !layout
            || (!integer && (!pointer
                || strcmp(operands[1].name, "xchg")))
            || layout->intrinsic_bits != width
            || layout->alignment_bytes < width / 8)
            break;
        LLVMAtomicRMWBinOp operation = !strcmp(operands[1].name, "xchg")
            ? LLVMAtomicRMWBinOpXchg
            : !strcmp(operands[1].name, "or")
                ? LLVMAtomicRMWBinOpOr
                : !strcmp(operands[1].name, "and")
                    ? LLVMAtomicRMWBinOpAnd
                : !strcmp(operands[1].name, "add")
                    ? LLVMAtomicRMWBinOpAdd : LLVMAtomicRMWBinOpSub;
        result = LLVMBuildAtomicRMW(state->builder, operation,
            address, value, LLVMAtomicOrderingSequentiallyConsistent, false);
        if (atomic_bool && result)
            result = LLVMBuildTrunc(state->builder, result,
                LLVMInt1TypeInContext(LLVMGetModuleContext(state->module)),
                "atomic.bool");
        return result && save_result(state, instruction, result);
    }
    case IR_OP_ATOMIC_CAS: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || operands[2].kind != IR_OPD_VALUE || !instruction->dest
            || instruction->n_mods) break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef expected = lowered_value(state, operands[1].value);
        LLVMValueRef desired = lowered_value(state, operands[2].value);
        th_type *storage = th_ind_referent(operands[0].value->type);
        bool atomic_bool = is_atomic_bool(state->function->owner, storage);
        if (atomic_bool && expected && desired
            && LLVMGetTypeKind(LLVMTypeOf(expected)) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMTypeOf(expected)) == 1
            && LLVMTypeOf(expected) == LLVMTypeOf(desired)) {
            LLVMTypeRef byte = LLVMInt8TypeInContext(
                LLVMGetModuleContext(state->module));
            expected = LLVMBuildZExt(state->builder, expected, byte,
                                     "atomic.expected.byte");
            desired = LLVMBuildZExt(state->builder, desired, byte,
                                    "atomic.desired.byte");
        }
        const ir_final_layout_t *layout = storage
            ? ir_layout_final(state->function->owner, storage,
                ir_module_target(state->function->owner)) : nullptr;
        bool pointer = expected
            && LLVMGetTypeKind(LLVMTypeOf(expected)) == LLVMPointerTypeKind;
        bool integer = expected
            && LLVMGetTypeKind(LLVMTypeOf(expected)) == LLVMIntegerTypeKind;
        unsigned width = pointer ? sizeof(void *) * 8
            : integer ? LLVMGetIntTypeWidth(LLVMTypeOf(expected)) : 0;
        if (!address || !expected || !desired || !layout
            || LLVMTypeOf(expected) != LLVMTypeOf(desired)
            || (!pointer && !integer)
            || layout->intrinsic_bits != width
            || layout->alignment_bytes < width / 8)
            break;
        LLVMValueRef exchange = LLVMBuildAtomicCmpXchg(state->builder,
            address, expected, desired,
            LLVMAtomicOrderingSequentiallyConsistent,
            LLVMAtomicOrderingSequentiallyConsistent, false);
        result = exchange
            ? LLVMBuildExtractValue(state->builder, exchange, 1, "exchanged")
            : nullptr;
        return result && save_result(state, instruction, result);
    }
    case IR_OP_SIGNAL_FENCE:
        if (count || instruction->dest || instruction->n_mods) break;
        return LLVMBuildFence(state->builder,
            LLVMAtomicOrderingSequentiallyConsistent, true, "") != nullptr;
    case IR_OP_TYPE_OF: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest)
            break;
        th_type *source_type = th_resolve(operands[0].value->type);
        LLVMTypeRef descriptor_type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!descriptor_type
            || LLVMGetTypeKind(descriptor_type) != LLVMIntegerTypeKind
            || LLVMGetIntTypeWidth(descriptor_type) != 32)
            break;
        const char *name = nullptr;
        size_t arguments = 0;
        bool dynamic = !th_is_closed(ir_module_types(state->function->owner),
            source_type) || th_kind_of(source_type) == TH_SUM
            || (th_named_info(source_type, &name, nullptr, nullptr,
                    &arguments) && name && !strcmp(name, "any")
                && !arguments);
        if (dynamic) {
            LLVMValueRef value = lowered_value(state, operands[0].value);
            LLVMTypeRef box = box_llvm_type(
                LLVMGetModuleContext(state->module));
            if (!value || LLVMTypeOf(value) != box) break;
            result = LLVMBuildExtractValue(state->builder, value, 2,
                                           "runtime.type.index");
        }
        else {
            uint32_t index = runtime_type_index(state, source_type);
            if (!index) break;
            result = LLVMConstInt(descriptor_type, index, false);
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_TYPE_TEST: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_TYPE) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (!value || LLVMTypeOf(value) != box_llvm_type(context)) break;
        if (!th_is_closed(ir_module_types(state->function->owner),
                          operands[1].type)) {
            th_type *pattern = th_resolve(operands[1].type);
            th_type *element = th_kind_of(pattern) == TH_SEQUENCE
                ? th_resolve(th_seq_elem(pattern)) : nullptr;
            if (!element || th_kind_of(element) != TH_VAR
                || th_var_constraint_count(element)) break;
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef signature = LLVMFunctionType(i32, &i32, 1, false);
            LLVMValueRef predicate = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_type_is_array");
            if (!predicate) predicate = LLVMAddFunction(state->module,
                "n00bb_bootstrap_type_is_array", signature);
            LLVMValueRef type_field = LLVMBuildExtractValue(state->builder,
                value, 2, "type.field");
            LLVMValueRef match = predicate ? LLVMBuildCall2(state->builder,
                signature, predicate, &type_field, 1, "type.is_array")
                : nullptr;
            if (!match) break;
            result = LLVMBuildICmp(state->builder, LLVMIntNE, match,
                LLVMConstNull(i32), "type.matches");
            return save_result(state, instruction, result);
        }
        uint32_t index = runtime_type_index(state, operands[1].type);
        if (!index) break;
        LLVMValueRef actual = LLVMBuildExtractValue(state->builder, value,
                                                    2, "type.index");
        actual = LLVMBuildAnd(state->builder, actual,
            LLVMConstInt(LLVMTypeOf(actual), UINT32_C(0x7fffffff), false),
            "type.index.masked");
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, actual,
            LLVMConstInt(LLVMTypeOf(actual), index, false), "type.matches");
        return save_result(state, instruction, result);
    }
    case IR_OP_TYPE_NARROW: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_TYPE) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value || LLVMTypeOf(value) != box_llvm_type(
                LLVMGetModuleContext(state->module))) break;
        result = unbox_value(state, value, operands[1].type);
        return result && save_result(state, instruction, result);
    }
    case IR_OP_AGGREGATE:
        result = tuple_aggregate(state, instruction);
        if (!result) result = constant_array(state, instruction);
        if (!result) result = dynamic_array(state, instruction);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    case IR_OP_PHASE_RESULT:
        if (ir_func_execution_phase(state->function)
                == IR_PHASE_COMPTIME) {
            LLVMTypeRef type = instruction->dest
                ? value_type(state->function->owner,
                    instruction->dest->type,
                    LLVMGetModuleContext(state->module)) : nullptr;
            LLVMValueRef global = phase_cache_global(state->module,
                ir_instr_phase_result(instruction), type, true);
            result = global ? LLVMBuildLoad2(state->builder, type, global,
                                             "literal.cached") : nullptr;
        } else {
            result = materialized_array(state, instruction);
            if (!result) result = materialized_string(state, instruction);
            if (!result) result = materialized_reloc_array(state, instruction);
            if (!result) result = materialized_reloc_value(state, instruction);
            if (!result) result = materialized_nested_array(state, instruction);
            if (!result) result = materialized_raw_value(state, instruction);
        }
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    case IR_OP_NEG: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap"))) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value) break;
        LLVMTypeRef type = LLVMTypeOf(value);
        if (LLVMGetTypeKind(type) == LLVMDoubleTypeKind) {
            if (instruction->n_mods) break;
            result = LLVMBuildFNeg(state->builder, value, "negated");
        } else if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
            if (!instruction->n_mods) {
                result = LLVMBuildNeg(state->builder, value, "negated");
            } else {
                ir_machine_t machine = ir_machine_of(state->function->owner,
                    operands[0].value->type);
                const char *name = machine.class == IR_CLASS_SINT
                    ? "llvm.ssub.with.overflow"
                    : machine.class == IR_CLASS_UINT
                        ? "llvm.usub.with.overflow" : nullptr;
                if (!name) break;
                unsigned id = LLVMLookupIntrinsicID(name, strlen(name));
                LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(
                    state->module, id, &type, 1);
                LLVMValueRef args[] = {LLVMConstNull(type), value};
                LLVMValueRef pair = LLVMBuildCall2(state->builder,
                    LLVMGlobalGetValueType(intrinsic), intrinsic, args, 2,
                    "neg.checked");
                result = LLVMBuildExtractValue(state->builder, pair, 0,
                                               "negated");
                LLVMValueRef overflow = LLVMBuildExtractValue(state->builder,
                    pair, 1, "overflow");
                LLVMValueRef function = LLVMGetBasicBlockParent(
                    LLVMGetInsertBlock(state->builder));
                LLVMContextRef context = LLVMGetModuleContext(state->module);
                LLVMBasicBlockRef bad = LLVMAppendBasicBlockInContext(context,
                    function, "overflow");
                LLVMBasicBlockRef good = LLVMAppendBasicBlockInContext(context,
                    function, "neg.cont");
                LLVMBuildCondBr(state->builder, overflow, bad, good);
                LLVMPositionBuilderAtEnd(state->builder, bad);
                emit_trap(state);
                LLVMPositionBuilderAtEnd(state->builder, good);
            }
        } else break;
        return result && save_result(state, instruction, result);
    }
    case IR_OP_ADD:
    case IR_OP_SUB:
    case IR_OP_MUL: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        if (instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap")
                && !ir_instr_has_mod(instruction, "wrap"))) break;
        if (LLVMGetTypeKind(LLVMTypeOf(left)) == LLVMDoubleTypeKind) {
            if (instruction->n_mods) break;
            result = instruction->op == IR_OP_ADD
                ? LLVMBuildFAdd(state->builder, left, right, "arithmetic")
                : instruction->op == IR_OP_SUB
                    ? LLVMBuildFSub(state->builder, left, right, "arithmetic")
                    : LLVMBuildFMul(state->builder, left, right, "arithmetic");
            return save_result(state, instruction, result);
        }
        if (LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMIntegerTypeKind) break;
        if (ir_instr_has_mod(instruction, "trap")) {
            ir_machine_t machine = ir_machine_of(state->function->owner,
                operands[0].value->type);
            const char *intrinsic_name = nullptr;
            if (machine.class == IR_CLASS_SINT)
                intrinsic_name = instruction->op == IR_OP_ADD
                    ? "llvm.sadd.with.overflow"
                    : instruction->op == IR_OP_SUB
                        ? "llvm.ssub.with.overflow"
                        : "llvm.smul.with.overflow";
            else if (machine.class == IR_CLASS_UINT)
                intrinsic_name = instruction->op == IR_OP_ADD
                    ? "llvm.uadd.with.overflow"
                    : instruction->op == IR_OP_SUB
                        ? "llvm.usub.with.overflow"
                        : "llvm.umul.with.overflow";
            if (!intrinsic_name) break;
            unsigned id = LLVMLookupIntrinsicID(intrinsic_name,
                strlen(intrinsic_name));
            LLVMTypeRef type = LLVMTypeOf(left);
            LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(state->module,
                id, &type, 1);
            LLVMValueRef args[] = {left, right};
            LLVMValueRef pair = LLVMBuildCall2(state->builder,
                LLVMGlobalGetValueType(intrinsic), intrinsic, args, 2,
                "arithmetic.checked");
            result = LLVMBuildExtractValue(state->builder, pair, 0,
                                           "arithmetic.result");
            LLVMValueRef overflow = LLVMBuildExtractValue(state->builder, pair,
                1, "overflow");
            emit_trap_if(state, overflow);
        } else {
            result = instruction->op == IR_OP_ADD
                ? LLVMBuildAdd(state->builder, left, right, "arithmetic")
                : instruction->op == IR_OP_SUB
                    ? LLVMBuildSub(state->builder, left, right, "arithmetic")
                    : LLVMBuildMul(state->builder, left, right, "arithmetic");
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_DIV:
    case IR_OP_REM: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        LLVMTypeRef type = LLVMTypeOf(left);
        if (LLVMGetTypeKind(type) == LLVMDoubleTypeKind) {
            if (instruction->n_mods) break;
            result = instruction->op == IR_OP_DIV
                ? LLVMBuildFDiv(state->builder, left, right, "quotient")
                : LLVMBuildFRem(state->builder, left, right, "remainder");
            return save_result(state, instruction, result);
        }
        if (LLVMGetTypeKind(type) != LLVMIntegerTypeKind
            || instruction->n_mods > 2
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap"))) break;
        ir_machine_t machine = ir_machine_of(state->function->owner,
                                             operands[0].value->type);
        if (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT) break;
        LLVMValueRef zero = LLVMConstNull(type);
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntEQ,
            right, zero, "div.zero"));
        if (machine.class == IR_CLASS_SINT) {
            unsigned width = LLVMGetIntTypeWidth(type);
            unsigned nwords = (width + 63) / 64;
            uint64_t *words = calloc(nwords, sizeof(*words));
            if (!words) break;
            words[(width - 1) / 64] = UINT64_C(1) << ((width - 1) % 64);
            LLVMValueRef min = LLVMConstIntOfArbitraryPrecision(type,
                                                                 nwords, words);
            free(words);
            LLVMValueRef min_left = LLVMBuildICmp(state->builder, LLVMIntEQ,
                left, min, "div.min");
            LLVMValueRef minus_one = LLVMBuildICmp(state->builder, LLVMIntEQ,
                right, LLVMConstAllOnes(type), "div.minus_one");
            emit_trap_if(state, LLVMBuildAnd(state->builder,
                min_left, minus_one, "div.overflow"));
        }
        result = instruction->op == IR_OP_DIV
            ? machine.class == IR_CLASS_SINT
                ? LLVMBuildSDiv(state->builder, left, right, "quotient")
                : LLVMBuildUDiv(state->builder, left, right, "quotient")
            : machine.class == IR_CLASS_SINT
                ? LLVMBuildSRem(state->builder, left, right, "remainder")
                : LLVMBuildURem(state->builder, left, right, "remainder");
        return save_result(state, instruction, result);
    }
    case IR_OP_AND:
    case IR_OP_OR:
    case IR_OP_XOR: {
        if (count != 2 || instruction->n_mods
            || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)
            || LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMIntegerTypeKind)
            break;
        result = instruction->op == IR_OP_AND
            ? LLVMBuildAnd(state->builder, left, right, "bit.and")
            : instruction->op == IR_OP_OR
                ? LLVMBuildOr(state->builder, left, right, "bit.or")
                : LLVMBuildXor(state->builder, left, right, "bit.xor");
        return save_result(state, instruction, result);
    }
    case IR_OP_SHL:
    case IR_OP_SHR: {
        if (count != 2 || instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "trap"))
            || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right
            || LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(LLVMTypeOf(right)) != LLVMIntegerTypeKind)
            break;
        ir_machine_t value_machine = ir_machine_of(state->function->owner,
                                                   operands[0].value->type);
        ir_machine_t count_machine = ir_machine_of(state->function->owner,
                                                   operands[1].value->type);
        if ((value_machine.class != IR_CLASS_SINT
             && value_machine.class != IR_CLASS_UINT)
            || (count_machine.class != IR_CLASS_SINT
                && count_machine.class != IR_CLASS_UINT)
            || !value_machine.width) break;
        LLVMTypeRef value_type = LLVMTypeOf(left);
        LLVMTypeRef count_type = LLVMTypeOf(right);
        unsigned value_width = LLVMGetIntTypeWidth(value_type);
        unsigned count_width = LLVMGetIntTypeWidth(count_type);
        if (instruction->n_mods) {
            if (count_machine.class == IR_CLASS_SINT)
                emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntSLT,
                    right, LLVMConstNull(count_type), "shift.negative"));
            unsigned check_width = count_width > 32 ? count_width : 32;
            LLVMTypeRef check_type = LLVMIntTypeInContext(
                LLVMGetModuleContext(state->module), check_width);
            LLVMValueRef checked_count = count_width == check_width
                ? right : LLVMBuildZExt(state->builder, right, check_type,
                                       "shift.count.wide");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntUGE,
                checked_count, LLVMConstInt(check_type, value_machine.width,
                                            false), "shift.range"));
        }
        LLVMValueRef amount = count_width == value_width ? right
            : LLVMBuildIntCast2(state->builder, right, value_type, false,
                                "shift.amount");
        result = instruction->op == IR_OP_SHL
            ? LLVMBuildShl(state->builder, left, amount, "shift.left")
            : value_machine.class == IR_CLASS_SINT
                ? LLVMBuildAShr(state->builder, left, amount, "shift.right")
                : LLVMBuildLShr(state->builder, left, amount, "shift.right");
        if (instruction->op == IR_OP_SHL && instruction->n_mods) {
            LLVMValueRef restored = value_machine.class == IR_CLASS_SINT
                ? LLVMBuildAShr(state->builder, result, amount,
                                "shift.restored")
                : LLVMBuildLShr(state->builder, result, amount,
                                "shift.restored");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                restored, left, "shift.overflow"));
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_EQ:
    case IR_OP_NE:
    case IR_OP_LT:
    case IR_OP_LE:
    case IR_OP_GT:
    case IR_OP_GE: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = lowered_value(state, operands[1].value);
        if (!left || !right || LLVMTypeOf(left) != LLVMTypeOf(right)) break;
        LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(left));
        if (kind == LLVMDoubleTypeKind) {
            LLVMRealPredicate predicate = instruction->op == IR_OP_EQ
                ? LLVMRealOEQ : instruction->op == IR_OP_NE
                    ? LLVMRealUNE : instruction->op == IR_OP_LT
                        ? LLVMRealOLT : instruction->op == IR_OP_LE
                            ? LLVMRealOLE : instruction->op == IR_OP_GT
                                ? LLVMRealOGT : LLVMRealOGE;
            result = LLVMBuildFCmp(state->builder, predicate, left, right,
                                   "compare");
        } else if (kind == LLVMIntegerTypeKind
                   || kind == LLVMPointerTypeKind) {
            if (kind == LLVMPointerTypeKind
                && instruction->op != IR_OP_EQ
                && instruction->op != IR_OP_NE) break;
            ir_machine_t machine = ir_machine_of(state->function->owner,
                operands[0].value->type);
            bool signed_integer = machine.class == IR_CLASS_SINT;
            LLVMIntPredicate predicate = instruction->op == IR_OP_EQ
                ? LLVMIntEQ : instruction->op == IR_OP_NE
                    ? LLVMIntNE : instruction->op == IR_OP_LT
                        ? signed_integer ? LLVMIntSLT : LLVMIntULT
                    : instruction->op == IR_OP_LE
                        ? signed_integer ? LLVMIntSLE : LLVMIntULE
                    : instruction->op == IR_OP_GT
                        ? signed_integer ? LLVMIntSGT : LLVMIntUGT
                        : signed_integer ? LLVMIntSGE : LLVMIntUGE;
            result = LLVMBuildICmp(state->builder, predicate, left, right,
                                   "compare");
        } else break;
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
        ir_func_t *target = operands[0].value->owner;
        ir_annot_t *native = target
            ? ir_annot_get(&target->annots, "n00bb.abi.fd_write") : nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s) {
            if (count != 3 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMValueRef fd = lowered_value(state, operands[1].value);
            LLVMValueRef string = lowered_value(state, operands[2].value);
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            if (!fd || !string || LLVMTypeOf(fd) != i32
                || LLVMGetTypeKind(LLVMTypeOf(string)) != LLVMStructTypeKind
                || LLVMCountStructElementTypes(LLVMTypeOf(string)) != 2)
                break;
            LLVMTypeRef parameters[] = {i32, i64, i64};
            LLVMTypeRef signature = LLVMFunctionType(i32, parameters,
                3, false);
            LLVMValueRef writer = LLVMGetNamedFunction(state->module,
                native->s);
            if (!writer)
                writer = LLVMAddFunction(state->module,
                    native->s, signature);
            if (!writer) break;
            LLVMValueRef arguments[] = {
                fd,
                LLVMBuildExtractValue(state->builder, string, 0,
                                      "fd_write.low"),
                LLVMBuildExtractValue(state->builder, string, 1,
                                      "fd_write.high"),
            };
            result = LLVMBuildCall2(state->builder, signature, writer,
                arguments, 3, "fd_write");
            return result && save_result(state, instruction, result);
        }
        native = target
            ? ir_annot_get(&target->annots, "n00bb.abi.repr_default") : nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s) {
            if (count != 3 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMValueRef box = lowered_value(state, operands[1].value);
            LLVMValueRef fd = lowered_value(state, operands[2].value);
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            if (!box || !fd || LLVMTypeOf(box) != box_llvm_type(context)
                || LLVMTypeOf(fd) != LLVMInt32TypeInContext(context)) break;
            LLVMValueRef dispatch = repr_dispatch_bridge(state->module);
            if (!dispatch) break;
            LLVMTypeRef parameters[] = {
                LLVMInt32TypeInContext(context),
                LLVMInt16TypeInContext(context),
                LLVMInt16TypeInContext(context),
                LLVMInt32TypeInContext(context),
                LLVMInt64TypeInContext(context),
                LLVMPointerTypeInContext(context, 0),
            };
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMInt32TypeInContext(context), parameters, 6, false);
            LLVMValueRef writer = LLVMGetNamedFunction(state->module,
                                                        native->s);
            if (!writer) writer = LLVMAddFunction(state->module, native->s,
                                                  signature);
            if (!writer) break;
            LLVMValueRef arguments[] = {
                fd,
                LLVMBuildExtractValue(state->builder, box, 0, "repr.extent"),
                LLVMBuildExtractValue(state->builder, box, 1, "repr.stride"),
                LLVMBuildExtractValue(state->builder, box, 2, "repr.type"),
                LLVMBuildExtractValue(state->builder, box, 3, "repr.payload"),
                dispatch,
            };
            result = LLVMBuildCall2(state->builder, signature, writer,
                                    arguments, 6, "repr.default");
            return result && save_result(state, instruction, result);
        }
        native = target
            ? ir_annot_get(&target->annots, "n00bb.abi.native") : nullptr;
        LLVMValueRef callee = nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s) {
            LLVMTypeRef signature = function_type(state->module, target);
            if (!signature) break;
            callee = LLVMGetNamedFunction(state->module, native->s);
            if (!callee)
                callee = LLVMAddFunction(state->module, native->s,
                                         signature);
            if (!callee || LLVMGlobalGetValueType(callee) != signature) break;
        } else callee = lowered_value(state, operands[0].value);
        if (!callee || LLVMGetTypeKind(LLVMGlobalGetValueType(callee))
                           != LLVMFunctionTypeKind
            || LLVMCountParamTypes(LLVMGlobalGetValueType(callee))
                           != count - 1) break;
        LLVMValueRef *arguments = count > 1
            ? calloc(count - 1, sizeof(*arguments)) : nullptr;
        if (count > 1 && !arguments) break;
        bool valid = true;
        uint32_t output_ordinal = 0;
        for (uint32_t i = 1; valid && i < count; i++) {
            const th_param *formal = target
                ? th_func_param(target->type, i - 1) : nullptr;
            bool fresh = operands[i].kind == IR_OPD_FRESH;
            valid = formal && (formal->mode == TH_OUT
                ? operands[i].kind == IR_OPD_OUT
                : operands[i].kind == IR_OPD_VALUE
                    || (fresh && formal->mode == TH_INOUT));
            if (!valid) break;
            if (th_mode_produces_output(formal->mode)) {
                LLVMTypeRef type = value_type(state->function->owner,
                    formal->type, LLVMGetModuleContext(state->module));
                LLVMValueRef storage = nullptr;
                if (fresh) {
                    th_type *referent = operands[i].value
                        ? th_ind_referent(operands[i].value->type) : nullptr;
                    storage = referent && type
                        && ir_type_eq(state->function->owner, referent,
                                      formal->type)
                        ? lowered_value(state, operands[i].value) : nullptr;
                } else if (type) storage = entry_alloca(state, type);
                if (storage && formal->mode == TH_INOUT && !fresh) {
                    LLVMValueRef input = lowered_value(state,
                                                       operands[i].value);
                    if (!input || LLVMTypeOf(input) != type) storage = nullptr;
                    else if (!store_gc_barrier(state, input, formal->type))
                        storage = nullptr;
                    else LLVMBuildStore(state->builder, input, storage);
                }
                valid = storage && save_call_output(state, instruction,
                    output_ordinal++, storage);
                arguments[i - 1] = storage;
            } else arguments[i - 1] = lowered_value(state,
                                                      operands[i].value);
            valid = valid && arguments[i - 1];
            if (valid) {
                LLVMValueRef parameter = LLVMGetParam(callee, i - 1);
                LLVMTypeRef expected = parameter
                    ? LLVMTypeOf(parameter) : nullptr;
                if (!th_mode_produces_output(formal->mode)
                    && expected == box_llvm_type(LLVMGetModuleContext(
                        state->module))
                    && LLVMTypeOf(arguments[i - 1]) != expected)
                    arguments[i - 1] = box_value(state, arguments[i - 1],
                                                  operands[i].value->type);
                valid = arguments[i - 1]
                    && (!expected || LLVMTypeOf(arguments[i - 1]) == expected);
            }
        }
        bool returns_void = LLVMGetTypeKind(LLVMGetReturnType(
            LLVMGlobalGetValueType(callee))) == LLVMVoidTypeKind;
        if (valid)
            result = LLVMBuildCall2(state->builder,
                LLVMGlobalGetValueType(callee), callee, arguments,
                count - 1, instruction->dest && !returns_void ? "call" : "");
        free(arguments);
        if (!valid || !result) break;
        return instruction->dest && !returns_void
            ? save_result(state, instruction, result) : true;
    }
    case IR_OP_CALL_OUTPUT: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_CONST
            || operands[1].lit.is_float || operands[1].lit.i.neg
            || operands[1].lit.i.mag > UINT32_MAX
            || !instruction->dest) break;
        const ir_instr_t *call = operands[0].value
            ? operands[0].value->def : nullptr;
        if (call && call->op == IR_OP_VARIANT_PAYLOAD)
            call = call->n_operands && call->operands[0].kind == IR_OPD_VALUE
                && call->operands[0].value
                ? call->operands[0].value->def : nullptr;
        if (!call || call->op != IR_OP_CALL) break;
        for (size_t i = 0; i < state->call_output_count; i++) {
            lowered_call_output_t *output = &state->call_outputs[i];
            if (output->call != call
                || output->ordinal != operands[1].lit.i.mag) continue;
            LLVMTypeRef type = value_type(state->function->owner,
                instruction->dest->type, LLVMGetModuleContext(state->module));
            if (!type) break;
            result = LLVMBuildLoad2(state->builder, type, output->storage,
                                    "copyout");
            return save_result(state, instruction, result);
        }
        break;
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
    {
        th_type *success = th_func_ret(state->function->type);
        bool has_value = success && ir_machine_of(state->function->owner,
            success).class != IR_CLASS_VOID;
        uint32_t first_output = has_value ? 1 : 0;
        uint32_t output_count = 0;
        for (size_t i = 0; i < th_func_nparams(state->function->type); i++) {
            const th_param *formal = th_func_param(state->function->type, i);
            if (formal && th_mode_produces_output(formal->mode))
                output_count++;
        }
        if (count != first_output + output_count) break;
        uint32_t next_output = first_output;
        bool outputs_valid = true;
        for (size_t i = 0; i < th_func_nparams(state->function->type); i++) {
            const th_param *formal = th_func_param(state->function->type, i);
            if (!formal || !th_mode_produces_output(formal->mode)) continue;
            if (operands[next_output].kind != IR_OPD_VALUE) {
                outputs_valid = false;
                break;
            }
            LLVMValueRef value = lowered_value(state,
                operands[next_output++].value);
            LLVMTypeRef expected = value_type(state->function->owner,
                formal->type, LLVMGetModuleContext(state->module));
            if (!value || !expected || LLVMTypeOf(value) != expected) {
                outputs_valid = false;
                break;
            }
            if (!store_gc_barrier(state, value, formal->type)) {
                outputs_valid = false;
                break;
            }
            LLVMBuildStore(state->builder, value,
                LLVMGetParam(state->target_function, (unsigned)i));
        }
        if (!outputs_valid || next_output != count) break;
        if (has_value) {
            if (operands[0].kind != IR_OPD_VALUE) break;
            result = lowered_value(state, operands[0].value);
            if (!result) break;
        }
        if (th_func_error(state->function->type)
            && ir_machine_of(state->function->owner,
                th_func_error(state->function->type)).class != IR_CLASS_VOID) {
            return emit_outcome_return(state, result, false);
        }
        if (!has_value) {
            LLVMBuildRetVoid(state->builder);
            return true;
        }
        if (LLVMTypeOf(result) != LLVMGetReturnType(
                LLVMGlobalGetValueType(state->target_function))) break;
        LLVMBuildRet(state->builder, result);
        return true;
    }
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
        LLVMTypeRef value = parameter
            ? value_type(function->owner, parameter->type, context)
            : nullptr;
        if (!value || LLVMGetTypeKind(value)
                                == LLVMVoidTypeKind) {
            free(parameters);
            return nullptr;
        }
        parameters[i] = th_mode_produces_output(parameter->mode)
            ? LLVMPointerTypeInContext(context, 0) : value;
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
               const n00bb_llvm_global_binding_t *global_bindings,
               size_t global_binding_count,
               core_list_t(layout_match_t) *layout_matches,
               core_list_t(member_match_t) *member_matches,
               core_diagnostics_t *diagnostics)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    core_buffer_t symbol = {0};
    core_buffer_printf(&symbol, "%s.%s", ir_module_name(function->owner),
        function->name);
    LLVMValueRef target_function = LLVMGetNamedFunction(target, symbol.data);
    core_buffer_release(&symbol);
    size_t input_count = 0;
    for (size_t i = 0; i < th_func_nparams(function->type); i++) {
        const th_param *formal = th_func_param(function->type, i);
        if (formal && th_mode_accepts_input(formal->mode)) input_count++;
    }
    if (!target_function || !function->n_blocks
        || function->n_params != input_count
        || th_func_nparams(function->type)
            != LLVMCountParams(target_function)) {
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
        .type_index_count = type_index_count,
        .global_bindings = global_bindings,
        .global_binding_count = global_binding_count,
        .layout_matches = layout_matches,
        .member_matches = member_matches};
    state.blocks = calloc(function->n_blocks, sizeof(*state.blocks));
    state.block_tails = calloc(function->n_blocks,
                               sizeof(*state.block_tails));
    state.locals = calloc(function->n_locals ? function->n_locals : 1,
        sizeof(*state.locals));
    for (uint32_t i = 0; i < function->n_blocks; i++)
        state.value_capacity += function->blocks[i]->n_instrs;
    state.values = calloc(state.value_capacity ? state.value_capacity : 1,
        sizeof(*state.values));
    state.builder = LLVMCreateBuilderInContext(context);
    bool ok = state.blocks && state.block_tails && state.locals
        && state.values && state.builder;
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
        for (uint32_t j = 0; ok && j < block->n_instrs; j++) {
            ir_instr_t *instruction = block->instrs[j];
            ok = lower_instruction(&state, instruction);
            if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics))) {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "could not lower '%s' in function '%s'",
                    ir_op_spelling(instruction->op), function->name);
                backend_error(diagnostics, message.data);
                core_buffer_release(&message);
            }
        }
        state.block_tails[i] = LLVMGetInsertBlock(state.builder);
    }
    for (uint32_t i = 0; ok && i < function->n_blocks; i++) {
        ir_block_t *block = function->blocks[i];
        for (uint32_t j = 0; ok && j < block->n_instrs; j++) {
            ir_instr_t *instruction = block->instrs[j];
            if (instruction->op != IR_OP_PHI) continue;
            LLVMValueRef phi = lowered_value(&state, instruction->dest);
            for (uint32_t k = 0; phi && k < instruction->n_operands; k++) {
                ir_operand_t *operand = &instruction->operands[k];
                LLVMValueRef value = operand->kind == IR_OPD_PHI_ARM
                    ? lowered_value(&state, operand->phi.value) : nullptr;
                LLVMBasicBlockRef predecessor = operand->kind == IR_OPD_PHI_ARM
                    ? lowered_block(&state, operand->phi.block) : nullptr;
                if (predecessor)
                    for (uint32_t p = 0; p < function->n_blocks; p++)
                        if (function->blocks[p] == operand->phi.block) {
                            predecessor = state.block_tails[p];
                            break;
                        }
                if (!value || !predecessor) { ok = false; break; }
                LLVMAddIncoming(phi, &value, &predecessor, 1);
            }
            if (!phi) ok = false;
        }
    }
    if (state.builder) LLVMDisposeBuilder(state.builder);
    free(state.blocks);
    free(state.block_tails);
    free(state.locals);
    free(state.values);
    free(state.call_outputs);
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
             const n00bb_llvm_global_binding_t *global_bindings,
             size_t global_binding_count,
             ir_func_t *const *reachable, size_t reachable_count,
             bool reachable_bound,
             core_diagnostics_t *diagnostics)
{
    for (size_t i = 0; i < ir_global_count(source); i++) {
        ir_global_t *global = ir_global_at((ir_module_t *)source, i);
        LLVMTypeRef type = value_type((ir_module_t *)source, global->type,
                                     LLVMGetModuleContext(target));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind)
            return backend_error(diagnostics,
                "LLVM backend cannot lower this global type yet");
        LLVMValueRef declared = global_ref(target, global,
            global_bindings, global_binding_count);
        if (!declared)
            return backend_error(diagnostics,
                "LLVM backend could not declare global");
        LLVMSetThreadLocal(declared,
            ir_global_storage_duration(global) == IR_STORAGE_DURATION_THREAD);
        if (global->is_extern) continue;
        bool abi_backed = false;
        for (size_t j = 0; j < global_binding_count; j++)
            if (global_bindings[j].global == ir_global_identity(global)) {
                abi_backed = true;
                break;
            }
        if (abi_backed) continue;
        const ir_global_init_value_t *init = global->static_initializer;
        th_type *atomic_element = atomic_element_type(global->type);
        ir_machine_t machine = ir_machine_of((ir_module_t *)source,
            atomic_element ? atomic_element : global->type);
        LLVMValueRef value = nullptr;
        if (!global->has_static_initializer
            || (init && init->kind == IR_GLOBAL_INIT_NIL))
            value = LLVMConstNull(type);
        else if (init) {
            if (init->kind == IR_GLOBAL_INIT_INT)
                value = integer_constant(machine, type, init->integer);
            else if (init->kind == IR_GLOBAL_INIT_BOOL)
                value = integer_constant(machine, type,
                    (ir_lit_t){.mag = init->boolean});
            else if (init->kind == IR_GLOBAL_INIT_FLOAT
                     && machine.class == IR_CLASS_FLOAT && machine.width == 64)
                value = LLVMConstReal(type, init->floating);
            else if (init->kind == IR_GLOBAL_INIT_STRING
                     && is_string_type(global->type))
                value = string_value(target,
                    (const uint8_t *)init->string.bytes, init->string.length,
                    default_string_encoding(init->string.bytes,
                                            init->string.length));
        }
        if (!value)
            return backend_error(diagnostics,
                "LLVM backend cannot lower this global initializer yet");
        LLVMSetInitializer(declared, value);
        LLVMSetGlobalConstant(declared, global->mutability == IR_MUT_CONST
            && global->has_static_initializer);
        if (global->is_static)
            LLVMSetLinkage(declared, LLVMInternalLinkage);
    }
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        LLVMTypeRef signature = function_type(target, function);
        if (!signature) {
            if (!function->n_blocks) continue;
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
    core_list_t(layout_match_t) layout_matches = {0};
    core_list_t(member_match_t) member_matches = {0};
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        bool emit_body = !reachable_bound;
        for (size_t j = 0; !emit_body && j < reachable_count; j++)
            emit_body = reachable[j] == function;
        if (function->n_blocks
            && emit_body
            && !lower_function(target, function, phase_arrays,
                               phase_array_count, type_indices,
                               type_index_count, global_bindings,
                               global_binding_count, &layout_matches,
                               &member_matches,
                               diagnostics)) {
            core_list_free(layout_matches);
            core_list_free(member_matches);
            return false;
        }
    }
    core_list_free(layout_matches);
    core_list_free(member_matches);
    const char *module_name = ir_module_name(source);
    size_t prefix = strlen(module_name);
    for (size_t i = 0; i < n00bb_runtime_native_root_count; i++) {
        const n00bb_runtime_native_root_t *root = &n00bb_runtime_native_roots[i];
        const char *name = root->qualified_name;
        if (strncmp(name, module_name, prefix) || name[prefix] != '.')
            continue;
        LLVMValueRef function = LLVMGetNamedFunction(target, name);
        if (!function || LLVMIsDeclaration(function)) continue;
        if (LLVMGetNamedFunction(target, root->c_alias)
            || !LLVMAddAlias2(target, LLVMGlobalGetValueType(function), 0,
                              function, root->c_alias))
            return backend_error(diagnostics,
                "LLVM backend could not export a native runtime root");
    }
    return true;
}

static bool
set_native_layout(LLVMModuleRef module)
{
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter())
        return false;
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = nullptr;
    char *message = nullptr;
    bool ok = triple && !LLVMGetTargetFromTriple(triple, &target, &message);
    LLVMTargetMachineRef machine = ok ? LLVMCreateTargetMachine(target,
        triple, "generic", "", LLVMCodeGenLevelDefault, LLVMRelocDefault,
        LLVMCodeModelDefault) : nullptr;
    LLVMTargetDataRef layout = machine
        ? LLVMCreateTargetDataLayout(machine) : nullptr;
    char *layout_text = layout ? LLVMCopyStringRepOfTargetData(layout)
                               : nullptr;
    ok = layout_text != nullptr;
    if (ok) LLVMSetDataLayout(module, layout_text);
    if (layout_text) LLVMDisposeMessage(layout_text);
    if (layout) LLVMDisposeTargetData(layout);
    if (machine) LLVMDisposeTargetMachine(machine);
    if (message) LLVMDisposeMessage(message);
    if (triple) LLVMDisposeMessage(triple);
    return ok;
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
    if (!set_native_layout(backend->comptime)
        || !set_native_layout(backend->runtime)) goto fail;
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

bool
n00bb_llvm_backend_bind_globals(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_global_binding_t *globals, size_t count)
{
    if (!backend || (count && !globals) || backend->runtime_lowered)
        return false;
    backend->global_bindings = globals;
    backend->global_binding_count = count;
    return true;
}

bool
n00bb_llvm_backend_bind_finalizers(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_finalizer_binding_t *finalizers, size_t count)
{
    if (!backend || (count && !finalizers) || backend->runtime_lowered)
        return false;
    backend->finalizers = finalizers;
    backend->finalizer_count = count;
    return true;
}

bool
n00bb_llvm_backend_bind_reachable_functions(
    n00bb_llvm_backend_t *backend, bool comptime,
    ir_func_t *const *functions, size_t count)
{
    if (!backend || (count && !functions)
        || (comptime ? backend->comptime_lowered : backend->runtime_lowered))
        return false;
    if (comptime) {
        backend->comptime_reachable = functions;
        backend->comptime_reachable_count = count;
        backend->comptime_reachable_bound = true;
    } else {
        backend->runtime_reachable = functions;
        backend->runtime_reachable_count = count;
        backend->runtime_reachable_bound = true;
    }
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
                      comptime ? nullptr : backend->global_bindings,
                      comptime ? 0 : backend->global_binding_count,
                      comptime ? backend->comptime_reachable
                               : backend->runtime_reachable,
                      comptime ? backend->comptime_reachable_count
                               : backend->runtime_reachable_count,
                      comptime ? backend->comptime_reachable_bound
                               : backend->runtime_reachable_bound,
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

static bool
emit_finalizer_bridge(n00bb_llvm_backend_t *backend,
                      core_diagnostics_t *diagnostics)
{
    LLVMModuleRef module = backend->runtime;
    LLVMContextRef context = backend->context;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef arguments[] = {i32, pointer};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(context), arguments, 2, false);
    LLVMValueRef bridge = LLVMGetNamedFunction(module, "n00b_gc_finalize");
    if (!bridge) bridge = LLVMAddFunction(module, "n00b_gc_finalize",
                                          signature);
    if (!bridge || LLVMCountBasicBlocks(bridge))
        return backend_error(diagnostics,
            "could not define the native finalizer bridge");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context, bridge,
                                                             "entry");
    LLVMBasicBlockRef missing = LLVMAppendBasicBlockInContext(context,
                                                               bridge,
                                                               "unbound");
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef branch = LLVMBuildSwitch(builder, LLVMGetParam(bridge, 0),
        missing, (unsigned)backend->finalizer_count);
    bool valid = true;
    for (size_t i = 0; valid && i < backend->finalizer_count; i++) {
        const n00bb_llvm_finalizer_binding_t *binding =
            &backend->finalizers[i];
        ir_func_t *function = binding->function;
        LLVMTypeRef target_signature = function
            ? function_type(module, function) : nullptr;
        if (!target_signature
            || LLVMGetTypeKind(LLVMGetReturnType(target_signature))
                != LLVMVoidTypeKind
            || LLVMCountParamTypes(target_signature) != 1) {
            valid = false;
            break;
        }
        LLVMTypeRef target_parameter = nullptr;
        LLVMGetParamTypes(target_signature, &target_parameter);
        if (LLVMGetTypeKind(target_parameter) != LLVMPointerTypeKind) {
            valid = false;
            break;
        }
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s.%s",
            ir_module_name(function->owner), function->name);
        LLVMValueRef target = LLVMGetNamedFunction(module, name.data);
        if (!target) target = LLVMAddFunction(module, name.data,
                                              target_signature);
        core_buffer_release(&name);
        if (!target) { valid = false; break; }
        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(context,
            bridge, "finalize");
        LLVMAddCase(branch, LLVMConstInt(i32, binding->type_index, false),
                    body);
        LLVMPositionBuilderAtEnd(builder, body);
        LLVMValueRef payload = LLVMGetParam(bridge, 1);
        LLVMBuildCall2(builder, target_signature, target, &payload, 1, "");
        LLVMBuildRetVoid(builder);
    }
    LLVMPositionBuilderAtEnd(builder, missing);
    unsigned trap_id = LLVMLookupIntrinsicID("llvm.trap",
                                              sizeof("llvm.trap") - 1);
    LLVMValueRef trap = LLVMGetIntrinsicDeclaration(module, trap_id,
                                                     nullptr, 0);
    LLVMBuildCall2(builder, LLVMGlobalGetValueType(trap), trap, nullptr, 0,
                   "");
    LLVMBuildUnreachable(builder);
    LLVMDisposeBuilder(builder);
    return valid || backend_error(diagnostics,
        "finalizer bridge target has an invalid native signature");
}

static bool
emit_entry_initializers(LLVMModuleRef module, LLVMContextRef context,
                        LLVMValueRef entry, LLVMBuilderRef builder,
                        LLVMTypeRef result, bool comptime,
                        const char *const *names, size_t count,
                        core_diagnostics_t *diagnostics)
{
    LLVMTypeRef initializer_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context), nullptr, 0, false);
    for (size_t i = 0; i < count; i++) {
        LLVMValueRef initializer = LLVMGetNamedFunction(module, names[i]);
        if (!initializer)
            initializer = LLVMAddFunction(module, names[i],
                initializer_type);
        LLVMTypeRef initializer_result = LLVMGetReturnType(
            LLVMGlobalGetValueType(initializer));
        bool failable = comptime
            && LLVMGetTypeKind(initializer_result) == LLVMStructTypeKind
            && LLVMCountStructElementTypes(initializer_result) == 2;
        if (LLVMCountParams(initializer)
            || (!failable && initializer_result
                != LLVMVoidTypeInContext(context)))
            return backend_error(diagnostics,
                "phase initializer signature is invalid");
        LLVMValueRef outcome = LLVMBuildCall2(builder,
            LLVMGlobalGetValueType(initializer), initializer,
            nullptr, 0, "");
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
    if (!comptime && initializer_count
        && !emit_finalizer_bridge(backend, diagnostics)) return false;
    if (initializer_count || array_producer_count) {
        LLVMTypeRef result = LLVMInt32TypeInContext(context);
        LLVMTypeRef signature = LLVMFunctionType(result, nullptr, 0, false);
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
            LLVMTypeRef nested_writer_parameters[] = {
                result, pointer, pointer, result, pointer,
            };
            LLVMTypeRef nested_writer_type = LLVMFunctionType(result,
                nested_writer_parameters, 5, false);
            LLVMValueRef nested_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_nested_array", nested_writer_type);
            LLVMTypeRef reloc_writer_parameters[] = {
                result, pointer, pointer, result, pointer, result, pointer,
            };
            LLVMTypeRef reloc_writer_type = LLVMFunctionType(result,
                reloc_writer_parameters, 7, false);
            LLVMValueRef reloc_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_reloc_array", reloc_writer_type);
            LLVMValueRef nested_reloc_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_nested_reloc_array",
                reloc_writer_type);
            LLVMTypeRef reloc_value_parameters[] = {
                result, pointer, result, pointer, result, pointer,
            };
            LLVMTypeRef reloc_value_type = LLVMFunctionType(result,
                reloc_value_parameters, 6, false);
            LLVMValueRef reloc_value_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_reloc_value", reloc_value_type);
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
            LLVMTypeRef value_writer_parameters[] = {result, pointer,
                                                       result};
            LLVMTypeRef value_writer_type = LLVMFunctionType(result,
                value_writer_parameters, 3, false);
            LLVMValueRef value_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_value", value_writer_type);
            bool initializers_run = false;
            for (size_t i = 0; i < array_producer_count; i++) {
                const n00bb_llvm_array_producer_t *spec = &array_producers[i];
                if (spec->after_initializers && !initializers_run) {
                    if (!emit_entry_initializers(module, context, entry,
                            builder, result, comptime, entry_initializers,
                            initializer_count, diagnostics)) {
                        LLVMDisposeBuilder(builder);
                        return false;
                    }
                    initializers_run = true;
                }
                th_type *producer_result = spec->function
                    ? th_func_ret(spec->function->type) : nullptr;
                bool reloc_array = spec->kind == N00BB_LLVM_LITERAL_RELOC
                    && producer_result
                    && th_kind_of(th_resolve(producer_result)) == TH_SEQUENCE;
                LLVMTypeRef expected = spec->kind == N00BB_LLVM_LITERAL_EFFECT
                    ? LLVMVoidTypeInContext(context)
                    : spec->kind == N00BB_LLVM_LITERAL_STRING
                        ? string_type
                    : (spec->kind == N00BB_LLVM_LITERAL_ARRAY
                        || reloc_array
                        || spec->kind == N00BB_LLVM_LITERAL_NESTED_ARRAY
                        || spec->kind == N00BB_LLVM_LITERAL_NESTED_RELOC)
                        ? array_type
                    : (spec->kind == N00BB_LLVM_LITERAL_RAW_VALUE
                        || spec->kind == N00BB_LLVM_LITERAL_RELOC)
                        && spec->function
                        ? value_type(spec->function->owner,
                            producer_result, context)
                        : nullptr;
                LLVMValueRef producer = spec->symbol
                    ? LLVMGetNamedFunction(module, spec->symbol) : nullptr;
                if (!producer && spec->symbol && expected)
                    producer = LLVMAddFunction(module, spec->symbol,
                        LLVMFunctionType(expected, nullptr, 0, false));
                if (!producer || (spec->kind == N00BB_LLVM_LITERAL_ARRAY
                        && !spec->element_stride)
                    || (spec->kind == N00BB_LLVM_LITERAL_RELOC
                        && (!spec->element_stride || !spec->string_offsets
                            || !spec->byte_mask
                            || !spec->string_offset_count
                            || spec->string_offset_count > UINT32_MAX
                            || (!reloc_array && (!expected
                                || LLVMABISizeOfType(
                                    LLVMGetModuleDataLayout(module), expected)
                                    != spec->element_stride))))
                    || ((spec->kind == N00BB_LLVM_LITERAL_NESTED_ARRAY
                        || spec->kind == N00BB_LLVM_LITERAL_NESTED_RELOC)
                        && (spec->element_stride
                            != sizeof(n00b_abi_array_value_v1)
                            || !spec->inner_stride || !spec->byte_mask
                            || (spec->kind == N00BB_LLVM_LITERAL_NESTED_RELOC
                                && (!spec->string_offsets
                                    || !spec->string_offset_count))))
                    || (spec->kind == N00BB_LLVM_LITERAL_RAW_VALUE
                        && (!spec->element_stride || !expected
                            || LLVMABISizeOfType(
                                LLVMGetModuleDataLayout(module), expected)
                                != spec->element_stride))
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
                if (spec->comptime_consumer
                    && spec->kind != N00BB_LLVM_LITERAL_EFFECT) {
                    LLVMValueRef global = phase_cache_global(module,
                        spec->identifier, expected, false);
                    if (!global) {
                        LLVMDisposeBuilder(builder);
                        return backend_error(diagnostics,
                            "compile-time literal cache binding is invalid");
                    }
                    LLVMBuildStore(builder, value, global);
                }
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
                } else if (spec->kind == N00BB_LLVM_LITERAL_RAW_VALUE) {
                    LLVMValueRef storage = LLVMBuildAlloca(builder, expected,
                        "literal.value.storage");
                    LLVMBuildStore(builder, value, storage);
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        storage,
                        LLVMConstInt(result, spec->element_stride, false),
                    };
                    status = LLVMBuildCall2(builder, value_writer_type,
                        value_writer, arguments, 3, "literal.write");
                } else if (spec->kind == N00BB_LLVM_LITERAL_RELOC) {
                    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                    LLVMValueRef *offsets = calloc(spec->string_offset_count,
                                                    sizeof(*offsets));
                    if (!offsets) {
                        LLVMDisposeBuilder(builder);
                        return backend_error(diagnostics,
                            "literal relocation offsets are unavailable");
                    }
                    for (size_t f = 0; f < spec->string_offset_count; f++)
                        offsets[f] = LLVMConstInt(i32,
                            spec->string_offsets[f], false);
                    LLVMValueRef offset_values = LLVMConstArray(i32, offsets,
                        (unsigned)spec->string_offset_count);
                    free(offsets);
                    LLVMValueRef offset_data = LLVMAddGlobal(module,
                        LLVMTypeOf(offset_values),
                        "n00bb.literal.string.offsets");
                    LLVMSetInitializer(offset_data, offset_values);
                    LLVMSetGlobalConstant(offset_data, true);
                    LLVMSetLinkage(offset_data, LLVMPrivateLinkage);
                    LLVMValueRef mask_values = LLVMConstStringInContext(
                        context, (const char *)spec->byte_mask,
                        spec->element_stride, true);
                    LLVMValueRef mask_data = LLVMAddGlobal(module,
                        LLVMTypeOf(mask_values), "n00bb.literal.byte.mask");
                    LLVMSetInitializer(mask_data, mask_values);
                    LLVMSetGlobalConstant(mask_data, true);
                    LLVMSetLinkage(mask_data, LLVMPrivateLinkage);
                    if (reloc_array) {
                        LLVMValueRef arguments[] = {
                            LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                         false),
                            LLVMBuildExtractValue(builder, value, 0,
                                                  "array.data"),
                            LLVMBuildExtractValue(builder, value, 1,
                                                  "array.descriptor"),
                            LLVMConstInt(result, spec->element_stride,
                                         false),
                            offset_data,
                            LLVMConstInt(result, spec->string_offset_count,
                                         false),
                            mask_data,
                        };
                        status = LLVMBuildCall2(builder, reloc_writer_type,
                            reloc_writer, arguments, 7, "literal.write");
                    } else {
                        LLVMValueRef storage = LLVMBuildAlloca(builder,
                            expected, "literal.reloc.storage");
                        LLVMBuildStore(builder, value, storage);
                        LLVMValueRef arguments[] = {
                            LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                         false),
                            storage,
                            LLVMConstInt(result, spec->element_stride,
                                         false),
                            offset_data,
                            LLVMConstInt(result, spec->string_offset_count,
                                         false),
                            mask_data,
                        };
                        status = LLVMBuildCall2(builder, reloc_value_type,
                            reloc_value_writer, arguments, 6,
                            "literal.write");
                    }
                } else if (spec->kind == N00BB_LLVM_LITERAL_NESTED_ARRAY
                    || spec->kind == N00BB_LLVM_LITERAL_NESTED_RELOC) {
                    LLVMValueRef mask_values = LLVMConstStringInContext(
                        context, (const char *)spec->byte_mask,
                        spec->inner_stride, true);
                    LLVMValueRef mask_data = LLVMAddGlobal(module,
                        LLVMTypeOf(mask_values), "n00bb.literal.inner.mask");
                    LLVMSetInitializer(mask_data, mask_values);
                    LLVMSetGlobalConstant(mask_data, true);
                    LLVMSetLinkage(mask_data, LLVMPrivateLinkage);
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        LLVMBuildExtractValue(builder, value, 0,
                                              "array.data"),
                        LLVMBuildExtractValue(builder, value, 1,
                                              "array.descriptor"),
                        LLVMConstInt(result, spec->inner_stride, false),
                        mask_data,
                    };
                    if (spec->kind == N00BB_LLVM_LITERAL_NESTED_RELOC) {
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                        LLVMValueRef *offsets = calloc(
                            spec->string_offset_count, sizeof(*offsets));
                        if (!offsets) {
                            LLVMDisposeBuilder(builder);
                            return backend_error(diagnostics,
                                "could not build nested string offsets");
                        }
                        for (size_t j = 0; j < spec->string_offset_count; j++)
                            offsets[j] = LLVMConstInt(i32,
                                spec->string_offsets[j], false);
                        LLVMValueRef offset_values = LLVMConstArray(i32,
                            offsets, (unsigned)spec->string_offset_count);
                        free(offsets);
                        LLVMValueRef offset_data = LLVMAddGlobal(module,
                            LLVMTypeOf(offset_values),
                            "n00bb.literal.nested.offsets");
                        LLVMSetInitializer(offset_data, offset_values);
                        LLVMSetGlobalConstant(offset_data, true);
                        LLVMSetLinkage(offset_data, LLVMPrivateLinkage);
                        LLVMValueRef reloc_arguments[] = {
                            arguments[0], arguments[1], arguments[2],
                            arguments[3], offset_data,
                            LLVMConstInt(result,
                                spec->string_offset_count, false),
                            mask_data,
                        };
                        status = LLVMBuildCall2(builder, reloc_writer_type,
                            nested_reloc_writer, reloc_arguments, 7,
                            "literal.write");
                    }
                    else status = LLVMBuildCall2(builder,
                        nested_writer_type, nested_writer, arguments, 5,
                        "literal.write");
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
        if (!comptime || !array_producer_count
            || !array_producers[array_producer_count - 1].after_initializers)
            if (!emit_entry_initializers(module, context, entry, builder,
                    result, comptime, entry_initializers,
                    initializer_count, diagnostics)) {
                LLVMDisposeBuilder(builder);
                return false;
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
