#include "llvm_backend.h"

#include "ir/layout.h"
#include "ir/shape.h"
#include "ir/typelang.h"
#include "abi_string.h"
#include "frame_roots.h"
#include "llvm_stack_maps.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
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
    char *cpu_name;
    char *cpu_features;
    const n00bb_llvm_phase_array_t *phase_arrays;
    size_t phase_array_count;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
    const n00bb_llvm_function_binding_t *function_indices;
    size_t function_index_count;
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
    ir_storage_origin_t *storage_origin;
    bool emit_frame_maps;
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

typedef ir_func_t *inline_function_ptr_t;
core_list_decl(inline_function_ptr_t);
core_list_decl(LLVMValueRef);

static const char gc_root_init_metadata[] = "n00bb.gc.root.init";

static bool
inline_list_contains(const core_list_t(inline_function_ptr_t) *functions,
                     const ir_func_t *function)
{
    for (size_t i = 0; i < functions->len; i++)
        if (functions->data[i] == function) return true;
    return false;
}

static ir_func_t *
direct_call_target(const ir_instr_t *instruction)
{
    if (instruction->op != IR_OP_CALL || !instruction->n_operands
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || instruction->operands[0].value->kind != IR_VAL_FUNC)
        return nullptr;
    return instruction->operands[0].value->owner;
}

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    ir_func_t *function;
    const ir_module_t *source_module;
    LLVMValueRef target_function;
    LLVMBasicBlockRef *blocks;
    LLVMBasicBlockRef *block_tails;
    LLVMValueRef *locals;
    LLVMValueRef mutator;
    const core_list_t(inline_function_ptr_t) *mutator_functions;
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
    const n00bb_llvm_function_binding_t *function_indices;
    size_t function_index_count;
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    ir_storage_origin_t *storage_origin;
    core_indexed_table_t *frame_roots;
    LLVMValueRef *frame_slots;
    size_t frame_slot_count;
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
    core_buffer_printf(&message,
        "LLVM backend cannot lower '%s' in '%s.%s' yet",
        ir_op_spelling(instruction->op),
        ir_module_name(state->function->owner), state->function->name);
    ir_func_t *callee = direct_call_target(instruction);
    if (callee)
        core_buffer_printf(&message, " (callee '%s.%s')",
            ir_module_name(callee->owner), callee->name);
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
    return n00bb_frame_pointer_option(type);
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
    if (type && !definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
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
    if (definition && definition != th_resolve(type)
        && th_kind_of(definition) != TH_RECORD
        && th_kind_of(definition) != TH_SUM)
        return value_type(module, definition, context);
    LLVMTypeRef scalar = scalar_type(module, type, context);
    if (scalar) return scalar;
    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE) {
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef fields[] = {pointer, pointer};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    const char *name = nullptr;
    size_t arguments = 0;
    bool variant = false;
    if ((!th_named_info(th_resolve(type), &name, nullptr,
                       &variant, &arguments) || !name)
        && (!definition || th_kind_of(definition) != TH_RECORD))
        return nullptr;
    if (!name) name = "";
    if (!strcmp(name, "any") && !arguments)
        return box_llvm_type(context);
    if (variant) {
        const ir_final_layout_t *layout = ir_layout_final(module, type,
            ir_module_target(module));
        uint64_t stride_bits = 0;
        if (!layout || !layout->alignment_bytes
            || layout->alignment_bytes > UINT_MAX / 8
            || !ir_layout_final_stride_bits(layout, &stride_bits)
            || !stride_bits) return nullptr;
        uint64_t unit_bits = (uint64_t)layout->alignment_bytes * 8;
        if (stride_bits % unit_bits || stride_bits / unit_bits > UINT32_MAX)
            return nullptr;
        LLVMTypeRef unit = LLVMIntTypeInContext(context,
            (unsigned)unit_bits);
        return LLVMArrayType(unit, (unsigned)(stride_bits / unit_bits));
    }
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
native_outcome_type(ir_module_t *module, th_type *success,
                    th_type *failure, LLVMContextRef context)
{
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

static LLVMTypeRef
native_result_type(ir_module_t *module, th_type *signature,
                   LLVMContextRef context)
{
    return signature ? native_outcome_type(module, th_func_ret(signature),
        th_func_error(signature), context) : nullptr;
}

static LLVMTypeRef function_type(LLVMModuleRef target, ir_func_t *function);
static LLVMTypeRef mutator_function_type(LLVMModuleRef target,
                                        ir_func_t *function);
static LLVMValueRef mutator_function(LLVMModuleRef target,
                                    ir_func_t *function);
static LLVMTypeRef native_function_type(LLVMModuleRef target,
                                        ir_module_t *owner,
                                        th_type *signature);
static LLVMTypeRef optional_record_type(LLVMModuleRef target,
                                        ir_func_t *function);
static void emit_trap_if(lower_state_t *state, LLVMValueRef condition);

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
global_initialized_ref(LLVMModuleRef module, const ir_global_t *global,
                       const n00bb_llvm_global_binding_t *bindings,
                       size_t count)
{
    LLVMValueRef storage = global_ref(module, global, bindings, count);
    if (!storage) return nullptr;
    size_t length = 0;
    const char *name = LLVMGetValueName2(storage, &length);
    core_buffer_t key = {0};
    core_buffer_printf(&key, "__n00bb_initialized.%.*s", (int)length, name);
    LLVMValueRef flag = LLVMGetNamedGlobal(module, key.data);
    if (!flag)
        flag = LLVMAddGlobal(module,
            LLVMInt8TypeInContext(LLVMGetModuleContext(module)), key.data);
    core_buffer_release(&key);
    return flag;
}

static bool
guard_global_read(lower_state_t *state, const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    if (ir_func_execution_phase(state->function) != IR_PHASE_RUNTIME
        || !identity || identity->is_extern
        || identity->has_static_initializer) return true;
    LLVMValueRef flag = global_initialized_ref(state->module, global,
        state->global_bindings, state->global_binding_count);
    if (!flag) return false;
    LLVMValueRef ready = LLVMBuildLoad2(state->builder,
        LLVMGlobalGetValueType(flag), flag, "global.initialized");
    LLVMSetOrdering(ready, LLVMAtomicOrderingAcquire);
    LLVMValueRef uninitialized = LLVMBuildICmp(state->builder, LLVMIntEQ,
        ready, LLVMConstNull(LLVMTypeOf(ready)), "global.uninitialized");
    emit_trap_if(state, uninitialized);
    return true;
}

static bool
mark_global_initialized(lower_state_t *state, const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    if (ir_func_execution_phase(state->function) != IR_PHASE_RUNTIME
        || !identity || identity->is_extern) return true;
    LLVMValueRef flag = global_initialized_ref(state->module, global,
        state->global_bindings, state->global_binding_count);
    if (!flag) return false;
    LLVMValueRef ready = LLVMBuildStore(state->builder,
        LLVMConstInt(LLVMGlobalGetValueType(flag), 1, false), flag);
    LLVMSetOrdering(ready, LLVMAtomicOrderingRelease);
    return true;
}

static const ir_global_t *
direct_global_address(const ir_value_t *address)
{
    while (address && address->kind == IR_VAL_INSTR && address->def) {
        const ir_instr_t *definition = address->def;
        if (definition->op == IR_OP_FIELD) {
            if (definition->n_operands != 2
                || definition->operands[1].kind != IR_OPD_NAME)
                break;
            address = ir_operand_value(&definition->operands[0]);
            continue;
        }
        if (definition->n_operands != 1) break;
        if (definition->op == IR_OP_ADDRESS_OF) {
            ir_value_t *target = ir_operand_value(&definition->operands[0]);
            return target && target->kind == IR_VAL_GLOBAL
                ? target->global : nullptr;
        }
        if (definition->op != IR_OP_COPY
            && definition->op != IR_OP_CAST
            && definition->op != IR_OP_BITCAST
            && definition->op != IR_OP_NOMINAL_CAST) break;
        address = ir_operand_value(&definition->operands[0]);
    }
    return nullptr;
}

static bool
direct_stack_address(const ir_func_t *function, const ir_value_t *address)
{
    while (address && address->kind == IR_VAL_INSTR && address->def) {
        const ir_instr_t *definition = address->def;
        if (definition->op == IR_OP_ALLOCA) return true;
        if (definition->op == IR_OP_ALLOC)
            return ir_allocation_has_only_local_uses(function, definition);
        if (definition->op == IR_OP_FIELD) {
            if (definition->n_operands != 2
                || definition->operands[1].kind != IR_OPD_NAME)
                return false;
        } else if (definition->n_operands != 1) return false;
        if (definition->operands[0].kind != IR_OPD_VALUE) return false;
        const ir_value_t *base = definition->operands[0].value;
        if (definition->op == IR_OP_ADDRESS_OF)
            return base && base->kind == IR_VAL_LOCAL;
        if (definition->op != IR_OP_FIELD
            && definition->op != IR_OP_COPY
            && definition->op != IR_OP_CAST
            && definition->op != IR_OP_BITCAST
            && definition->op != IR_OP_NOMINAL_CAST) return false;
        address = base;
    }
    return false;
}

typedef struct {
    lower_state_t *state;
    bool valid;
} global_load_guard_t;

static bool
guard_global_load_origin(void *context, ir_value_t *origin)
{
    global_load_guard_t *guard = context;
    if (origin->kind == IR_VAL_GLOBAL && origin->global)
        guard->valid = guard_global_read(guard->state, origin->global);
    return guard->valid;
}

static LLVMValueRef
frozen_scalar_global(lower_state_t *state, const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    if (ir_func_execution_phase(state->function) != IR_PHASE_RUNTIME
        || !identity || identity->is_extern
        || identity->mutability != IR_MUT_CONST
        || ir_global_storage_duration(identity) == IR_STORAGE_DURATION_THREAD
        || ir_global_can_write(identity, IR_PHASE_RUNTIME)
        || ir_global_can_initialize(identity, IR_PHASE_RUNTIME)
        || !identity->has_static_initializer
        || !identity->static_initializer
        || atomic_element_type(identity->type)) return nullptr;
    const ir_global_init_value_t *init = identity->static_initializer;
    LLVMTypeRef type = value_type(state->function->owner, global->type,
                                 LLVMGetModuleContext(state->module));
    ir_machine_t machine = ir_machine_of(state->function->owner,
                                         global->type);
    if (!type) return nullptr;
    switch (init->kind) {
    case IR_GLOBAL_INIT_INT:
        return integer_constant(machine, type, init->integer);
    case IR_GLOBAL_INIT_BOOL:
        return integer_constant(machine, type,
                                (ir_lit_t){.mag = init->boolean});
    case IR_GLOBAL_INIT_FLOAT:
        return machine.class == IR_CLASS_FLOAT
            ? LLVMConstReal(type, init->floating) : nullptr;
    default:
        return nullptr;
    }
}

static LLVMValueRef
lowered_value(lower_state_t *state, const ir_value_t *value)
{
    if (!value) return nullptr;
    if (value->kind == IR_VAL_PARAM && value->owner == state->function
        && value->index < state->function->n_params) {
        size_t input_index = 0, formal_index = 0;
        size_t optional_index = 0;
        const th_param *formal = nullptr;
        for (; formal_index < th_func_nparams(state->function->type);
             formal_index++) {
            const th_param *candidate = th_func_param(state->function->type,
                                                        formal_index);
            bool optional = ir_func_parameter_is_optional(state->function,
                                                            formal_index);
            if (!candidate || !th_mode_accepts_input(candidate->mode))
                continue;
            if (input_index++ == value->index) {
                formal = candidate;
                break;
            }
            if (optional) optional_index++;
        }
        if (!formal) return nullptr;
        if (ir_func_parameter_is_optional(state->function, formal_index)) {
            LLVMTypeRef record_type = optional_record_type(state->module,
                                                            state->function);
            unsigned last = LLVMCountParams(state->target_function);
            LLVMTypeRef type = value_type(state->function->owner, value->type,
                LLVMGetModuleContext(state->module));
            if (!record_type || !last || !type) return nullptr;
            LLVMValueRef record = LLVMGetParam(state->target_function,
                                               last - 1);
            LLVMValueRef field = LLVMBuildStructGEP2(state->builder,
                record_type, record, (unsigned)optional_index + 1,
                "optional.value.address");
            return LLVMBuildLoad2(state->builder, type, field,
                                  "optional.value");
        }
        if (formal_index >= LLVMCountParams(state->target_function))
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
        if (!function && value->owner->owner != state->source_module) {
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
        LLVMValueRef frozen = frozen_scalar_global(state, value->global);
        if (frozen) return frozen;
        LLVMValueRef global = global_ref(state->module, value->global,
            state->global_bindings, state->global_binding_count);
        if (!global || !guard_global_read(state, value->global)) return nullptr;
        return LLVMBuildLoad2(state->builder, LLVMGlobalGetValueType(global),
                              global, "global");
    }
    if (value->kind == IR_VAL_LOCAL && value->local) {
        for (uint32_t i = 0; i < state->function->n_locals; i++)
            if (state->function->locals[i] == value->local) {
                LLVMValueRef slot = state->locals[i];
                LLVMTypeRef type = value_type(state->function->owner,
                    value->local->type,
                    LLVMGetModuleContext(state->module));
                return type ? LLVMBuildLoad2(state->builder, type,
                    slot, "local") : nullptr;
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

static const ir_instr_t *
call_output_source(const ir_instr_t *output)
{
    if (!output || output->op != IR_OP_CALL_OUTPUT
        || output->n_operands != 2
        || output->operands[0].kind != IR_OPD_VALUE
        || !output->operands[0].value) return nullptr;
    const ir_instr_t *call = output->operands[0].value->def;
    if (call && call->op == IR_OP_VARIANT_PAYLOAD)
        call = call->n_operands
            && call->operands[0].kind == IR_OPD_VALUE
            && call->operands[0].value
            ? call->operands[0].value->def : nullptr;
    return call && call->op == IR_OP_CALL ? call : nullptr;
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
    if (state->frame_roots && instruction->op != IR_OP_PHI) {
        const core_indexed_entry_t *root = core_indexed_table_find(
            state->frame_roots, instruction->dest);
        if (root) {
            LLVMValueRef stored = LLVMIsUndef(value) || LLVMIsPoison(value)
                ? LLVMConstNull(LLVMTypeOf(value)) : value;
            LLVMValueRef write = LLVMBuildStore(state->builder, stored,
                state->frame_slots[root->index]);
            LLVMSetVolatile(write, true);
        }
    }
    return true;
}

static bool
store_phi_frame_roots(lower_state_t *state, const ir_block_t *block,
                      uint32_t last_phi)
{
    for (uint32_t i = 0; i <= last_phi; i++) {
        ir_instr_t *instruction = block->instrs[i];
        if (instruction->op != IR_OP_PHI || !instruction->dest) continue;
        const core_indexed_entry_t *root = core_indexed_table_find(
            state->frame_roots, instruction->dest);
        if (!root) continue;
        LLVMValueRef value = lowered_value(state, instruction->dest);
        if (!value) return false;
        LLVMValueRef write = LLVMBuildStore(state->builder, value,
            state->frame_slots[root->index]);
        LLVMSetVolatile(write, true);
    }
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
    if (LLVMIsAConstantInt(condition)
        && !LLVMConstIntGetZExtValue(condition)) return;
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

static LLVMValueRef
semantic_integer_result(lower_state_t *state, LLVMValueRef value,
                        th_type *type, bool checked)
{
    if (!value || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
        return value;
    ir_machine_t machine = ir_machine_of(state->function->owner, type);
    unsigned storage_width = LLVMGetIntTypeWidth(LLVMTypeOf(value));
    if ((machine.class != IR_CLASS_SINT && machine.class != IR_CLASS_UINT)
        || !machine.width || machine.width >= storage_width) return value;
    LLVMTypeRef narrow_type = LLVMIntTypeInContext(
        LLVMGetModuleContext(state->module), machine.width);
    LLVMValueRef narrow = LLVMBuildTrunc(state->builder, value, narrow_type,
                                         "integer.semantic_bits");
    LLVMValueRef canonical = machine.class == IR_CLASS_SINT
        ? LLVMBuildSExt(state->builder, narrow, LLVMTypeOf(value),
                        "integer.signed_bits")
        : LLVMBuildZExt(state->builder, narrow, LLVMTypeOf(value),
                        "integer.unsigned_bits");
    if (checked)
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
            value, canonical, "integer.semantic_overflow"));
    return canonical;
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
    LLVMValueRef outcome = LLVMBuildLoad2(state->builder, type, storage,
        "outcome.value");
    LLVMBuildRet(state->builder, outcome);
    return true;
}

static LLVMTypeRef
array_descriptor_type(LLVMContextRef context)
{
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef descriptor_fields[] = {i64, i32, i32, i32, i32, i64};
    return LLVMStructTypeInContext(context, descriptor_fields, 6, false);
}

static LLVMValueRef
array_descriptor(lower_state_t *state, uint64_t count)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef descriptor_type = array_descriptor_type(context);
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
    if (!definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
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
    if ((!th_named_info(th_resolve(type), &name, nullptr, nullptr, &count)
         || !name)
        && (!definition || th_kind_of(definition) != TH_RECORD))
        return false;
    if (!name) name = "";
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

static LLVMValueRef
record_aggregate(lower_state_t *state, ir_instr_t *instruction)
{
    ir_module_t *module = state->function->owner;
    th_type *type = instruction->dest ? instruction->dest->type : nullptr;
    th_type *definition = type ? ir_type_definition(module, type) : nullptr;
    if (type && !definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    const ir_final_layout_t *layout = type ? ir_layout_final(module, type,
        ir_module_target(module)) : nullptr;
    bool variant = false;
    th_named_info(type, nullptr, nullptr, &variant, nullptr);
    size_t count = definition && th_kind_of(definition) == TH_RECORD
        ? th_record_len(definition) : 0;
    if (!definition || th_kind_of(definition) != TH_RECORD || variant
        || !layout || layout->n_members != count
        || count != instruction->n_operands || count > UINT32_MAX)
        return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(module, type, context);
    if (!native || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != count
        || !cached_literal_element_layout_matches(state, type, native))
        return nullptr;
    size_t *order = count ? calloc(count, sizeof(*order)) : nullptr;
    LLVMValueRef *fields = count ? calloc(count, sizeof(*fields)) : nullptr;
    if (count && (!order || !fields)) {
        free(order);
        free(fields);
        return nullptr;
    }
    record_layout_order(layout, order);
    bool constant = true;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        uint32_t ordinal = layout->members[order[i]].ordinal;
        ir_operand_t *operand = ordinal < count
            ? &instruction->operands[ordinal] : nullptr;
        fields[i] = operand && operand->kind == IR_OPD_VALUE
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
                (unsigned)i, "record.field");
    }
    free(order);
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

static LLVMValueRef
allocation_mutator(lower_state_t *state)
{
    return state->mutator;
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

static uint32_t
runtime_function_index(lower_state_t *state, const ir_func_t *function)
{
    for (size_t i = 0; i < state->function_index_count; i++)
        if (state->function_indices[i].function == function)
            return state->function_indices[i].index;
    return 0;
}

static bool
native_call_outcome(ir_module_t *module, const ir_value_t *value)
{
    const ir_instr_t *instruction = value ? value->def : nullptr;
    th_type *signature = nullptr;
    if (!instruction) return false;
    if (instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE)
        signature = ir_instr_call_signature(instruction);
    else if (instruction->op == IR_OP_CALLBACK_CREATE)
        ir_instr_callback_contract(instruction, nullptr, nullptr, &signature);
    th_type *failure = signature ? th_func_error(signature) : nullptr;
    return failure && ir_machine_of(module, failure).class != IR_CLASS_VOID;
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
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    if (LLVMTypeOf(value) == pointer
        && (th_kind_of(th_resolve(type)) == TH_INDIRECTION
            || is_pointer_option(type))) {
        LLVMTypeRef params[] = {pointer};
        LLVMTypeRef signature = LLVMFunctionType(
            LLVMVoidTypeInContext(context), params, 1, false);
        LLVMValueRef barrier = LLVMGetNamedFunction(state->module,
            "builtin.gc.alloc.barrier_pointer_runtime");
        if (!barrier) barrier = LLVMAddFunction(state->module,
            "builtin.gc.alloc.barrier_pointer_runtime", signature);
        if (LLVMGlobalGetValueType(barrier) != signature) return false;
        LLVMValueRef args[] = {value};
        return LLVMBuildCall2(state->builder, signature, barrier, args, 1,
                              "") != nullptr;
    }
    th_type *definition = ir_type_definition(state->function->owner, type);
    if (!definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    bool variant = false;
    th_named_info(th_resolve(type), nullptr, nullptr, &variant, nullptr);
    if (!variant && definition
        && th_kind_of(definition) == TH_RECORD
        && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMStructTypeKind) {
        size_t count = th_record_len(definition);
        const ir_final_layout_t *layout = ir_layout_final(
            state->function->owner, type,
            ir_module_target(state->function->owner));
        if (layout && count == layout->n_members
            && count == LLVMCountStructElementTypes(LLVMTypeOf(value))) {
            size_t *order = count ? calloc(count, sizeof(*order)) : nullptr;
            if (count && !order) return false;
            record_layout_order(layout, order);
            bool supported = true;
            for (size_t i = 0; i < count; i++) {
                const ir_final_member_layout_t *member =
                    &layout->members[order[i]];
                if (!member->name || member->inline_element_count
                    || !th_record_lookup(definition, member->name)) {
                    supported = false;
                    break;
                }
            }
            if (supported) {
                for (size_t i = 0; i < count; i++) {
                    const ir_final_member_layout_t *member =
                        &layout->members[order[i]];
                    th_type *field = th_record_lookup(definition,
                        member->name);
                    LLVMValueRef part = LLVMBuildExtractValue(state->builder,
                        value, (unsigned)i, "barrier.field");
                    if (!part || !store_gc_barrier(state, part, field)) {
                        free(order);
                        return false;
                    }
                }
                free(order);
                return true;
            }
            free(order);
        }
    }
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
    if (source_type == box_type) return value;
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
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef parameters[] = {i32, i32, word_type, i32, pointer};
        LLVMTypeRef signature = LLVMFunctionType(pointer, parameters,
                                                  5, false);
        LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                      "n00b_allocate");
        if (!allocator)
            allocator = LLVMAddFunction(state->module, "n00b_allocate",
                                        signature);
        if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
            return nullptr;
        LLVMValueRef arguments[] = {
            LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
            LLVMConstInt(i32, index, false),
            LLVMConstInt(word_type, size, false),
            LLVMConstInt(i32, LLVMABIAlignmentOfType(layout,
                                                     source_type), false),
            LLVMConstNull(pointer),
        };
        LLVMValueRef slot = LLVMBuildCall2(state->builder, signature,
            allocator, arguments, 5, "box.storage");
        if (!slot) return nullptr;
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
        LLVMConstInt(LLVMInt32TypeInContext(context), index, false),
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
variant_layout(ir_module_t *module, th_type *type,
               const ir_final_layout_t **out, uint64_t *stride_bytes)
{
    bool variant = false;
    if (!th_named_info(th_resolve(type), nullptr, nullptr, &variant, nullptr)
        || !variant) return false;
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    uint64_t stride_bits = 0;
    if (!layout || !layout->has_discriminant
        || layout->discriminant_is_niche
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || layout->discriminant.unit_bit_offset % 8
        || !layout->discriminant.unit_width_bits
        || layout->discriminant.unit_width_bits > 64
        || layout->discriminant.unit_width_bits % 8
        || !layout->discriminant.value_width_bits
        || layout->discriminant.value_width_bits > 64
        || layout->discriminant.value_width_bits
            > layout->discriminant.unit_width_bits
        || layout->discriminant.lsb
            > layout->discriminant.unit_width_bits
                - layout->discriminant.value_width_bits
        || layout->discriminant.unit_bit_offset > stride_bits
        || layout->discriminant.unit_width_bits > stride_bits
            - layout->discriminant.unit_bit_offset) return false;
    if (out) *out = layout;
    if (stride_bytes) *stride_bytes = stride_bits / 8;
    return true;
}

static const ir_final_member_layout_t *
variant_member(const ir_final_layout_t *layout, const char *name)
{
    for (size_t i = 0; layout && name && i < layout->n_members; i++)
        if (layout->members[i].name
            && !strcmp(layout->members[i].name, name))
            return &layout->members[i];
    return nullptr;
}

static bool
variant_payload_bytes(const ir_final_layout_t *layout,
                      const ir_final_member_layout_t *member,
                      uint64_t payload_bits, uint64_t stride_bytes,
                      uint64_t *offset)
{
    if (!layout || !member || !offset || member->bit_offset % 8
        || payload_bits % 8) return false;
    uint64_t first = member->bit_offset / 8;
    uint64_t bytes = payload_bits / 8;
    if (first > stride_bytes || bytes > stride_bytes - first)
        return false;
    uint64_t tag_first = layout->discriminant.unit_bit_offset / 8;
    uint64_t tag_end = tag_first
        + layout->discriminant.unit_width_bits / 8;
    if (bytes && first < tag_end && tag_first < first + bytes)
        return false;
    *offset = first;
    return true;
}

static LLVMValueRef
variant_byte_address(lower_state_t *state, LLVMValueRef storage,
                     uint64_t offset)
{
    LLVMTypeRef byte = LLVMInt8TypeInContext(
        LLVMGetModuleContext(state->module));
    LLVMValueRef index = LLVMConstInt(LLVMInt64TypeInContext(
        LLVMGetModuleContext(state->module)), offset, false);
    return LLVMBuildGEP2(state->builder, byte, storage, &index, 1,
                         "variant.byte");
}

static LLVMValueRef
variant_physical_tag(lower_state_t *state, LLVMValueRef storage,
                     const ir_final_layout_t *layout)
{
    const ir_layout_access_t *access = &layout->discriminant;
    LLVMTypeRef unit = LLVMIntTypeInContext(
        LLVMGetModuleContext(state->module), access->unit_width_bits);
    LLVMValueRef address = variant_byte_address(state, storage,
        access->unit_bit_offset / 8);
    LLVMValueRef word = address
        ? LLVMBuildLoad2(state->builder, unit, address, "variant.tag.word")
        : nullptr;
    if (!word) return nullptr;
    LLVMSetAlignment(word, 1);
    if (access->lsb)
        word = LLVMBuildLShr(state->builder, word,
            LLVMConstInt(unit, access->lsb, false), "variant.tag.shift");
    uint64_t mask = access->value_width_bits == 64 ? UINT64_MAX
        : (UINT64_C(1) << access->value_width_bits) - 1;
    return LLVMBuildAnd(state->builder, word,
        LLVMConstInt(unit, mask, false), "variant.tag.physical");
}

static bool
variant_store_tag(lower_state_t *state, LLVMValueRef storage,
                  const ir_final_layout_t *layout, uint64_t tag)
{
    const ir_layout_access_t *access = &layout->discriminant;
    uint64_t mask = access->value_width_bits == 64 ? UINT64_MAX
        : (UINT64_C(1) << access->value_width_bits) - 1;
    if (tag > mask) return false;
    LLVMTypeRef unit = LLVMIntTypeInContext(
        LLVMGetModuleContext(state->module), access->unit_width_bits);
    LLVMValueRef address = variant_byte_address(state, storage,
        access->unit_bit_offset / 8);
    LLVMValueRef prior = address
        ? LLVMBuildLoad2(state->builder, unit, address, "variant.tag.prior")
        : nullptr;
    if (!prior) return false;
    LLVMSetAlignment(prior, 1);
    LLVMValueRef cleared = LLVMBuildAnd(state->builder, prior,
        LLVMConstInt(unit, ~(mask << access->lsb), false),
        "variant.tag.clear");
    LLVMValueRef encoded = LLVMConstInt(unit, tag << access->lsb, false);
    LLVMValueRef merged = LLVMBuildOr(state->builder, cleared, encoded,
                                      "variant.tag.insert");
    LLVMValueRef stored = LLVMBuildStore(state->builder, merged, address);
    if (stored) {
        LLVMSetAlignment(stored, 1);
        LLVMSetVolatile(stored, true);
    }
    return stored != nullptr;
}

static bool
lower_instruction(lower_state_t *state, ir_instr_t *instruction)
{
    LLVMValueRef result = nullptr;
    ir_operand_t *operands = instruction->operands;
    uint32_t count = instruction->n_operands;
    switch (instruction->op) {
    case IR_OP_VARIANT_NEW: {
        bool variant = false;
        LLVMTypeRef type = instruction->dest
            ? value_type(state->function->owner, instruction->dest->type,
                         LLVMGetModuleContext(state->module)) : nullptr;
        if (count || !instruction->dest || !type
            || !th_named_info(th_resolve(instruction->dest->type),
                nullptr, nullptr, &variant, nullptr) || !variant) break;
        return save_result(state, instruction, LLVMConstNull(type));
    }
    case IR_OP_VARIANT_MAKE: {
        if (count != 2 || !instruction->dest
            || operands[0].kind != IR_OPD_NAME
            || operands[1].kind != IR_OPD_VALUE) break;
        ir_module_t *owner = state->function->owner;
        const ir_final_layout_t *layout = nullptr;
        uint64_t stride = 0;
        if (!variant_layout(owner, instruction->dest->type, &layout, &stride))
            break;
        const ir_final_member_layout_t *member = variant_member(layout,
            operands[0].name);
        LLVMTypeRef type = value_type(owner, instruction->dest->type,
            LLVMGetModuleContext(state->module));
        LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
        if (!member || !type || member->ordinal >= UINT32_MAX
            || LLVMABISizeOfType(target, type) != stride
            || LLVMABIAlignmentOfType(target, type)
                != layout->alignment_bytes) break;
        LLVMValueRef storage = LLVMBuildAlloca(state->builder, type,
                                               "variant.storage");
        if (!storage) break;
        LLVMValueRef zeroed = LLVMBuildStore(state->builder,
            LLVMConstNull(type), storage);
        if (!zeroed) break;
        if (!variant_store_tag(state, storage, layout,
                               (uint64_t)member->ordinal + 1)) break;
        th_type *payload_type = operands[1].value->type;
        if (ir_machine_of(owner, payload_type).class != IR_CLASS_VOID) {
            LLVMValueRef payload = lowered_value(state, operands[1].value);
            LLVMTypeRef native = value_type(owner, payload_type,
                LLVMGetModuleContext(state->module));
            const ir_final_layout_t *payload_layout = ir_layout_final(owner,
                payload_type, ir_module_target(owner));
            uint64_t payload_stride = 0;
            uint64_t payload_offset = 0;
            if (!payload || !native || !payload_layout
                || !ir_layout_final_stride_bits(payload_layout,
                                                &payload_stride)
                || !variant_payload_bytes(layout, member, payload_stride,
                                          stride, &payload_offset)
                || LLVMABISizeOfType(target, native) != payload_stride / 8
                || LLVMABIAlignmentOfType(target, native)
                    != payload_layout->alignment_bytes)
                break;
            LLVMValueRef address = variant_byte_address(state, storage,
                payload_offset);
            if (!address || !payload_layout->alignment_bytes
                || payload_offset % payload_layout->alignment_bytes) break;
            LLVMValueRef payload_store = LLVMBuildStore(state->builder,
                payload, address);
            if (!payload_store) break;
            LLVMSetAlignment(payload_store,
                payload_layout->alignment_bytes);
        }
        result = LLVMBuildLoad2(state->builder, type, storage,
                                "variant.value");
        if (!result) return unsupported(state, instruction);
        return save_result(state, instruction, result);
    }
    case IR_OP_ARMTAG: {
        if (count != 1 || operands[0].kind != IR_OPD_NAME
            || !instruction->dest) break;
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!type || LLVMGetTypeKind(type) != LLVMIntegerTypeKind) break;
        return save_result(state, instruction,
            LLVMConstInt(type, th_arm_tag(operands[0].name), false));
    }
    case IR_OP_PARAM_PRESENT: {
        const ir_value_t *parameter = count == 1
            && operands[0].kind == IR_OPD_VALUE
            ? operands[0].value : nullptr;
        if (!parameter || parameter->kind != IR_VAL_PARAM
            || parameter->owner != state->function) break;
        size_t input_index = 0, optional_index = 0;
        bool found = false;
        for (size_t i = 0; i < th_func_nparams(state->function->type); i++) {
            const th_param *formal = th_func_param(state->function->type, i);
            if (!formal || !th_mode_accepts_input(formal->mode)) continue;
            if (input_index++ == parameter->index) {
                found = ir_func_parameter_is_optional(state->function, i);
                break;
            }
            if (ir_func_parameter_is_optional(state->function, i))
                optional_index++;
        }
        LLVMTypeRef record_type = optional_record_type(state->module,
                                                        state->function);
        unsigned native_count = LLVMCountParams(state->target_function);
        if (!found || !record_type || !native_count || optional_index >= 64)
            break;
        LLVMValueRef record = LLVMGetParam(state->target_function,
                                           native_count - 1);
        LLVMValueRef slot = LLVMBuildStructGEP2(state->builder, record_type,
                                                record, 0, "omissions.address");
        LLVMTypeRef word = LLVMInt64TypeInContext(
            LLVMGetModuleContext(state->module));
        LLVMValueRef omissions = LLVMBuildLoad2(state->builder, word, slot,
                                                "omissions");
        LLVMValueRef bit = LLVMBuildAnd(state->builder, omissions,
            LLVMConstInt(word, UINT64_C(1) << optional_index, false),
            "omitted.bit");
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, bit,
                               LLVMConstNull(word), "parameter.present");
        return save_result(state, instruction, result);
    }
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
    case IR_OP_RETAIN_REF:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        if (operands[0].value->kind != IR_VAL_FUNC) {
            th_type *referent = th_ind_referent(operands[0].value->type);
            if (!referent || th_kind_of(th_resolve(referent)) != TH_FUNCTION) {
                const ir_instr_t *source = operands[0].value->def;
                if (operands[0].value->kind != IR_VAL_PARAM
                    && (!source || source->op != IR_OP_ALLOC
                        || ir_allocation_has_only_local_uses(state->function,
                                                              source))) break;
            }
        }
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
        if (!base || !index
            || LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind
            || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind)
            break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMValueRef limit = nullptr;
        bool inline_array = array != nullptr;
        if (inline_array) {
            if (!extent) break;
        } else {
            ir_module_t *source = state->function->owner;
            th_type *sequence = th_ind_referent(operands[0].value->type);
            if (!sequence || th_kind_of(th_resolve(sequence)) != TH_SEQUENCE)
                break;
            th_type *element = th_seq_elem(th_resolve(sequence));
            LLVMTypeRef native_element = element
                ? value_type(source, element, context) : nullptr;
            LLVMTypeRef native_array = value_type(source, sequence, context);
            if (!native_element || !native_array
                || !literal_element_layout_matches(source, element,
                    native_element, LLVMGetModuleDataLayout(state->module)))
                break;
            LLVMValueRef value = LLVMBuildLoad2(state->builder, native_array,
                base, "array.value");
            LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder,
                value, 1, "array.descriptor");
            base = LLVMBuildExtractValue(state->builder, value, 0,
                "array.data");
            LLVMTypeRef descriptor_type = array_descriptor_type(context);
            LLVMValueRef length_address = LLVMBuildStructGEP2(state->builder,
                descriptor_type, descriptor, 0, "array.length.address");
            limit = LLVMBuildLoad2(state->builder,
                LLVMInt64TypeInContext(context), length_address,
                "array.length");
            array = native_element;
        }
        unsigned width = LLVMGetIntTypeWidth(LLVMTypeOf(index));
        LLVMTypeRef arithmetic = LLVMIntTypeInContext(context,
            width > 64 ? width : 64);
        bool signed_index = ir_machine_of(state->function->owner,
            operands[1].value->type).class == IR_CLASS_SINT;
        LLVMValueRef position = LLVMBuildIntCast2(state->builder, index,
            arithmetic, signed_index, "index.extended");
        limit = inline_array ? LLVMConstInt(arithmetic, extent, false)
            : LLVMTypeOf(limit) == arithmetic ? limit
            : LLVMBuildZExt(state->builder, limit, arithmetic,
                "array.length.extended");
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
        if (inline_array) {
            LLVMValueRef indices[] = {
                LLVMConstInt(arithmetic, 0, false), position,
            };
            result = LLVMBuildInBoundsGEP2(state->builder, array, base,
                indices, 2, "index.address");
        } else {
            result = LLVMBuildInBoundsGEP2(state->builder, array, base,
                &position, 1, "index.address");
        }
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
        unsigned semantic_width = to.width && to.width < to_width
            ? to.width : to_width;
        if (semantic_width < to_width) {
            LLVMTypeRef narrow_type = LLVMIntTypeInContext(
                LLVMGetModuleContext(state->module), semantic_width);
            LLVMValueRef narrow = LLVMBuildTrunc(state->builder, result,
                                                  narrow_type, "cast.bits");
            result = to.class == IR_CLASS_SINT
                ? LLVMBuildSExt(state->builder, narrow, target,
                                "cast.signed_bits")
                : LLVMBuildZExt(state->builder, narrow, target,
                                "cast.unsigned_bits");
        }
        if (checked && semantic_width < from_width) {
            LLVMTypeRef narrow_type = LLVMIntTypeInContext(
                LLVMGetModuleContext(state->module), semantic_width);
            LLVMValueRef narrow = LLVMBuildTrunc(state->builder, source,
                                                  narrow_type, "cast.check_bits");
            LLVMValueRef restored = to.class == IR_CLASS_SINT
                ? LLVMBuildSExt(state->builder, narrow, LLVMTypeOf(source),
                                "cast.restore")
                : LLVMBuildZExt(state->builder, narrow, LLVMTypeOf(source),
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
    case IR_OP_NOMINAL_CAST: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        LLVMValueRef source = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!source || !target
            || LLVMGetTypeKind(target) == LLVMVoidTypeKind) break;
        LLVMTypeRef source_type = LLVMTypeOf(source);
        if (source_type == target)
            return save_result(state, instruction, source);
        LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
        if (!layout || LLVMABISizeOfType(layout, source_type)
                != LLVMABISizeOfType(layout, target)
            || LLVMABIAlignmentOfType(layout, source_type)
                != LLVMABIAlignmentOfType(layout, target)) break;
        LLVMValueRef slot = LLVMBuildAlloca(state->builder, source_type,
                                            "nominal.storage");
        if (!slot || !LLVMBuildStore(state->builder, source, slot)) break;
        result = LLVMBuildLoad2(state->builder, target, slot, "nominal.value");
        return result && save_result(state, instruction, result);
    }
    case IR_OP_BOX: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        LLVMValueRef source = lowered_value(state, operands[0].value);
        if (!source) break;
        result = box_value(state, source, operands[0].value->type);
        return result && save_result(state, instruction, result);
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
            || ir_allocation_has_only_local_uses(state->function,
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
            uint64_t typehash = th_hash(ir_module_types(
                state->function->owner), operands[0].type);
            const n00bb_llvm_type_index_t *binding = nullptr;
            for (size_t i = 0; i < state->type_index_count; i++)
                if (state->type_indices[i].typehash == typehash) {
                    binding = &state->type_indices[i];
                    break;
                }
            uint32_t type_index = binding ? binding->index : 0;
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
            LLVMValueRef mutator = allocation_mutator(state);
            if (!mutator) break;
            LLVMTypeRef i1 = LLVMInt1TypeInContext(context);
            LLVMTypeRef params[] = {pointer, i32, i64, i32, i1,
                                    pointer, pointer};
            LLVMTypeRef allocator_type = LLVMFunctionType(pointer, params,
                                                          7, false);
            LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                "builtin.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator");
            if (!allocator)
                allocator = LLVMAddFunction(state->module,
                    "builtin.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator",
                    allocator_type);
            LLVMValueRef args[] = {
                mutator,
                LLVMConstInt(i32, type_index, false),
                LLVMConstInt(i64, bytes, false),
                LLVMConstInt(i32, layout->alignment_bytes, false),
                LLVMConstInt(i1, binding->finalizable, false),
                LLVMConstNull(pointer),
                mutator,
            };
            result = LLVMBuildCall2(state->builder, allocator_type, allocator,
                                    args, 7, "storage");
            emit_trap_if(state, LLVMBuildIsNull(state->builder, result,
                "allocation.failed"));
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
        LLVMBuildStore(state->builder, value, global);
        return mark_global_initialized(state, operands[0].value->global);
    }
    case IR_OP_LOAD: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest
            || instruction->n_mods > 2
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, "atomic")
                && !ir_instr_has_mod(instruction, "volatile"))
            || ((ir_instr_has_mod(instruction, "relaxed")
                 || ir_instr_has_mod(instruction, "acquire"))
                && !ir_instr_has_mod(instruction, "atomic"))
            || ir_instr_has_mod(instruction, "release"))
            break;
        global_load_guard_t guard = {.state = state, .valid = true};
        if (ir_func_execution_phase(state->function) == IR_PHASE_RUNTIME)
            ir_value_visit_store_origins(operands[0].value,
                guard_global_load_origin, &guard);
        if (!guard.valid) return false;
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
            LLVMSetOrdering(loaded,
                ir_instr_has_mod(instruction, "relaxed")
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, "acquire")
                        ? LLVMAtomicOrderingAcquire
                        : LLVMAtomicOrderingSequentiallyConsistent);
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
            || instruction->n_mods > 2
            || (instruction->n_mods && !ir_instr_has_mod(instruction, "atomic"))
            || ir_instr_has_mod(instruction, "acquire"))
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
        if (!direct_stack_address(state->function, operands[0].value)
            && !direct_global_address(operands[0].value)
            && !ir_storage_origin_unmanaged(state->storage_origin,
                                             operands[0].value)
            && !store_gc_barrier(state, value, storage)) return false;
        result = LLVMBuildStore(state->builder, value, address);
        if (instruction->n_mods) {
            const ir_final_layout_t *layout = storage
                ? ir_layout_final(state->function->owner, storage,
                    ir_module_target(state->function->owner)) : nullptr;
            if (!layout || !layout->alignment_bytes
                || (atomic_bool && layout->intrinsic_bits != 8)) break;
            LLVMSetAlignment(result, layout->alignment_bytes);
            LLVMSetOrdering(result,
                ir_instr_has_mod(instruction, "relaxed")
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, "release")
                        ? LLVMAtomicOrderingRelease
                        : LLVMAtomicOrderingSequentiallyConsistent);
        }
        const ir_global_t *global = direct_global_address(operands[0].value);
        return !global || mark_global_initialized(state, global);
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
            || (instruction->n_mods != 0
                && (instruction->n_mods != 1
                    || (!ir_instr_has_mod(instruction, "release")
                        && !ir_instr_has_mod(instruction, "relaxed")
                        && !ir_instr_has_mod(instruction, "acquire")))))
            break;
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
            address, value,
            ir_instr_has_mod(instruction, "release")
                ? LLVMAtomicOrderingRelease
                : ir_instr_has_mod(instruction, "relaxed")
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, "acquire")
                        ? LLVMAtomicOrderingAcquire
                    : LLVMAtomicOrderingSequentiallyConsistent,
            false);
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
    case IR_OP_PREFETCH: {
        if (count != 1 || instruction->dest || instruction->n_mods
            || operands[0].kind != IR_OPD_VALUE) break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        if (!address || LLVMGetTypeKind(LLVMTypeOf(address))
            != LLVMPointerTypeKind) break;
        unsigned id = LLVMLookupIntrinsicID("llvm.prefetch",
                                            sizeof("llvm.prefetch") - 1);
        LLVMTypeRef pointer = LLVMTypeOf(address);
        LLVMValueRef intrinsic = LLVMGetIntrinsicDeclaration(state->module,
            id, &pointer, 1);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(
            LLVMGetModuleContext(state->module));
        LLVMValueRef arguments[] = {
            address,
            LLVMConstInt(i32, 0, false),
            LLVMConstInt(i32, 3, false),
            LLVMConstInt(i32, 1, false),
        };
        return intrinsic && LLVMBuildCall2(state->builder,
            LLVMGlobalGetValueType(intrinsic), intrinsic,
            arguments, 4, "") != nullptr;
    }
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
        if (!result) result = record_aggregate(state, instruction);
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
        result = semantic_integer_result(state, result,
            operands[0].value->type, instruction->n_mods != 0);
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
        result = semantic_integer_result(state, result,
            operands[0].value->type,
            ir_instr_has_mod(instruction, "trap"));
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
        result = semantic_integer_result(state, result,
            operands[0].value->type, instruction->n_mods != 0);
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
        result = semantic_integer_result(state, result,
            operands[0].value->type, false);
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
            bool count_type_proves_range = count_machine.class == IR_CLASS_UINT
                && count_machine.width < 64
                && (UINT64_C(1) << count_machine.width)
                    <= value_machine.width;
            bool constant_proves_range = count_machine.class == IR_CLASS_UINT
                && count_width <= 64 && LLVMIsAConstantInt(right)
                && LLVMConstIntGetZExtValue(right) < value_machine.width;
            if (!count_type_proves_range && !constant_proves_range) {
                unsigned check_width = count_width > 32 ? count_width : 32;
                LLVMTypeRef check_type = LLVMIntTypeInContext(
                    LLVMGetModuleContext(state->module), check_width);
                LLVMValueRef checked_count = count_width == check_width
                    ? right : LLVMBuildZExt(state->builder, right, check_type,
                                           "shift.count.wide");
                emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntUGE,
                    checked_count, LLVMConstInt(check_type,
                                                value_machine.width, false),
                    "shift.range"));
            }
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
        result = semantic_integer_result(state, result,
            operands[0].value->type,
            instruction->op == IR_OP_SHL && instruction->n_mods);
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
            || !operands[0].value
            || !instruction->dest) break;
        ir_module_t *owner = state->function->owner;
        const ir_final_layout_t *layout = nullptr;
        uint64_t stride = 0;
        bool named_variant = false;
        if (th_named_info(th_resolve(operands[0].value->type), nullptr,
                          nullptr, &named_variant, nullptr)
            && named_variant
            && !native_call_outcome(owner, operands[0].value)) {
            if (!variant_layout(owner, operands[0].value->type, &layout,
                                &stride)) break;
            LLVMValueRef variant = lowered_value(state, operands[0].value);
            LLVMTypeRef native = variant ? LLVMTypeOf(variant) : nullptr;
            LLVMTypeRef dest = value_type(owner, instruction->dest->type,
                LLVMGetModuleContext(state->module));
            LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
            if (!native || !dest || LLVMGetTypeKind(dest)
                    != LLVMIntegerTypeKind
                || LLVMABISizeOfType(target, native) != stride
                || LLVMABIAlignmentOfType(target, native)
                    != layout->alignment_bytes) break;
            LLVMValueRef storage = LLVMBuildAlloca(state->builder, native,
                "variant.tag.storage");
            if (!storage || !LLVMBuildStore(state->builder, variant,
                                             storage)) break;
            LLVMValueRef physical = variant_physical_tag(state, storage,
                layout);
            if (!physical) break;
            LLVMTypeRef unit = LLVMTypeOf(physical);
            result = LLVMConstInt(dest, 0, false);
            for (size_t i = 0; i < layout->n_members; i++) {
                const ir_final_member_layout_t *member = &layout->members[i];
                LLVMValueRef matches = LLVMBuildICmp(state->builder,
                    LLVMIntEQ, physical,
                    LLVMConstInt(unit, (uint64_t)member->ordinal + 1, false),
                    "variant.arm.matches");
                result = LLVMBuildSelect(state->builder, matches,
                    LLVMConstInt(dest, th_arm_tag(member->name),
                                 false), result, "variant.arm.tag");
            }
            return save_result(state, instruction, result);
        }
        if (!native_call_outcome(owner, operands[0].value)) break;
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
            || !operands[0].value
            || !instruction->dest) break;
        ir_module_t *owner = state->function->owner;
        const ir_final_layout_t *layout = nullptr;
        uint64_t stride = 0;
        bool named_variant = false;
        if (th_named_info(th_resolve(operands[0].value->type), nullptr,
                          nullptr, &named_variant, nullptr)
            && named_variant
            && !native_call_outcome(owner, operands[0].value)) {
            if (!variant_layout(owner, operands[0].value->type, &layout,
                                &stride)) break;
            const ir_final_member_layout_t *member = variant_member(layout,
                operands[1].name);
            const ir_final_layout_t *payload_layout = ir_layout_final(owner,
                instruction->dest->type, ir_module_target(owner));
            uint64_t payload_stride = 0;
            uint64_t payload_offset = 0;
            LLVMValueRef variant = lowered_value(state, operands[0].value);
            LLVMTypeRef native = variant ? LLVMTypeOf(variant) : nullptr;
            LLVMTypeRef payload_type = value_type(owner,
                instruction->dest->type,
                LLVMGetModuleContext(state->module));
            LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
            if (!member || !native || !payload_type || !payload_layout
                || !ir_layout_final_stride_bits(payload_layout,
                                                &payload_stride)
                || !variant_payload_bytes(layout, member, payload_stride,
                                          stride, &payload_offset)
                || LLVMABISizeOfType(target, native) != stride
                || LLVMABISizeOfType(target, payload_type)
                    != payload_stride / 8
                || LLVMABIAlignmentOfType(target, native)
                    != layout->alignment_bytes
                || LLVMABIAlignmentOfType(target, payload_type)
                    != payload_layout->alignment_bytes)
                break;
            LLVMValueRef storage = LLVMBuildAlloca(state->builder, native,
                "variant.payload.storage");
            if (!storage || !LLVMBuildStore(state->builder, variant,
                                             storage)) break;
            LLVMValueRef physical = variant_physical_tag(state, storage,
                layout);
            if (!physical) break;
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                physical, LLVMConstInt(LLVMTypeOf(physical),
                    (uint64_t)member->ordinal + 1, false),
                "variant.wrong.arm"));
            LLVMValueRef address = variant_byte_address(state, storage,
                payload_offset);
            result = address ? LLVMBuildLoad2(state->builder, payload_type,
                address, "variant.payload") : nullptr;
            if (!result) break;
            LLVMSetAlignment(result, 1);
            return save_result(state, instruction, result);
        }
        if (!native_call_outcome(owner, operands[0].value)) break;
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
    case IR_OP_CALLBACK_CREATE: {
        th_type *setup_signature = nullptr;
        if (!instruction->dest
            || !ir_instr_callback_contract(instruction, nullptr, nullptr,
                &setup_signature)
            || (setup_signature ? count < 3 : count != 1)
            || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        if (setup_signature
            && (operands[1].kind != IR_OPD_VALUE
                || !operands[1].value
                || operands[2].kind != IR_OPD_VALUE
                || !operands[2].value)) break;
        ir_value_t *handler_ref = operands[setup_signature ? 1 : 0].value;
        const ir_instr_t *retained = handler_ref->def;
        ir_value_t *handler = retained && retained->op == IR_OP_RETAIN_REF
            && retained->n_operands == 1
            && retained->operands[0].kind == IR_OPD_VALUE
            ? retained->operands[0].value : nullptr;
        if (!handler) break;
        uint32_t handler_index = handler->kind == IR_VAL_FUNC
            ? runtime_function_index(state, handler->owner) : 0;
        if (handler->kind == IR_VAL_FUNC && !handler_index) break;
        th_type *callback_type = instruction->dest->type;
        th_type *failure = setup_signature
            ? th_func_error(setup_signature) : nullptr;
        bool can_fail = failure && ir_machine_of(state->function->owner,
            failure).class != IR_CLASS_VOID;
        if (can_fail) {
            th_type *definition = ir_type_definition(state->function->owner,
                callback_type);
            callback_type = definition
                ? th_record_lookup(definition, "ok") : nullptr;
        }
        uint32_t callback_index = callback_type
            ? runtime_type_index(state, callback_type) : 0;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef type = callback_type ? value_type(state->function->owner,
            callback_type, context) : nullptr;
        if (!callback_index || !type
            || LLVMGetTypeKind(type) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(type) != 3) break;
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMValueRef index_value = handler_index
            ? LLVMConstInt(i32, handler_index, false) : nullptr;
        if (!index_value) {
            LLVMValueRef address = lowered_value(state, handler_ref);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef index_signature = LLVMFunctionType(i32,
                &pointer, 1, false);
            LLVMValueRef lookup = LLVMGetNamedFunction(state->module,
                "n00b_abi_v1_runtime_function_index");
            if (!lookup) lookup = LLVMAddFunction(state->module,
                "n00b_abi_v1_runtime_function_index", index_signature);
            if (!address || LLVMTypeOf(address) != pointer || !lookup
                || LLVMGlobalGetValueType(lookup) != index_signature) break;
            index_value = LLVMBuildCall2(state->builder, index_signature,
                lookup, &address, 1, "callback.handler.index");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntEQ,
                index_value, LLVMConstNull(i32),
                "callback.handler.unknown"));
        }
        LLVMValueRef callback = LLVMGetUndef(type);
        callback = LLVMBuildInsertValue(state->builder, callback,
            index_value, 0, "callback.handler");
        callback = LLVMBuildInsertValue(state->builder, callback,
            LLVMConstInt(i32, callback_index, false), 1, "callback.type");
        LLVMValueRef environment = setup_signature
            && operands[2].kind == IR_OPD_VALUE
            ? lowered_value(state, operands[2].value)
            : LLVMConstNull(LLVMPointerTypeInContext(context, 0));
        callback = environment ? LLVMBuildInsertValue(state->builder,
            callback, environment, 2, "callback.environment") : nullptr;
        if (!callback) break;
        if (!setup_signature) return save_result(state, instruction, callback);
        ir_value_t *setup_value = operands[0].value;
        ir_func_t *setup_target = setup_value->kind == IR_VAL_FUNC
            ? setup_value->owner : nullptr;
        LLVMValueRef setup = setup_target
            ? lowered_value(state, setup_value) : nullptr;
        LLVMTypeRef setup_type = setup ? LLVMGlobalGetValueType(setup)
            : nullptr;
        size_t formal_count = setup_target
            ? th_func_nparams(setup_target->type) : 0;
        unsigned native_count = setup_type
            ? LLVMCountParamTypes(setup_type) : 0;
        ir_value_t **provided = formal_count
            ? calloc(formal_count, sizeof(*provided)) : nullptr;
        LLVMValueRef *setup_args = native_count
            ? calloc(native_count, sizeof(*setup_args)) : nullptr;
        LLVMTypeRef *expected = native_count
            ? calloc(native_count, sizeof(*expected)) : nullptr;
        bool valid = setup && setup_type && formal_count >= 2
            && provided && setup_args && expected;
        if (valid) LLVMGetParamTypes(setup_type, expected);
        if (valid) {
            provided[0] = handler_ref;
            provided[1] = operands[2].value;
        }
        size_t positional = 2;
        for (uint32_t i = 3; valid && i < count; i++) {
            if (operands[i].kind == IR_OPD_VALUE) {
                valid = positional < formal_count
                    && !provided[positional]
                    && operands[i].value;
                if (valid) provided[positional++] = operands[i].value;
            } else if (operands[i].kind == IR_OPD_NAMED_ARG) {
                size_t at = 0;
                for (; at < formal_count; at++)
                    if (setup_target->params[at]
                        && setup_target->params[at]->name
                        && !strcmp(setup_target->params[at]->name,
                                   operands[i].named.name)) break;
                valid = at >= 2 && at < formal_count
                    && !provided[at] && operands[i].named.value;
                if (valid) provided[at] = operands[i].named.value;
            } else valid = false;
        }
        size_t native_at = 0, optional_at = 0;
        uint64_t omissions = 0;
        LLVMTypeRef record_type = optional_record_type(state->module,
                                                        setup_target);
        LLVMValueRef record = record_type
            ? entry_alloca(state, record_type) : nullptr;
        if (record)
            LLVMBuildStore(state->builder, LLVMConstNull(record_type), record);
        for (size_t i = 0; valid && i < formal_count; i++) {
            bool optional = ir_func_parameter_is_optional(setup_target, i);
            if (!provided[i]) {
                valid = optional && optional_at < 64;
                if (valid) omissions |= UINT64_C(1) << optional_at++;
                continue;
            }
            LLVMValueRef value = lowered_value(state, provided[i]);
            LLVMTypeRef wanted = optional
                ? record_type ? LLVMStructGetTypeAtIndex(record_type,
                    (unsigned)optional_at + 1) : nullptr
                : native_at < native_count ? expected[native_at] : nullptr;
            if (value && wanted == box_llvm_type(context)
                && LLVMTypeOf(value) != wanted)
                value = box_value(state, value, provided[i]->type);
            valid = value && wanted && LLVMTypeOf(value) == wanted;
            if (!valid) break;
            if (optional) {
                LLVMValueRef slot = LLVMBuildStructGEP2(state->builder,
                    record_type, record, (unsigned)optional_at + 1,
                    "callback.setup.optional");
                LLVMBuildStore(state->builder, value, slot);
                optional_at++;
            } else setup_args[native_at++] = value;
        }
        if (valid && record) {
            LLVMValueRef omission_slot = LLVMBuildStructGEP2(state->builder,
                record_type, record, 0, "callback.setup.omissions");
            LLVMBuildStore(state->builder,
                LLVMConstInt(LLVMInt64TypeInContext(context), omissions,
                             false), omission_slot);
            if (native_at < native_count) setup_args[native_at++] = record;
            else valid = false;
        }
        valid = valid && native_at == native_count;
        if (valid) result = LLVMBuildCall2(state->builder, setup_type, setup,
            setup_args, native_count, can_fail ? "callback.setup" : "");
        free(provided);
        free(setup_args);
        free(expected);
        if (!valid) break;
        if (!result) break;
        if (!can_fail) return save_result(state, instruction, callback);
        LLVMTypeRef setup_outcome = LLVMTypeOf(result);
        LLVMTypeRef outcome = native_outcome_type(state->function->owner,
            callback_type, failure, context);
        LLVMTypeRef failure_type = value_type(state->function->owner,
            failure, context);
        if (!outcome || !failure_type
            || LLVMGetTypeKind(setup_outcome) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(setup_outcome) != 2
            || LLVMGetTypeKind(outcome) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(outcome) != 2) break;
        LLVMValueRef storage = entry_alloca(state, outcome);
        LLVMBuildStore(state->builder, LLVMConstNull(outcome), storage);
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, result, 1,
            "callback.setup.tag");
        LLVMValueRef outcome_tag = LLVMBuildStructGEP2(state->builder,
            outcome, storage, 1, "callback.result.tag");
        LLVMBuildStore(state->builder, tag, outcome_tag);
        LLVMValueRef payload = LLVMBuildStructGEP2(state->builder, outcome,
            storage, 0, "callback.setup.payload");
        LLVMValueRef success = LLVMBuildICmp(state->builder, LLVMIntEQ,
            tag, LLVMConstInt(LLVMTypeOf(tag), 0, false),
            "callback.setup.ok");
        LLVMBasicBlockRef yes = LLVMAppendBasicBlockInContext(context,
            state->target_function, "callback.setup.success");
        LLVMBasicBlockRef no = LLVMAppendBasicBlockInContext(context,
            state->target_function, "callback.setup.failure");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
            state->target_function, "callback.setup.done");
        LLVMBuildCondBr(state->builder, success, yes, no);
        LLVMPositionBuilderAtEnd(state->builder, yes);
        LLVMBuildStore(state->builder, callback, payload);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, no);
        LLVMValueRef setup_storage = entry_alloca(state, setup_outcome);
        LLVMBuildStore(state->builder, result, setup_storage);
        LLVMValueRef setup_payload = LLVMBuildStructGEP2(state->builder,
            setup_outcome, setup_storage, 0, "callback.setup.error");
        LLVMValueRef error_value = LLVMBuildLoad2(state->builder,
            failure_type, setup_payload, "callback.error.value");
        LLVMBuildStore(state->builder, error_value, payload);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, done);
        result = LLVMBuildLoad2(state->builder, outcome, storage,
            "callback.setup.outcome");
        return save_result(state, instruction, result);
    }
    case IR_OP_CALLBACK_INVOKE: {
        if (!count || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        LLVMValueRef callback = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (!callback || LLVMGetTypeKind(LLVMTypeOf(callback))
            != LLVMStructTypeKind
            || LLVMCountStructElementTypes(LLVMTypeOf(callback)) != 3)
            break;
        th_type *handler_type = ir_instr_callback_invoke_handler(instruction);
        if (!handler_type)
            handler_type = ir_instr_call_signature(instruction);
        LLVMTypeRef signature = native_function_type(state->module,
            state->function->owner, handler_type);
        unsigned parameter_count = signature
            ? LLVMCountParamTypes(signature) : 0;
        bool stateful = parameter_count == count;
        if (!signature || (parameter_count != count - 1 && !stateful)) break;
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef resolver_type = LLVMFunctionType(pointer, &i32, 1, false);
        LLVMValueRef resolver = LLVMGetNamedFunction(state->module,
            "n00b_abi_v1_runtime_function_address");
        if (!resolver) resolver = LLVMAddFunction(state->module,
            "n00b_abi_v1_runtime_function_address", resolver_type);
        if (!resolver || LLVMGlobalGetValueType(resolver) != resolver_type)
            break;
        LLVMValueRef index = LLVMBuildExtractValue(state->builder, callback,
                                                    0, "callback.index");
        LLVMValueRef callee = LLVMBuildCall2(state->builder, resolver_type,
            resolver, &index, 1, "callback.target");
        if (!callee) break;
        emit_trap_if(state, LLVMBuildIsNull(state->builder, callee,
                                            "callback.unresolved"));
        LLVMValueRef *arguments = parameter_count
            ? calloc(parameter_count, sizeof(*arguments)) : nullptr;
        LLVMTypeRef *expected = parameter_count
            ? calloc(parameter_count, sizeof(*expected)) : nullptr;
        bool valid = (!parameter_count || (arguments && expected));
        if (valid && parameter_count)
            LLVMGetParamTypes(signature, expected);
        LLVMValueRef environment = nullptr;
        LLVMValueRef state_storage = nullptr;
        th_type *state_type = nullptr;
        if (valid && stateful) {
            const th_param *parameter = th_func_param(handler_type, 0);
            state_type = parameter && parameter->mode == TH_INOUT
                ? parameter->type : nullptr;
            LLVMTypeRef native_state = state_type
                ? value_type(state->function->owner, state_type, context)
                : nullptr;
            environment = LLVMBuildExtractValue(state->builder, callback, 2,
                "callback.environment");
            valid = native_state && environment && expected[0] == pointer;
            if (valid) {
                emit_trap_if(state, LLVMBuildIsNull(state->builder,
                    environment, "callback.missing.environment"));
                state_storage = entry_alloca(state, native_state);
                LLVMValueRef input = LLVMBuildLoad2(state->builder,
                    native_state, environment, "callback.state.input");
                LLVMBuildStore(state->builder, input, state_storage);
                arguments[0] = state_storage;
                valid = state_storage != nullptr;
            }
        }
        for (uint32_t i = 1; valid && i < count; i++) {
            unsigned ordinal = i - 1 + (stateful ? 1 : 0);
            arguments[ordinal] = operands[i].kind == IR_OPD_VALUE
                ? lowered_value(state, operands[i].value) : nullptr;
            valid = arguments[ordinal]
                && LLVMTypeOf(arguments[ordinal]) == expected[ordinal];
        }
        if (valid) result = LLVMBuildCall2(state->builder, signature, callee,
            arguments, parameter_count,
            instruction->dest ? "callback.call" : "");
        if (valid && result && stateful) {
            LLVMTypeRef native_state = value_type(state->function->owner,
                state_type, context);
            th_type *failure = th_func_error(handler_type);
            bool can_fail = failure && ir_machine_of(state->function->owner,
                failure).class != IR_CLASS_VOID;
            LLVMBasicBlockRef done = nullptr;
            if (can_fail) {
                LLVMValueRef tag = LLVMBuildExtractValue(state->builder,
                    result, 1, "callback.result.tag");
                LLVMValueRef success = LLVMBuildICmp(state->builder,
                    LLVMIntEQ, tag,
                    LLVMConstInt(LLVMTypeOf(tag), 0, false),
                    "callback.copyout.ok");
                LLVMBasicBlockRef yes = LLVMAppendBasicBlockInContext(
                    context, state->target_function, "callback.copyout");
                done = LLVMAppendBasicBlockInContext(context,
                    state->target_function, "callback.copyout.done");
                LLVMBuildCondBr(state->builder, success, yes, done);
                LLVMPositionBuilderAtEnd(state->builder, yes);
            }
            LLVMValueRef output = LLVMBuildLoad2(state->builder,
                native_state, state_storage, "callback.state.output");
            valid = store_gc_barrier(state, output, state_type)
                && LLVMBuildStore(state->builder, output, environment);
            if (done) {
                LLVMBuildBr(state->builder, done);
                LLVMPositionBuilderAtEnd(state->builder, done);
            }
        }
        free(arguments);
        free(expected);
        if (!valid || !result) break;
        return instruction->dest ? save_result(state, instruction, result)
                                 : true;
    }
    case IR_OP_CALL: {
        if (!count || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        ir_func_t *target = operands[0].value->kind == IR_VAL_FUNC
            ? operands[0].value->owner : nullptr;
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
        if (native && native->kind == IR_ANNOT_STR && native->s
            && !strcmp(native->s, "n00b_builtin_abi_ref_address")) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            LLVMValueRef address = lowered_value(state, operands[1].value);
            LLVMTypeRef integer = value_type(state->function->owner,
                instruction->dest->type, LLVMGetModuleContext(state->module));
            if (!address || !integer
                || LLVMGetTypeKind(LLVMTypeOf(address)) != LLVMPointerTypeKind
                || LLVMGetTypeKind(integer) != LLVMIntegerTypeKind) break;
            result = LLVMBuildPtrToInt(state->builder, address, integer,
                "ref.address");
            return result && save_result(state, instruction, result);
        }
        bool mutator_call = !native && target
            && inline_list_contains(state->mutator_functions, target);
        LLVMValueRef callee = nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s) {
            LLVMTypeRef signature = function_type(state->module, target);
            if (!signature) break;
            callee = LLVMGetNamedFunction(state->module, native->s);
            if (!callee)
                callee = LLVMAddFunction(state->module, native->s,
                                         signature);
            if (!callee || LLVMGlobalGetValueType(callee) != signature) break;
        } else if (mutator_call) {
            callee = mutator_function(state->module, target);
            if (!callee) {
                LLVMTypeRef type = mutator_function_type(state->module,
                                                         target);
                core_buffer_t name = {0};
                core_buffer_printf(&name, "%s.%s$gc_mutator",
                    ir_module_name(target->owner), target->name);
                if (type) callee = LLVMAddFunction(state->module,
                    name.data, type);
                core_buffer_release(&name);
            }
        } else callee = lowered_value(state, operands[0].value);
        LLVMTypeRef signature = target && callee
            ? LLVMGlobalGetValueType(callee)
            : native_function_type(state->module, state->function->owner,
                                   ir_instr_call_signature(instruction));
        th_type *formal_signature = target ? target->type
            : ir_instr_call_signature(instruction);
        size_t optional_count = 0;
        if (target)
            for (size_t i = 0; i < th_func_nparams(target->type); i++)
                if (ir_func_parameter_is_optional(target, i)) optional_count++;
        LLVMTypeRef record_type = optional_count
            ? optional_record_type(state->module, target) : nullptr;
        unsigned native_count = signature &&
            LLVMGetTypeKind(signature) == LLVMFunctionTypeKind
            ? LLVMCountParamTypes(signature) : 0;
        if (!callee || !signature
            || LLVMGetTypeKind(signature) != LLVMFunctionTypeKind
            || !formal_signature
            || th_func_nparams(formal_signature) != count - 1
            || native_count != count - 1 - optional_count
                + (optional_count != 0) + (mutator_call ? 1 : 0)
            || (optional_count && !record_type)
            || (!target && LLVMGetTypeKind(LLVMTypeOf(callee))
                != LLVMPointerTypeKind)) break;
        LLVMValueRef *arguments = native_count
            ? calloc(native_count, sizeof(*arguments)) : nullptr;
        LLVMTypeRef *expected_types = native_count
            ? calloc(native_count, sizeof(*expected_types)) : nullptr;
        if (native_count && (!arguments || !expected_types)) {
            free(arguments);
            free(expected_types);
            break;
        }
        if (native_count) LLVMGetParamTypes(signature, expected_types);
        LLVMValueRef record = optional_count
            ? entry_alloca(state, record_type) : nullptr;
        if (record) LLVMBuildStore(state->builder,
                                   LLVMConstNull(record_type), record);
        bool valid = !optional_count || record;
        uint32_t output_ordinal = 0;
        size_t native_index = 0, optional_index = 0;
        uint64_t omissions = 0;
        for (uint32_t i = 1; valid && i < count; i++) {
            const th_param *formal = formal_signature
                ? th_func_param(formal_signature, i - 1) : nullptr;
            bool optional = target
                && ir_func_parameter_is_optional(target, i - 1);
            if (operands[i].kind == IR_OPD_MISSING) {
                valid = optional && formal
                    && th_mode_is_input_only(formal->mode)
                    && optional_index < 64;
                if (valid) omissions |= UINT64_C(1) << optional_index++;
                continue;
            }
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
                    else LLVMBuildStore(state->builder, input, storage);
                }
                valid = storage && save_call_output(state, instruction,
                    output_ordinal++, storage);
                if (valid) arguments[native_index] = storage;
            } else {
                LLVMValueRef value = lowered_value(state, operands[i].value);
                if (optional) {
                    LLVMTypeRef expected = LLVMStructGetTypeAtIndex(
                        record_type, (unsigned)optional_index + 1);
                    if (value && expected == box_llvm_type(
                            LLVMGetModuleContext(state->module))
                        && LLVMTypeOf(value) != expected)
                        value = box_value(state, value,
                                          operands[i].value->type);
                    valid = value && LLVMTypeOf(value) == expected;
                    if (valid) {
                        LLVMValueRef slot = LLVMBuildStructGEP2(state->builder,
                            record_type, record,
                            (unsigned)optional_index + 1,
                            "optional.argument.address");
                        LLVMBuildStore(state->builder, value, slot);
                    }
                    optional_index++;
                    continue;
                }
                arguments[native_index] = value;
            }
            valid = valid && arguments[native_index];
            if (valid) {
                LLVMTypeRef expected = expected_types[native_index];
                if (!th_mode_produces_output(formal->mode)
                    && expected == box_llvm_type(LLVMGetModuleContext(
                        state->module))
                    && LLVMTypeOf(arguments[native_index]) != expected)
                    arguments[native_index] = box_value(state,
                        arguments[native_index], operands[i].value->type);
                valid = arguments[native_index]
                    && (!expected || LLVMTypeOf(arguments[native_index])
                        == expected);
            }
            native_index++;
        }
        if (valid && optional_count) {
            LLVMValueRef slot = LLVMBuildStructGEP2(state->builder,
                record_type, record, 0, "omissions.address");
            LLVMBuildStore(state->builder,
                LLVMConstInt(LLVMInt64TypeInContext(
                    LLVMGetModuleContext(state->module)), omissions, false),
                slot);
            arguments[native_index++] = record;
        }
        if (valid && mutator_call) {
            arguments[native_index++] = allocation_mutator(state);
            valid = arguments[native_index - 1] != nullptr;
        }
        valid = valid && native_index == native_count;
        bool returns_void = LLVMGetTypeKind(LLVMGetReturnType(signature))
            == LLVMVoidTypeKind;
        if (valid)
            result = LLVMBuildCall2(state->builder, signature,
                callee, arguments,
                native_count,
                instruction->dest && !returns_void ? "call" : "");
        free(arguments);
        free(expected_types);
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
        const ir_instr_t *call = call_output_source(instruction);
        if (!call) break;
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
    case IR_OP_SWITCH: {
        if (count < 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_DEFAULT_BLOCK) break;
        LLVMValueRef scrutinee = lowered_value(state, operands[0].value);
        LLVMBasicBlockRef otherwise = lowered_block(state, operands[1].block);
        if (!scrutinee || !otherwise
            || LLVMGetTypeKind(LLVMTypeOf(scrutinee))
                != LLVMIntegerTypeKind) break;
        ir_machine_t machine = ir_machine_of(state->function->owner,
                                            operands[0].value->type);
        bool valid = true;
        for (uint32_t i = 2; i < count; i++) {
            if (operands[i].kind != IR_OPD_SWITCH_ARM
                || !integer_constant(machine, LLVMTypeOf(scrutinee),
                                     operands[i].arm.value)
                || !lowered_block(state, operands[i].arm.block)) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        LLVMValueRef branch = LLVMBuildSwitch(state->builder, scrutinee,
                                               otherwise, count - 2);
        if (!branch) break;
        for (uint32_t i = 2; i < count; i++)
            LLVMAddCase(branch,
                integer_constant(machine, LLVMTypeOf(scrutinee),
                                 operands[i].arm.value),
                lowered_block(state, operands[i].arm.block));
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
native_function_type(LLVMModuleRef target, ir_module_t *owner,
                     th_type *signature)
{
    if (!owner || !signature
        || th_kind_of(th_resolve(signature)) != TH_FUNCTION) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(owner, signature, context);
    if (!result || th_func_nkw(signature)
        || th_func_rest(signature))
        return nullptr;
    size_t count = th_func_nparams(signature);
    if (count > UINT32_MAX) return nullptr;
    LLVMTypeRef *parameters = count
        ? calloc(count, sizeof(*parameters)) : nullptr;
    if (count && !parameters) return nullptr;
    for (size_t i = 0; i < count; i++) {
        const th_param *parameter = th_func_param(signature, i);
        LLVMTypeRef value = parameter
            ? value_type(owner, parameter->type, context)
            : nullptr;
        if (!value || LLVMGetTypeKind(value)
                                == LLVMVoidTypeKind) {
            free(parameters);
            return nullptr;
        }
        parameters[i] = th_mode_produces_output(parameter->mode)
            ? LLVMPointerTypeInContext(context, 0) : value;
    }
    LLVMTypeRef native_signature = LLVMFunctionType(result, parameters,
        (unsigned)count, false);
    free(parameters);
    return native_signature;
}

static LLVMTypeRef
optional_record_type(LLVMModuleRef target, ir_func_t *function)
{
    if (!target || !function) return nullptr;
    size_t count = th_func_nparams(function->type);
    size_t optional_count = 0;
    for (size_t i = 0; i < count; i++)
        if (ir_func_parameter_is_optional(function, i)) optional_count++;
    if (!optional_count || optional_count > 64) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef *fields = calloc(optional_count + 1, sizeof(*fields));
    if (!fields) return nullptr;
    fields[0] = LLVMInt64TypeInContext(context);
    size_t ordinal = 1;
    for (size_t i = 0; i < count; i++) {
        if (!ir_func_parameter_is_optional(function, i)) continue;
        const th_param *formal = th_func_param(function->type, i);
        fields[ordinal] = formal && th_mode_is_input_only(formal->mode)
            ? value_type(function->owner, formal->type, context) : nullptr;
        if (!fields[ordinal]
            || LLVMGetTypeKind(fields[ordinal]) == LLVMVoidTypeKind) {
            free(fields);
            return nullptr;
        }
        ordinal++;
    }
    LLVMTypeRef record = LLVMStructTypeInContext(context, fields,
        (unsigned)(optional_count + 1), false);
    free(fields);
    return record;
}

static LLVMTypeRef
function_type(LLVMModuleRef target, ir_func_t *function)
{
    if (!function) return nullptr;
    size_t count = th_func_nparams(function->type);
    size_t optional_count = 0;
    for (size_t i = 0; i < count; i++)
        if (ir_func_parameter_is_optional(function, i)) optional_count++;
    if (!optional_count)
        return native_function_type(target, function->owner, function->type);
    LLVMTypeRef record = optional_record_type(target, function);
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(function->owner, function->type,
                                            context);
    if (!record || !result || count > UINT32_MAX || th_func_nkw(function->type)
        || th_func_rest(function->type)) return nullptr;
    LLVMTypeRef *parameters = calloc(count - optional_count + 1,
                                     sizeof(*parameters));
    if (!parameters) return nullptr;
    size_t native_index = 0;
    for (size_t i = 0; i < count; i++) {
        if (ir_func_parameter_is_optional(function, i)) continue;
        const th_param *formal = th_func_param(function->type, i);
        LLVMTypeRef value = formal
            ? value_type(function->owner, formal->type, context) : nullptr;
        if (!value || LLVMGetTypeKind(value) == LLVMVoidTypeKind) {
            free(parameters);
            return nullptr;
        }
        parameters[native_index++] = th_mode_produces_output(formal->mode)
            ? LLVMPointerTypeInContext(context, 0) : value;
    }
    parameters[native_index++] = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef signature = LLVMFunctionType(result, parameters,
        (unsigned)native_index, false);
    free(parameters);
    return signature;
}

static LLVMTypeRef
mutator_function_type(LLVMModuleRef target, ir_func_t *function)
{
    LLVMTypeRef base = function_type(target, function);
    if (!base) return nullptr;
    unsigned count = LLVMCountParamTypes(base);
    LLVMTypeRef *parameters = calloc((size_t)count + 1,
                                     sizeof(*parameters));
    if (!parameters) return nullptr;
    if (count) LLVMGetParamTypes(base, parameters);
    parameters[count] = LLVMPointerTypeInContext(
        LLVMGetModuleContext(target), 0);
    LLVMTypeRef result = LLVMFunctionType(LLVMGetReturnType(base), parameters,
        count + 1, false);
    free(parameters);
    return result;
}

static LLVMValueRef
mutator_function(LLVMModuleRef target, ir_func_t *function)
{
    core_buffer_t name = {0};
    core_buffer_printf(&name, "%s.%s$gc_mutator",
        ir_module_name(function->owner), function->name);
    LLVMValueRef result = LLVMGetNamedFunction(target, name.data);
    core_buffer_release(&name);
    return result;
}

static bool
lower_mutator_wrapper(LLVMModuleRef target, ir_func_t *function,
                      core_diagnostics_t *diagnostics)
{
    core_buffer_t name = {0};
    core_buffer_printf(&name, "%s.%s", ir_module_name(function->owner),
        function->name);
    LLVMValueRef wrapper = LLVMGetNamedFunction(target, name.data);
    core_buffer_release(&name);
    LLVMValueRef body = mutator_function(target, function);
    if (!wrapper || !body)
        return backend_error(diagnostics,
            "LLVM backend could not declare mutator entry");
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    unsigned count = LLVMCountParams(wrapper);
    LLVMValueRef *arguments = calloc((size_t)count + 1,
                                    sizeof(*arguments));
    if (!builder || !arguments) {
        LLVMDisposeBuilder(builder);
        free(arguments);
        return backend_error(diagnostics,
            "LLVM backend could not build mutator wrapper");
    }
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context,
        wrapper, "entry");
    LLVMPositionBuilderAtEnd(builder, entry);
    for (unsigned i = 0; i < count; i++)
        arguments[i] = LLVMGetParam(wrapper, i);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef getter_type = LLVMFunctionType(pointer, nullptr, 0, false);
    LLVMValueRef getter = LLVMGetNamedFunction(target,
        "builtin.gc.alloc.mutator_for_native");
    if (!getter) getter = LLVMAddFunction(target,
        "builtin.gc.alloc.mutator_for_native", getter_type);
    if (!getter) {
        LLVMDisposeBuilder(builder);
        free(arguments);
        return backend_error(diagnostics,
            "LLVM backend could not declare mutator getter");
    }
    LLVMValueRef mutator = LLVMBuildCall2(builder, getter_type, getter,
        nullptr, 0, "mutator");
    LLVMBasicBlockRef missing = LLVMAppendBasicBlockInContext(context,
        wrapper, "mutator.missing");
    LLVMBasicBlockRef ready = LLVMAppendBasicBlockInContext(context,
        wrapper, "mutator.ready");
    LLVMBuildCondBr(builder, LLVMBuildIsNull(builder, mutator, "no.mutator"),
        missing, ready);
    LLVMPositionBuilderAtEnd(builder, missing);
    unsigned trap_id = LLVMLookupIntrinsicID("llvm.trap",
        sizeof("llvm.trap") - 1);
    LLVMValueRef trap = LLVMGetIntrinsicDeclaration(target, trap_id,
        nullptr, 0);
    LLVMBuildCall2(builder, LLVMGlobalGetValueType(trap), trap, nullptr, 0,
        "");
    LLVMBuildUnreachable(builder);
    LLVMPositionBuilderAtEnd(builder, ready);
    arguments[count] = mutator;
    LLVMTypeRef signature = LLVMGlobalGetValueType(body);
    bool returns_void = LLVMGetTypeKind(LLVMGetReturnType(signature))
        == LLVMVoidTypeKind;
    LLVMValueRef result = LLVMBuildCall2(builder, signature, body,
        arguments, count + 1, returns_void ? "" : "result");
    if (returns_void) LLVMBuildRetVoid(builder);
    else LLVMBuildRet(builder, result);
    LLVMDisposeBuilder(builder);
    free(arguments);
    return true;
}

static ir_value_t *
frame_stable_root_source(lower_state_t *state, ir_value_t *value)
{
    // A phi root may be overwritten on the next iteration; other SSA roots stay stable.
    while (value->kind == IR_VAL_INSTR && value->def) {
        ir_instr_t *definition = value->def;
        if ((definition->op != IR_OP_COPY
             && definition->op != IR_OP_BITCAST
             && definition->op != IR_OP_FIELD
             && definition->op != IR_OP_OPTION_SOME
             && definition->op != IR_OP_OPTION_GET)
            || !definition->n_operands) break;
        ir_value_t *source = ir_operand_value(&definition->operands[0]);
        if (!source || source == value
            || (source->kind != IR_VAL_PARAM
                && source->kind != IR_VAL_INSTR)
            || (source->kind == IR_VAL_INSTR
                && (!source->def || source->def->op == IR_OP_PHI))
            || !n00bb_frame_native_pointer(state->function->owner,
                                           source->type)
            || ir_storage_origin_unmanaged(state->storage_origin, source))
            break;
        value = source;
    }
    return value;
}

static bool
frame_root_needs_slot(lower_state_t *state, n00bb_frame_root_kind_t kind,
                      ir_value_t *value)
{
    return (kind == N00BB_FRAME_ROOT_PARAM
            || kind == N00BB_FRAME_ROOT_INSTR)
        && value && n00bb_frame_native_pointer(state->function->owner,
                                               value->type)
        && !(kind == N00BB_FRAME_ROOT_INSTR && value->def
             && value->def->op == IR_OP_OPTION_NONE)
        && !ir_storage_origin_unmanaged(state->storage_origin, value)
        && (kind != N00BB_FRAME_ROOT_INSTR
            || frame_stable_root_source(state, value) == value);
}

typedef struct {
    lower_state_t *state;
    size_t count;
    bool possible;
} frame_root_count_t;

static bool
count_frame_root(void *opaque, n00bb_frame_root_kind_t kind,
                 size_t index, ir_value_t *value, ir_local_t *local,
                 ir_instr_t *instruction)
{
    (void)index;
    (void)local;
    (void)instruction;
    frame_root_count_t *count = opaque;
    if ((kind != N00BB_FRAME_ROOT_PARAM
         && kind != N00BB_FRAME_ROOT_INSTR)
        || !value
        || !n00bb_frame_native_pointer(count->state->function->owner,
                                       value->type))
        count->possible = false;
    else if (frame_root_needs_slot(count->state, kind, value))
        count->count++;
    return true;
}

static uint64_t
frame_root_hash(void *key)
{
    return core_hash_raw(&key, sizeof(key));
}

static bool
frame_root_equal(const void *left, const void *right)
{
    return left == right;
}

static bool
frame_root_slot(void *opaque, n00bb_frame_root_kind_t kind,
                size_t index, ir_value_t *value, ir_local_t *local,
                ir_instr_t *instruction)
{
    (void)local;
    (void)instruction;
    lower_state_t *state = opaque;
    if (!frame_root_needs_slot(state, kind, value)) return true;
    index = state->frame_slot_count++;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef slot = LLVMBuildAlloca(state->builder, pointer, "gc.root");
    if (!slot) return false;
    LLVMValueRef empty = LLVMBuildStore(state->builder,
        LLVMConstNull(pointer), slot);
    LLVMSetVolatile(empty, true);
    unsigned init_kind = LLVMGetMDKindIDInContext(context, gc_root_init_metadata,
        sizeof(gc_root_init_metadata) - 1);
    LLVMSetMetadata(empty, init_kind, LLVMMetadataAsValue(context,
        LLVMMDNodeInContext2(context, nullptr, 0)));
    const core_indexed_entry_t *entry = core_indexed_table_intern(
        state->frame_roots, value, nullptr, nullptr);
    if (!entry || entry->index != index) return false;
    state->frame_slots[index] = slot;
    if (kind == N00BB_FRAME_ROOT_PARAM) {
        LLVMValueRef parameter = lowered_value(state, value);
        if (!parameter || LLVMTypeOf(parameter) != pointer) return false;
        LLVMValueRef write = LLVMBuildStore(state->builder, parameter, slot);
        LLVMSetVolatile(write, true);
    }
    return true;
}

static bool
mark_conservative_frame(lower_state_t *state)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef parameters[] = {LLVMInt64TypeInContext(context),
                                LLVMInt32TypeInContext(context)};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                            parameters, 2, true);
    LLVMValueRef intrinsic = LLVMGetNamedFunction(state->module,
        "llvm.experimental.stackmap");
    if (!intrinsic) intrinsic = LLVMAddFunction(state->module,
        "llvm.experimental.stackmap", signature);
    if (!intrinsic) return false;
    LLVMValueRef arguments[] = {
        LLVMConstInt(parameters[0], N00BB_LLVM_FRAME_CONSERVATIVE_ID, false),
        LLVMConstInt(parameters[1], 0, false),
    };
    return LLVMBuildCall2(state->builder, signature, intrinsic, arguments,
                          2, "") != nullptr;
}

static bool
prepare_frame_map(lower_state_t *state)
{
    frame_root_count_t selected = {.state = state, .possible = true};
    size_t total = 0;
    for (uint32_t i = 0; i < state->function->n_locals; i++) {
        ir_local_t *local = state->function->locals[i];
        if (!local || !local->type) return false;
        ir_class_t category = ir_machine_of(state->function->owner,
                                             local->type).class;
        if (category != IR_CLASS_BOOL && category != IR_CLASS_SINT
            && category != IR_CLASS_UINT && category != IR_CLASS_FLOAT)
            return mark_conservative_frame(state);
    }
    if (!n00bb_frame_roots(state->function, count_frame_root,
                           &selected, &total)) return false;
    if (!selected.possible) return mark_conservative_frame(state);
    if (!selected.count) return true;
    size_t count = selected.count;
    if (count > UINT32_MAX - 2) return false;
    state->frame_roots = core_indexed_table_new(0, UINT32_MAX,
        frame_root_hash, frame_root_equal);
    state->frame_slots = calloc(count, sizeof(*state->frame_slots));
    if (!state->frame_roots || !state->frame_slots) return false;
    if (!n00bb_frame_roots(state->function, frame_root_slot, state, &total)
        || state->frame_slot_count != count) return false;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef parameters[] = {LLVMInt64TypeInContext(context),
                                LLVMInt32TypeInContext(context)};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                            parameters, 2, true);
    LLVMValueRef intrinsic = LLVMGetNamedFunction(state->module,
        "llvm.experimental.stackmap");
    if (!intrinsic) intrinsic = LLVMAddFunction(state->module,
        "llvm.experimental.stackmap", signature);
    if (!intrinsic) return false;
    core_buffer_t name = {0};
    core_buffer_printf(&name, "%s.%s",
        ir_module_name(state->function->owner), state->function->name);
    uint64_t id = core_hash_raw(name.data, name.byte_len);
    if (id == N00BB_LLVM_FRAME_SENTINEL_ID
        || id == N00BB_LLVM_FRAME_CONSERVATIVE_ID) id = 0;
    core_buffer_release(&name);
    LLVMValueRef *arguments = calloc(count + 2, sizeof(*arguments));
    if (!arguments) return false;
    arguments[0] = LLVMConstInt(parameters[0], id, false);
    arguments[1] = LLVMConstInt(parameters[1], 0, false);
    memcpy(arguments + 2, state->frame_slots,
           count * sizeof(*state->frame_slots));
    LLVMValueRef call = LLVMBuildCall2(state->builder, signature, intrinsic,
                                      arguments, (unsigned)(count + 2), "");
    free(arguments);
    return call != nullptr;
}

static bool
lower_function(LLVMModuleRef target, const ir_module_t *source,
               ir_func_t *function,
               bool with_mutator,
               bool emit_frame_maps,
               const core_list_t(inline_function_ptr_t) *mutator_functions,
               const n00bb_llvm_phase_array_t *phase_arrays,
               size_t phase_array_count,
               const n00bb_llvm_type_index_t *type_indices,
               size_t type_index_count,
               const n00bb_llvm_function_binding_t *function_indices,
               size_t function_index_count,
               const n00bb_llvm_global_binding_t *global_bindings,
               size_t global_binding_count,
               ir_storage_origin_t *storage_origin,
               core_list_t(layout_match_t) *layout_matches,
               core_list_t(member_match_t) *member_matches,
               core_diagnostics_t *diagnostics)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    core_buffer_t symbol = {0};
    core_buffer_printf(&symbol, "%s.%s", ir_module_name(function->owner),
        function->name);
    LLVMValueRef target_function = with_mutator
        ? mutator_function(target, function)
        : LLVMGetNamedFunction(target, symbol.data);
    core_buffer_release(&symbol);
    size_t input_count = 0;
    size_t optional_count = 0;
    for (size_t i = 0; i < th_func_nparams(function->type); i++) {
        const th_param *formal = th_func_param(function->type, i);
        if (formal && th_mode_accepts_input(formal->mode)) input_count++;
        if (ir_func_parameter_is_optional(function, i)) optional_count++;
    }
    size_t native_count = th_func_nparams(function->type) - optional_count
        + (optional_count != 0);
    if (!target_function || !function->n_blocks
        || function->n_params != input_count
        || native_count + (with_mutator ? 1 : 0)
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
        .source_module = source,
        .target_function = target_function, .diagnostics = diagnostics,
        .phase_arrays = phase_arrays,
        .phase_array_count = phase_array_count,
        .type_indices = type_indices,
        .type_index_count = type_index_count,
        .function_indices = function_indices,
        .function_index_count = function_index_count,
        .global_bindings = global_bindings,
        .global_binding_count = global_binding_count,
        .mutator_functions = mutator_functions,
        .storage_origin = storage_origin,
        .layout_matches = layout_matches,
        .member_matches = member_matches};
    state.blocks = calloc(function->n_blocks, sizeof(*state.blocks));
    state.block_tails = calloc(function->n_blocks,
                               sizeof(*state.block_tails));
    state.locals = calloc(function->n_locals ? function->n_locals : 1,
        sizeof(*state.locals));
    for (uint32_t i = 0; i < function->n_blocks; i++) {
        state.value_capacity += function->blocks[i]->n_instrs;
    }
    if (with_mutator)
        state.mutator = LLVMGetParam(target_function,
            (unsigned)native_count);
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
        if (emit_frame_maps) ok = prepare_frame_map(&state);
        for (uint32_t i = 0; i < function->n_locals; i++) {
            if (!ok) break;
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
            if (ok && state.frame_roots && instruction->op == IR_OP_PHI
                && (j + 1 == block->n_instrs
                    || block->instrs[j + 1]->op != IR_OP_PHI))
                ok = store_phi_frame_roots(&state, block, j);
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
    core_indexed_table_free(state.frame_roots);
    free(state.frame_slots);
    if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics)))
        backend_error(diagnostics, "could not lower LLVM function");
    return ok;
}

static bool
inline_error(core_diagnostics_t *diagnostics, const ir_func_t *function,
             const char *detail)
{
    if (!diagnostics) return false;
    core_buffer_t message = {0};
    core_buffer_printf(&message, "inline function '%s.%s' %s",
        ir_module_name(function->owner), function->name, detail);
    core_diagnostics_report_at(diagnostics, nullptr, CORE_DIAG_FATAL,
        CORE_STRING_STATIC("N00BB-B0006"),
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

static bool
inline_reaches_self(ir_func_t *current, ir_func_t *start,
                    core_list_t(inline_function_ptr_t) *visited,
                    core_diagnostics_t *diagnostics)
{
    if (inline_list_contains(visited, current)) return true;
    core_list_push(*visited, current);
    for (uint32_t b = 0; b < current->n_blocks; b++) {
        ir_block_t *block = current->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (instruction->op != IR_OP_CALL) continue;
            ir_func_t *callee = direct_call_target(instruction);
            if (!callee && current == start)
                return inline_error(diagnostics, start,
                    "contains an indirect call whose recursion cannot be checked");
            if (!callee) continue;
            if (callee == start)
                return inline_error(diagnostics, start,
                    "participates in direct or indirect recursion");
            if (callee->n_blocks && !inline_reaches_self(callee, start,
                    visited, diagnostics)) return false;
        }
    }
    return true;
}

static bool
collect_inline_callees(ir_func_t *function,
                       core_list_t(inline_function_ptr_t) *collected,
                       core_diagnostics_t *diagnostics)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            for (uint32_t operand = 0; operand < instruction->n_operands;
                 operand++) {
                const ir_operand_t *used = &instruction->operands[operand];
                ir_func_t *value = used->kind == IR_OPD_VALUE && used->value
                    && used->value->kind == IR_VAL_FUNC
                    ? used->value->owner : nullptr;
                if (value && ir_func_is_always_inline(value)
                    && (instruction->op != IR_OP_CALL || operand != 0))
                    return inline_error(diagnostics, value,
                        "cannot be used as a function value");
            }
            ir_func_t *callee = direct_call_target(instruction);
            if (!callee || !ir_func_is_always_inline(callee)
                || inline_list_contains(collected, callee)) continue;
            if (!callee->n_blocks)
                return inline_error(diagnostics, callee,
                    "has no body available at this call site");
            core_list_t(inline_function_ptr_t) visited =
                core_list_new(inline_function_ptr_t);
            bool valid = inline_reaches_self(callee, callee, &visited,
                                             diagnostics);
            core_list_free(visited);
            if (!valid) return false;
            core_list_push(*collected, callee);
            if (!collect_inline_callees(callee, collected, diagnostics))
                return false;
        }
    }
    return true;
}

static bool
has_dynamic_allocation(ir_func_t *function)
{
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_instr_t *instruction = block->instrs[i];
            if (instruction->op == IR_OP_ALLOC
                && !ir_allocation_has_only_local_uses(function, instruction))
                return true;
        }
    }
    return false;
}

static void
collect_mutator_candidates(ir_func_t *function,
                           core_list_t(inline_function_ptr_t) *candidates)
{
    if (!function->n_blocks || inline_list_contains(candidates, function))
        return;
    core_list_push(*candidates, function);
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_func_t *callee = direct_call_target(block->instrs[i]);
            if (callee) collect_mutator_candidates(callee, candidates);
        }
    }
}

static void
select_mutator_functions(
    const core_list_t(inline_function_ptr_t) *candidates,
    core_list_t(inline_function_ptr_t) *selected)
{
    for (size_t f = 0; f < candidates->len; f++) {
        ir_func_t *function = candidates->data[f];
        if (has_dynamic_allocation(function))
            core_list_push(*selected, function);
    }
    bool changed;
    do {
        changed = false;
        for (size_t f = 0; f < candidates->len; f++) {
            ir_func_t *function = candidates->data[f];
            if (inline_list_contains(selected, function)) continue;
            for (uint32_t b = 0; b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                for (uint32_t i = 0; i < block->n_instrs; i++) {
                    ir_func_t *callee = direct_call_target(block->instrs[i]);
                    if (callee && inline_list_contains(selected, callee)) {
                        core_list_push(*selected, function);
                        changed = true;
                        goto next_caller;
                    }
                }
            }
next_caller:;
        }
    } while (changed);
}

static void
mark_always_inline(LLVMContextRef context, LLVMValueRef function)
{
    unsigned kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(context, kind, 0);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, attribute);
}

static bool
lower_module(LLVMModuleRef target, const ir_module_t *source,
             bool emit_frame_maps,
             const n00bb_llvm_phase_array_t *phase_arrays,
             size_t phase_array_count,
             const n00bb_llvm_type_index_t *type_indices,
             size_t type_index_count,
             const n00bb_llvm_function_binding_t *function_indices,
             size_t function_index_count,
             const n00bb_llvm_global_binding_t *global_bindings,
             size_t global_binding_count,
             ir_storage_origin_t *storage_origin,
             ir_func_t *const *reachable, size_t reachable_count,
             bool reachable_bound,
             core_diagnostics_t *diagnostics)
{
    for (size_t i = 0; i < ir_global_count(source); i++) {
        ir_global_t *global = ir_global_at((ir_module_t *)source, i);
        LLVMTypeRef type = value_type((ir_module_t *)source, global->type,
                                     LLVMGetModuleContext(target));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) {
            core_buffer_t message = {0};
            core_buffer_printf(&message,
                "LLVM backend cannot lower global '%s' of type %s",
                global->name,
                th_print(ir_module_types((ir_module_t *)source),
                         global->type));
            bool reported = backend_error(diagnostics, message.data);
            core_buffer_release(&message);
            return reported;
        }
        bool variant = false;
        if (th_named_info(th_resolve(global->type), nullptr, nullptr,
                          &variant, nullptr) && variant) {
            const ir_final_layout_t *layout = ir_layout_final(
                (ir_module_t *)source, global->type,
                ir_module_target(source));
            uint64_t stride_bits = 0;
            LLVMTargetDataRef target_data = LLVMGetModuleDataLayout(target);
            if (!layout || !ir_layout_final_stride_bits(layout, &stride_bits)
                || LLVMABISizeOfType(target_data, type) != stride_bits / 8
                || LLVMABIAlignmentOfType(target_data, type)
                    != layout->alignment_bytes)
                return backend_error(diagnostics,
                    "LLVM variant storage does not match its finalized layout");
        }
        LLVMValueRef declared = global_ref(target, global,
            global_bindings, global_binding_count);
        if (!declared)
            return backend_error(diagnostics,
                "LLVM backend could not declare global");
        LLVMSetThreadLocal(declared,
            ir_global_storage_duration(global) == IR_STORAGE_DURATION_THREAD);
        if (global->is_extern) continue;
        LLVMValueRef initialized = global_initialized_ref(target, global,
            global_bindings, global_binding_count);
        if (!initialized)
            return backend_error(diagnostics,
                "LLVM backend could not declare global initialization state");
        LLVMSetInitializer(initialized,
            LLVMConstInt(LLVMGlobalGetValueType(initialized),
                global->has_static_initializer ? 1 : 0, false));
        LLVMSetThreadLocal(initialized,
            ir_global_storage_duration(global) == IR_STORAGE_DURATION_THREAD);
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
        if (ir_func_is_always_inline(function))
            mark_always_inline(LLVMGetModuleContext(target), declared);
    }
    core_list_t(inline_function_ptr_t) inline_callees =
        core_list_new(inline_function_ptr_t);
    bool inline_ok = true;
    for (size_t i = 0; inline_ok && i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        bool emit_body = !reachable_bound;
        for (size_t j = 0; !emit_body && j < reachable_count; j++)
            emit_body = reachable[j] == function;
        if (!function->n_blocks || !emit_body) continue;
        if (ir_func_is_always_inline(function)) {
            core_list_t(inline_function_ptr_t) visited =
                core_list_new(inline_function_ptr_t);
            inline_ok = inline_reaches_self(function, function, &visited,
                                            diagnostics);
            core_list_free(visited);
        }
        if (inline_ok)
            inline_ok = collect_inline_callees(function, &inline_callees,
                                                diagnostics);
    }
    bool needs_allocation_callee = false;
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        bool emit_body = !reachable_bound;
        for (size_t j = 0; !emit_body && j < reachable_count; j++)
            emit_body = reachable[j] == function;
        if (emit_body && has_dynamic_allocation(function))
            needs_allocation_callee = true;
    }
    for (size_t i = 0; i < inline_callees.len; i++)
        if (has_dynamic_allocation(inline_callees.data[i]))
            needs_allocation_callee = true;
    if (inline_ok && needs_allocation_callee && reachable_bound) {
        for (size_t i = 0; i < reachable_count; i++) {
            ir_func_t *function = reachable[i];
            if (strcmp(ir_module_name(function->owner), "builtin.gc.alloc")
                || strcmp(function->name,
                    "allocate_runtime_known_on_mutator")
                || function->owner == source
                || inline_list_contains(&inline_callees, function))
                continue;
            if (ir_func_is_always_inline(function) && function->n_blocks) {
                core_list_push(inline_callees, function);
                inline_ok = collect_inline_callees(function, &inline_callees,
                                                    diagnostics);
            }
            break;
        }
    }
    for (size_t i = 0; inline_ok && i < inline_callees.len; i++) {
        ir_func_t *function = inline_callees.data[i];
        if (function->owner == source) continue;
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "%s.%s", ir_module_name(function->owner),
                           function->name);
        LLVMValueRef declared = LLVMGetNamedFunction(target, symbol.data);
        if (!declared) {
            LLVMTypeRef signature = function_type(target, function);
            declared = signature
                ? LLVMAddFunction(target, symbol.data, signature) : nullptr;
        }
        core_buffer_release(&symbol);
        if (!declared) {
            inline_ok = backend_error(diagnostics,
                "LLVM backend could not declare inline function");
            break;
        }
        LLVMSetLinkage(declared, LLVMAvailableExternallyLinkage);
        mark_always_inline(LLVMGetModuleContext(target), declared);
    }
    if (!inline_ok) {
        core_list_free(inline_callees);
        return false;
    }
    core_list_t(inline_function_ptr_t) candidates =
        core_list_new(inline_function_ptr_t);
    core_list_t(inline_function_ptr_t) mutator_functions =
        core_list_new(inline_function_ptr_t);
    for (size_t i = 0; i < ir_func_count(source); i++)
        collect_mutator_candidates(ir_func_at((ir_module_t *)source, i),
                                   &candidates);
    for (size_t i = 0; i < inline_callees.len; i++)
        collect_mutator_candidates(inline_callees.data[i], &candidates);
    select_mutator_functions(&candidates, &mutator_functions);
    core_list_free(candidates);
    for (size_t i = 0; i < mutator_functions.len; i++) {
        ir_func_t *function = mutator_functions.data[i];
        LLVMTypeRef signature = mutator_function_type(target, function);
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s.%s$gc_mutator",
            ir_module_name(function->owner), function->name);
        LLVMValueRef declared = signature
            ? LLVMGetNamedFunction(target, name.data) : nullptr;
        if (!declared && signature)
            declared = LLVMAddFunction(target, name.data, signature);
        core_buffer_release(&name);
        if (!declared) {
            core_list_free(mutator_functions);
            core_list_free(inline_callees);
            return backend_error(diagnostics,
                "LLVM backend could not declare mutator entry");
        }
        if (function->owner != source
            && inline_list_contains(&inline_callees, function))
            LLVMSetLinkage(declared, LLVMAvailableExternallyLinkage);
        if (ir_func_is_always_inline(function))
            mark_always_inline(LLVMGetModuleContext(target), declared);
    }
    core_list_t(layout_match_t) layout_matches = {0};
    core_list_t(member_match_t) member_matches = {0};
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        bool emit_body = !reachable_bound;
        for (size_t j = 0; !emit_body && j < reachable_count; j++)
            emit_body = reachable[j] == function;
        bool selected = inline_list_contains(&mutator_functions, function);
        if (function->n_blocks && emit_body
            && ((selected && !lower_mutator_wrapper(target, function,
                                                     diagnostics))
                || !lower_function(target, source, function, selected,
                               emit_frame_maps,
                               &mutator_functions, phase_arrays,
                               phase_array_count, type_indices,
                               type_index_count, function_indices,
                               function_index_count, global_bindings,
                               global_binding_count, storage_origin,
                               &layout_matches,
                               &member_matches,
                               diagnostics))) {
            core_list_free(mutator_functions);
            core_list_free(inline_callees);
            core_list_free(layout_matches);
            core_list_free(member_matches);
            return false;
        }
    }
    for (size_t i = 0; i < inline_callees.len; i++) {
        ir_func_t *function = inline_callees.data[i];
        if (function->owner == source) continue;
        bool selected = inline_list_contains(&mutator_functions, function);
        if ((selected && !lower_mutator_wrapper(target, function,
                                                diagnostics))
            || !lower_function(target, source, function, selected,
                            emit_frame_maps,
                            &mutator_functions, phase_arrays,
                            phase_array_count, type_indices,
                            type_index_count, function_indices,
                            function_index_count, global_bindings,
                            global_binding_count, storage_origin,
                            &layout_matches, &member_matches,
                            diagnostics)) {
            core_list_free(mutator_functions);
            core_list_free(inline_callees);
            core_list_free(layout_matches);
            core_list_free(member_matches);
            return false;
        }
    }
    core_list_free(mutator_functions);
    core_list_free(inline_callees);
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
        if (!root->c_alias) continue;
        if (LLVMGetNamedFunction(target, root->c_alias)
            || !LLVMAddAlias2(target, LLVMGlobalGetValueType(function), 0,
                              function, root->c_alias))
            return backend_error(diagnostics,
                "LLVM backend could not export a native runtime root");
    }
    return true;
}

static bool
set_native_layout(LLVMModuleRef module, const char *cpu,
                  const char *features)
{
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter())
        return false;
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = nullptr;
    char *message = nullptr;
    bool ok = triple && !LLVMGetTargetFromTriple(triple, &target, &message);
    LLVMTargetMachineRef machine = ok ? LLVMCreateTargetMachine(target,
        triple, cpu, features, LLVMCodeGenLevelDefault, LLVMRelocDefault,
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
                           bool debug_enabled, bool native_cpu,
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
    if (native_cpu) {
        backend->cpu_name = LLVMGetHostCPUName();
        backend->cpu_features = LLVMGetHostCPUFeatures();
        if (!backend->cpu_name || !backend->cpu_name[0]
            || !backend->cpu_features) goto fail;
    }
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
    const char *cpu = native_cpu ? backend->cpu_name : "generic";
    const char *features = native_cpu ? backend->cpu_features : "";
    if (!set_native_layout(backend->comptime, cpu, features)
        || !set_native_layout(backend->runtime, cpu, features)) goto fail;
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
    if (backend->cpu_name) LLVMDisposeMessage(backend->cpu_name);
    if (backend->cpu_features) LLVMDisposeMessage(backend->cpu_features);
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
n00bb_llvm_backend_bind_function_indices(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_function_binding_t *functions, size_t count)
{
    if (!backend || backend->runtime_lowered || (count && !functions))
        return false;
    backend->function_indices = functions;
    backend->function_index_count = count;
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

bool
n00bb_llvm_backend_bind_storage_origin(
    n00bb_llvm_backend_t *backend, ir_storage_origin_t *origin)
{
    if (!backend || backend->runtime_lowered) return false;
    backend->storage_origin = origin;
    return true;
}

bool
n00bb_llvm_backend_enable_frame_maps(n00bb_llvm_backend_t *backend)
{
    if (!backend || backend->runtime_lowered) return false;
    backend->emit_frame_maps = true;
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
                      !comptime && backend->emit_frame_maps,
                      comptime ? nullptr : backend->phase_arrays,
                      comptime ? 0 : backend->phase_array_count,
                      comptime ? nullptr : backend->type_indices,
                      comptime ? 0 : backend->type_index_count,
                      comptime ? nullptr : backend->function_indices,
                      comptime ? 0 : backend->function_index_count,
                      comptime ? nullptr : backend->global_bindings,
                      comptime ? 0 : backend->global_binding_count,
                      comptime ? nullptr : backend->storage_origin,
                      comptime ? backend->comptime_reachable
                               : backend->runtime_reachable,
                      comptime ? backend->comptime_reachable_count
                               : backend->runtime_reachable_count,
                      comptime ? backend->comptime_reachable_bound
                               : backend->runtime_reachable_bound,
                      diagnostics))
        return false;
    if (!comptime) {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        LLVMErrorRef error = LLVMRunPasses(backend->runtime, "always-inline",
                                           nullptr, options);
        LLVMDisposePassBuilderOptions(options);
        if (error) {
            char *message = LLVMGetErrorMessage(error);
            backend_error(diagnostics, message ? message
                : "LLVM always-inline pass failed");
            LLVMDisposeErrorMessage(message);
            return false;
        }
    }
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
emit_tls_address_resolver(n00bb_llvm_backend_t *backend,
                          core_diagnostics_t *diagnostics)
{
    LLVMModuleRef module = backend->runtime;
    LLVMContextRef context = backend->context;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef signature = LLVMFunctionType(pointer, &i32, 1, false);
    LLVMValueRef resolver = LLVMAddFunction(module,
        "n00bb_abi_tls_address_current_thread", signature);
    if (!resolver || LLVMCountBasicBlocks(resolver))
        return backend_error(diagnostics,
            "could not define the TLS storage address resolver");
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(context,
        resolver, "entry");
    LLVMBasicBlockRef missing = LLVMAppendBasicBlockInContext(context,
        resolver, "unbound");
    LLVMPositionBuilderAtEnd(builder, entry);
    LLVMValueRef branch = LLVMBuildSwitch(builder, LLVMGetParam(resolver, 0),
        missing, (unsigned)backend->global_binding_count);
    bool valid = true;
    for (size_t i = 0; valid && i < backend->global_binding_count; i++) {
        const n00bb_llvm_global_binding_t *binding =
            &backend->global_bindings[i];
        if (ir_global_storage_duration(binding->global)
            != IR_STORAGE_DURATION_THREAD) continue;
        LLVMValueRef global = global_ref(module, binding->global,
            backend->global_bindings, backend->global_binding_count);
        if (!global || !binding->storage_index) {
            valid = false;
            break;
        }
        LLVMSetThreadLocal(global, true);
        LLVMBasicBlockRef body = LLVMAppendBasicBlockInContext(context,
            resolver, "tls.address");
        LLVMAddCase(branch,
            LLVMConstInt(i32, binding->storage_index, false), body);
        LLVMPositionBuilderAtEnd(builder, body);
        LLVMBuildRet(builder, global);
    }
    LLVMPositionBuilderAtEnd(builder, missing);
    LLVMBuildRet(builder, LLVMConstPointerNull(pointer));
    LLVMDisposeBuilder(builder);
    return valid || backend_error(diagnostics,
        "could not bind a TLS storage address");
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

static bool
is_stack_map_call(LLVMValueRef instruction)
{
    if (LLVMGetInstructionOpcode(instruction) != LLVMCall) return false;
    LLVMValueRef callee = LLVMGetCalledValue(instruction);
    size_t length = 0;
    const char *name = callee ? LLVMGetValueName2(callee, &length) : nullptr;
    return name && length == sizeof("llvm.experimental.stackmap") - 1
        && !memcmp(name, "llvm.experimental.stackmap", length);
}

static bool
is_conservative_frame_marker(LLVMValueRef instruction)
{
    if (!is_stack_map_call(instruction)
        || LLVMGetNumArgOperands(instruction) != 2) return false;
    LLVMValueRef id = LLVMGetOperand(instruction, 0);
    return LLVMIsAConstantInt(id)
        && LLVMConstIntGetZExtValue(id)
            == N00BB_LLVM_FRAME_CONSERVATIVE_ID;
}

static bool
is_root_lifetime_call(LLVMValueRef instruction)
{
    if (LLVMGetInstructionOpcode(instruction) != LLVMCall) return false;
    LLVMValueRef callee = LLVMGetCalledValue(instruction);
    size_t length = 0;
    const char *name = callee ? LLVMGetValueName2(callee, &length) : nullptr;
    const char start[] = "llvm.lifetime.start.";
    const char end[] = "llvm.lifetime.end.";
    return name && ((length >= sizeof(start) - 1
                     && !memcmp(name, start, sizeof(start) - 1))
                    || (length >= sizeof(end) - 1
                        && !memcmp(name, end, sizeof(end) - 1)));
}

static bool
llvm_value_list_contains(const core_list_t(LLVMValueRef) *values,
                         LLVMValueRef value)
{
    for (size_t i = 0; i < values->len; i++)
        if (values->data[i] == value) return true;
    return false;
}

static bool
null_only_frame_root(LLVMValueRef slot)
{
    if (LLVMGetValueKind(slot) != LLVMInstructionValueKind
        || LLVMGetInstructionOpcode(slot) != LLVMAlloca) return false;
    bool stored = false, mapped = false;
    for (LLVMUseRef use = LLVMGetFirstUse(slot); use;
         use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMGetValueKind(user) != LLVMInstructionValueKind) return false;
        if (LLVMGetInstructionOpcode(user) == LLVMStore
            && LLVMGetOperand(user, 1) == slot
            && LLVMIsNull(LLVMGetOperand(user, 0))) {
            stored = true;
            continue;
        }
        if (is_stack_map_call(user)) {
            mapped = true;
            continue;
        }
        if (!is_root_lifetime_call(user)) return false;
    }
    return stored && mapped;
}

static bool
prune_null_frame_roots(LLVMModuleRef module, LLVMBuilderRef builder)
{
    LLVMValueRef map = LLVMGetNamedFunction(module,
        "llvm.experimental.stackmap");
    if (!map) return true;
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
         function = LLVMGetNextFunction(function)) {
        core_list_t(LLVMValueRef) dead = core_list_new(LLVMValueRef);
        if (!dead.data) return false;
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function); block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction; instruction = LLVMGetNextInstruction(instruction)) {
                if (!is_stack_map_call(instruction)) continue;
                unsigned count = LLVMGetNumArgOperands(instruction);
                for (unsigned i = 2; i < count; i++) {
                    LLVMValueRef slot = LLVMGetOperand(instruction, i);
                    if (!llvm_value_list_contains(&dead, slot)
                        && null_only_frame_root(slot))
                        core_list_push(dead, slot);
                }
            }
        if (!dead.len) { core_list_free(dead); continue; }
        bool ok = true;
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function);
             ok && block; block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 ok && instruction;) {
                LLVMValueRef next = LLVMGetNextInstruction(instruction);
                if (is_stack_map_call(instruction)) {
                    unsigned count = LLVMGetNumArgOperands(instruction);
                    LLVMValueRef *args = malloc((size_t)count * sizeof(*args));
                    if (!args) { ok = false; break; }
                    unsigned kept = 0;
                    for (unsigned i = 0; i < count; i++) {
                        LLVMValueRef operand = LLVMGetOperand(instruction, i);
                        if (i < 2 || !llvm_value_list_contains(&dead, operand))
                            args[kept++] = operand;
                    }
                    if (kept != count && kept > 2) {
                        LLVMPositionBuilderBefore(builder, instruction);
                        ok = LLVMBuildCall2(builder,
                            LLVMGlobalGetValueType(map), map, args, kept,
                            "") != nullptr;
                    }
                    free(args);
                    if (ok && kept != count)
                        LLVMInstructionEraseFromParent(instruction);
                } else if (LLVMGetInstructionOpcode(instruction) == LLVMStore
                           && llvm_value_list_contains(&dead,
                               LLVMGetOperand(instruction, 1)))
                    LLVMInstructionEraseFromParent(instruction);
                else if (is_root_lifetime_call(instruction)
                         && llvm_value_list_contains(&dead,
                             LLVMGetOperand(instruction, 0)))
                    LLVMInstructionEraseFromParent(instruction);
                instruction = next;
            }
        for (size_t i = 0; ok && i < dead.len; i++) {
            if (LLVMGetFirstUse(dead.data[i])) { ok = false; break; }
            LLVMInstructionEraseFromParent(dead.data[i]);
        }
        core_list_free(dead);
        if (!ok) return false;
    }
    return true;
}

typedef struct {
    LLVMValueRef slot;
    LLVMValueRef value;
} frame_single_value_root_t;
core_list_decl(frame_single_value_root_t);

static LLVMValueRef
frame_root_single_value(LLVMValueRef slot, LLVMBasicBlockRef entry,
                        unsigned init_kind)
{
    if (LLVMGetValueKind(slot) != LLVMInstructionValueKind
        || LLVMGetInstructionOpcode(slot) != LLVMAlloca
        || LLVMGetInstructionParent(slot) != entry) return nullptr;
    LLVMValueRef value = nullptr;
    bool mapped = false;
    for (LLVMUseRef use = LLVMGetFirstUse(slot); use;
         use = LLVMGetNextUse(use)) {
        LLVMValueRef user = LLVMGetUser(use);
        if (LLVMGetValueKind(user) != LLVMInstructionValueKind) return nullptr;
        if (LLVMGetInstructionOpcode(user) == LLVMStore
            && LLVMGetOperand(user, 1) == slot) {
            LLVMValueRef stored = LLVMGetOperand(user, 0);
            if (LLVMIsNull(stored)) {
                if (!LLVMGetMetadata(user, init_kind)) return nullptr;
                continue;
            }
            if (value && value != stored) return nullptr;
            value = stored;
        } else if (is_stack_map_call(user)) mapped = true;
        else if (!is_root_lifetime_call(user)) return nullptr;
    }
    return mapped ? value : nullptr;
}

static bool
merge_single_value_frame_roots(LLVMModuleRef module, LLVMBuilderRef builder)
{
    LLVMContextRef context = LLVMGetModuleContext(module);
    unsigned init_kind = LLVMGetMDKindIDInContext(context,
        gc_root_init_metadata, sizeof(gc_root_init_metadata) - 1);
    LLVMValueRef map = LLVMGetNamedFunction(module,
        "llvm.experimental.stackmap");
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
         function = LLVMGetNextFunction(function)) {
        LLVMBasicBlockRef entry = LLVMGetFirstBasicBlock(function);
        if (!entry) continue;
        core_list_t(frame_single_value_root_t) roots =
            core_list_new(frame_single_value_root_t);
        core_list_t(frame_single_value_root_t) merges =
            core_list_new(frame_single_value_root_t);
        if (!roots.data || !merges.data) {
            core_list_free(roots);
            core_list_free(merges);
            return false;
        }
        for (LLVMBasicBlockRef block = entry; block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction; instruction = LLVMGetNextInstruction(instruction)) {
                if (!is_stack_map_call(instruction)) continue;
                unsigned count = LLVMGetNumArgOperands(instruction);
                for (unsigned i = 2; i < count; i++) {
                    LLVMValueRef slot = LLVMGetOperand(instruction, i);
                    LLVMValueRef value = frame_root_single_value(slot, entry,
                                                                  init_kind);
                    if (!value) continue;
                    bool seen = false;
                    for (size_t j = 0; j < roots.len; j++)
                        if (roots.data[j].slot == slot) seen = true;
                    if (seen) continue;
                    LLVMValueRef canonical = nullptr;
                    for (size_t j = 0; j < roots.len; j++)
                        if (roots.data[j].value == value) {
                            canonical = roots.data[j].slot;
                            break;
                        }
                    core_list_push(roots, ((frame_single_value_root_t){
                        .slot = slot, .value = value,
                    }));
                    if (canonical) core_list_push(merges,
                        ((frame_single_value_root_t){
                            .slot = slot, .value = canonical,
                        }));
                }
            }
        for (size_t i = 0; i < merges.len; i++) {
            LLVMValueRef duplicate = merges.data[i].slot;
            LLVMValueRef canonical = merges.data[i].value;
            for (LLVMBasicBlockRef block = entry; block;
                 block = LLVMGetNextBasicBlock(block))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                     instruction;) {
                    LLVMValueRef next = LLVMGetNextInstruction(instruction);
                    if (is_stack_map_call(instruction)) {
                        unsigned count = LLVMGetNumArgOperands(instruction);
                        for (unsigned arg = 2; arg < count; arg++)
                            if (LLVMGetOperand(instruction, arg) == duplicate)
                                LLVMSetOperand(instruction, arg, canonical);
                    } else if (LLVMGetInstructionOpcode(instruction)
                               == LLVMStore
                               && LLVMGetOperand(instruction, 1)
                                    == duplicate) {
                        if (LLVMIsNull(LLVMGetOperand(instruction, 0)))
                            LLVMInstructionEraseFromParent(instruction);
                        else LLVMSetOperand(instruction, 1, canonical);
                    } else if (is_root_lifetime_call(instruction)
                               && LLVMGetOperand(instruction, 0)
                                    == duplicate)
                        LLVMInstructionEraseFromParent(instruction);
                    instruction = next;
                }
            if (LLVMGetFirstUse(duplicate)) {
                core_list_free(roots);
                core_list_free(merges);
                return false;
            }
            LLVMInstructionEraseFromParent(duplicate);
        }
        for (size_t i = 0; i < roots.len; i++) {
            LLVMValueRef slot = roots.data[i].slot;
            bool merged = false;
            for (size_t j = 0; j < merges.len; j++)
                if (merges.data[j].slot == slot) merged = true;
            if (merged) continue;
            bool canonical = false;
            for (size_t j = 0; j < merges.len; j++)
                if (merges.data[j].value == slot) canonical = true;
            if (!canonical) continue;
            if (LLVMGetValueKind(slot) != LLVMInstructionValueKind) continue;
            for (LLVMBasicBlockRef block = entry; block;
                 block = LLVMGetNextBasicBlock(block)) {
                LLVMValueRef last = nullptr;
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                     instruction;) {
                    LLVMValueRef next = LLVMGetNextInstruction(instruction);
                    if (LLVMGetInstructionOpcode(instruction) == LLVMStore
                        && LLVMGetOperand(instruction, 1) == slot) {
                        LLVMValueRef value = LLVMGetOperand(instruction, 0);
                        if (last == value)
                            LLVMInstructionEraseFromParent(instruction);
                        else last = value;
                    }
                    instruction = next;
                }
            }
        }
        if (merges.len) {
            for (LLVMBasicBlockRef block = entry; block;
                 block = LLVMGetNextBasicBlock(block))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                     instruction;) {
                    LLVMValueRef next = LLVMGetNextInstruction(instruction);
                    if (is_stack_map_call(instruction)) {
                        unsigned count = LLVMGetNumArgOperands(instruction);
                        LLVMValueRef *args = malloc((size_t)count * sizeof(*args));
                        if (!args) {
                            core_list_free(roots);
                            core_list_free(merges);
                            return false;
                        }
                        unsigned kept = 0;
                        for (unsigned arg = 0; arg < count; arg++) {
                            LLVMValueRef value = LLVMGetOperand(instruction, arg);
                            bool seen = false;
                            for (unsigned prior = 2; prior < kept; prior++)
                                if (args[prior] == value) seen = true;
                            if (arg < 2 || !seen) args[kept++] = value;
                        }
                        if (kept < count) {
                            LLVMPositionBuilderBefore(builder, instruction);
                            LLVMValueRef replacement = LLVMBuildCall2(builder,
                                LLVMGlobalGetValueType(map), map, args, kept,
                                "");
                            if (!replacement) {
                                free(args);
                                core_list_free(roots);
                                core_list_free(merges);
                                return false;
                            }
                            LLVMInstructionEraseFromParent(instruction);
                        }
                        free(args);
                    }
                    instruction = next;
                }
        }
        core_list_free(roots);
        core_list_free(merges);
    }
    return true;
}

static bool
discard_unpublished_frame_roots(LLVMValueRef function, unsigned init_kind)
{
    core_list_t(LLVMValueRef) roots = core_list_new(LLVMValueRef);
    if (!roots.data) return false;
    bool valid = true;
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function);
         valid && block; block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
             instruction; instruction = LLVMGetNextInstruction(instruction)) {
            if (is_stack_map_call(instruction)) {
                unsigned count = LLVMGetNumArgOperands(instruction);
                for (unsigned i = 2; i < count; i++) {
                    LLVMValueRef slot = LLVMGetOperand(instruction, i);
                    if (!llvm_value_list_contains(&roots, slot))
                        core_list_push(roots, slot);
                }
            } else if (LLVMGetInstructionOpcode(instruction) == LLVMStore
                       && LLVMGetMetadata(instruction, init_kind)) {
                LLVMValueRef slot = LLVMGetOperand(instruction, 1);
                if (!llvm_value_list_contains(&roots, slot))
                    core_list_push(roots, slot);
            }
        }
    for (size_t i = 0; valid && i < roots.len; i++) {
        LLVMValueRef slot = roots.data[i];
        valid = LLVMGetValueKind(slot) == LLVMInstructionValueKind
            && LLVMGetInstructionOpcode(slot) == LLVMAlloca
            && LLVMGetTypeKind(LLVMGetAllocatedType(slot))
                == LLVMPointerTypeKind;
        for (LLVMUseRef use = valid ? LLVMGetFirstUse(slot) : nullptr;
             use; use = LLVMGetNextUse(use)) {
            LLVMValueRef user = LLVMGetUser(use);
            if (LLVMGetValueKind(user) != LLVMInstructionValueKind
                || (!is_stack_map_call(user)
                    && !(LLVMGetInstructionOpcode(user) == LLVMStore
                         && LLVMGetOperand(user, 1) == slot)
                    && !(is_root_lifetime_call(user)
                         && LLVMGetOperand(user, 0) == slot))) {
                valid = false;
                break;
            }
        }
    }
    if (valid)
        for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function);
             block; block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction;) {
                LLVMValueRef next = LLVMGetNextInstruction(instruction);
                if (is_stack_map_call(instruction)
                    || (LLVMGetInstructionOpcode(instruction) == LLVMStore
                        && llvm_value_list_contains(&roots,
                            LLVMGetOperand(instruction, 1)))
                    || (is_root_lifetime_call(instruction)
                        && llvm_value_list_contains(&roots,
                            LLVMGetOperand(instruction, 0))))
                    LLVMInstructionEraseFromParent(instruction);
                instruction = next;
            }
    for (size_t i = 0; valid && i < roots.len; i++) {
        if (LLVMGetFirstUse(roots.data[i])) { valid = false; break; }
        LLVMInstructionEraseFromParent(roots.data[i]);
    }
    core_list_free(roots);
    return valid;
}

static bool
emit_frame_sentinels(LLVMModuleRef module,
                     core_diagnostics_t *diagnostics)
{
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMValueRef map = LLVMGetNamedFunction(module,
        "llvm.experimental.stackmap");
    if (!map) return true;
    LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
    if (!builder) return backend_error(diagnostics,
        "could not create frame sentinel builder");
    if (!prune_null_frame_roots(module, builder)) {
        LLVMDisposeBuilder(builder);
        return backend_error(diagnostics,
            "could not prune null-only native frame roots");
    }
    if (!merge_single_value_frame_roots(module, builder)) {
        LLVMDisposeBuilder(builder);
        return backend_error(diagnostics,
            "could not merge equivalent native frame roots");
    }
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    unsigned init_kind = LLVMGetMDKindIDInContext(context,
        gc_root_init_metadata, sizeof(gc_root_init_metadata) - 1);
    bool ok = true;
    for (LLVMValueRef function = LLVMGetFirstFunction(module);
         ok && function; function = LLVMGetNextFunction(function)) {
        LLVMBasicBlockRef entry = LLVMGetFirstBasicBlock(function);
        if (!entry) continue;
        bool conservative = false;
        for (LLVMBasicBlockRef block = entry; block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction; instruction = LLVMGetNextInstruction(instruction))
                if (is_conservative_frame_marker(instruction))
                    conservative = true;
        if (conservative) {
            ok = discard_unpublished_frame_roots(function, init_kind);
            continue;
        }
        core_list_t(LLVMValueRef) roots = core_list_new(LLVMValueRef);
        core_list_t(LLVMValueRef) clears = core_list_new(LLVMValueRef);
        core_list_t(LLVMValueRef) lifetimes = core_list_new(LLVMValueRef);
        if (!roots.data || !clears.data || !lifetimes.data) {
            core_list_free(roots);
            core_list_free(clears);
            core_list_free(lifetimes);
            ok = false;
            break;
        }
        bool possible = true;
        for (LLVMBasicBlockRef block = entry; block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction; instruction = LLVMGetNextInstruction(instruction))
                if (is_stack_map_call(instruction)) {
                    unsigned count = LLVMGetNumArgOperands(instruction);
                    for (unsigned i = 2; i < count; i++) {
                        LLVMValueRef root = LLVMGetOperand(instruction, i);
                        if (LLVMGetValueKind(root) != LLVMInstructionValueKind
                            || LLVMGetInstructionOpcode(root) != LLVMAlloca
                            || LLVMGetInstructionParent(root) != entry
                            || LLVMGetAllocatedType(root) != pointer) {
                            possible = false;
                            continue;
                        }
                        bool found = false;
                        for (size_t j = 0; j < roots.len; j++)
                            if (roots.data[j] == root) found = true;
                        if (!found) core_list_push(roots, root);
                    }
                } else if (LLVMGetInstructionOpcode(instruction) == LLVMStore
                           && LLVMGetMetadata(instruction, init_kind)) {
                    core_list_push(clears, instruction);
                }
        if (!roots.len) possible = false;
        for (size_t i = 0; i < clears.len; i++) {
            LLVMValueRef slot = LLVMGetOperand(clears.data[i], 1);
            bool found = false;
            for (size_t j = 0; j < roots.len; j++)
                if (roots.data[j] == slot) found = true;
            if (!found) possible = false;
        }
        for (size_t i = 0; i < roots.len; i++) {
            bool found = false;
            for (size_t j = 0; j < clears.len; j++)
                if (LLVMGetOperand(clears.data[j], 1) == roots.data[i])
                    found = true;
            if (!found) possible = false;
        }
        if (!possible) {
            core_list_free(roots);
            core_list_free(clears);
            core_list_free(lifetimes);
            ok = discard_unpublished_frame_roots(function, init_kind);
            continue;
        }
        for (LLVMBasicBlockRef block = entry; block;
             block = LLVMGetNextBasicBlock(block))
            for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                 instruction; instruction = LLVMGetNextInstruction(instruction))
                if (is_root_lifetime_call(instruction)) {
                    LLVMValueRef slot = LLVMGetOperand(instruction, 0);
                    for (size_t i = 0; i < roots.len; i++)
                        if (roots.data[i] == slot) {
                            core_list_push(lifetimes, instruction);
                            break;
                        }
                }
        LLVMValueRef last_root = nullptr;
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(entry);
             instruction; instruction = LLVMGetNextInstruction(instruction))
            for (size_t i = 0; i < roots.len; i++)
                if (instruction == roots.data[i]) last_root = instruction;
        if (!last_root) {
            core_list_free(roots);
            core_list_free(clears);
            core_list_free(lifetimes);
            ok = false;
            break;
        }
        LLVMValueRef last_init = nullptr;
        for (size_t i = 0; i < roots.len; i++) {
            LLVMValueRef next = LLVMGetNextInstruction(roots.data[i]);
            if (!next) { ok = false; break; }
            LLVMPositionBuilderBefore(builder, next);
            LLVMValueRef init = LLVMBuildStore(builder,
                LLVMConstNull(pointer), roots.data[i]);
            if (!init) { ok = false; break; }
            LLVMSetVolatile(init, true);
            if (roots.data[i] == last_root) last_init = init;
        }
        if (!ok || !last_init) {
            core_list_free(roots);
            core_list_free(clears);
            core_list_free(lifetimes);
            ok = false;
            break;
        }
        for (size_t i = 0; i < clears.len; i++)
            LLVMInstructionEraseFromParent(clears.data[i]);
        for (size_t i = 0; i < lifetimes.len; i++)
            LLVMInstructionEraseFromParent(lifetimes.data[i]);
        core_list_free(roots);
        core_list_free(clears);
        core_list_free(lifetimes);
        LLVMValueRef next = LLVMGetNextInstruction(last_init);
        if (!next) { ok = false; break; }
        LLVMPositionBuilderBefore(builder, next);
        size_t length = 0;
        const char *function_name = LLVMGetValueName2(function, &length);
        if (!function_name || length > INT_MAX) { ok = false; break; }
        core_buffer_t symbol = {0};
        core_buffer_printf(&symbol, "__n00bb_gc_frame.%.*s",
                           (int)length, function_name);
        LLVMValueRef descriptor = LLVMAddGlobal(module,
            LLVMInt8TypeInContext(context), symbol.data);
        core_buffer_release(&symbol);
        if (!descriptor) { ok = false; break; }
        LLVMValueRef slot = LLVMBuildAlloca(builder, pointer,
                                            "gc.frame.descriptor");
        LLVMValueRef write = LLVMBuildStore(builder, descriptor, slot);
        LLVMSetVolatile(write, true);
        LLVMValueRef args[] = {
            LLVMConstInt(i64, N00BB_LLVM_FRAME_SENTINEL_ID, false),
            LLVMConstInt(i32, 0, false), slot,
        };
        ok = LLVMBuildCall2(builder, LLVMGlobalGetValueType(map), map,
                            args, 3, "") != nullptr;
    }
    LLVMDisposeBuilder(builder);
    return ok || backend_error(diagnostics,
        "could not emit native frame sentinels");
}

static LLVMValueRef
runtime_function_address_table(n00bb_llvm_backend_t *backend,
                               uint32_t *count_out)
{
    LLVMModuleRef module = backend->runtime;
    LLVMTypeRef pointer = LLVMPointerTypeInContext(backend->context, 0);
    uint32_t count = 1;
    for (size_t i = 0; i < backend->function_index_count; i++) {
        uint32_t index = backend->function_indices[i].index;
        if (index == UINT32_MAX) return nullptr;
        if (index >= count) count = index + 1;
    }
    LLVMValueRef *addresses = calloc(count, sizeof(*addresses));
    if (!addresses) return nullptr;
    for (uint32_t i = 0; i < count; i++)
        addresses[i] = LLVMConstNull(pointer);
    for (size_t i = 0; i < backend->function_index_count; i++) {
        ir_func_t *function = backend->function_indices[i].function;
        bool emitted = !backend->runtime_reachable_bound;
        for (size_t j = 0; !emitted && j < backend->runtime_reachable_count;
             j++)
            emitted = backend->runtime_reachable[j] == function;
        if (!emitted) continue;
        LLVMTypeRef signature = function_type(module, function);
        core_buffer_t name = {0};
        core_buffer_printf(&name, "%s.%s", ir_module_name(function->owner),
                           function->name);
        LLVMValueRef target = signature && name.data
            ? LLVMGetNamedFunction(module, name.data) : nullptr;
        if (!target && signature && name.data)
            target = LLVMAddFunction(module, name.data, signature);
        core_buffer_release(&name);
        if (!target || LLVMGlobalGetValueType(target) != signature) {
            free(addresses);
            return nullptr;
        }
        addresses[backend->function_indices[i].index] = target;
    }
    LLVMTypeRef array = LLVMArrayType(pointer, count);
    LLVMValueRef table = LLVMAddGlobal(module, array,
                                        "__n00bb_function_addresses");
    if (table) {
        LLVMSetInitializer(table, LLVMConstArray(pointer, addresses, count));
        LLVMSetGlobalConstant(table, true);
        LLVMSetLinkage(table, LLVMPrivateLinkage);
        *count_out = count;
    }
    free(addresses);
    return table;
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
    uint32_t function_address_count = 0;
    LLVMValueRef function_addresses = !comptime && initializer_count
        ? runtime_function_address_table(backend, &function_address_count)
        : nullptr;
    if (!comptime && initializer_count && !function_addresses)
        return backend_error(diagnostics,
            "could not emit indexed runtime function addresses");
    if (!comptime && initializer_count
        && !emit_tls_address_resolver(backend, diagnostics)) return false;
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
                "n00b_abi_v1_register_linked_image", register_type);
            LLVMValueRef register_args[] = {descriptor,
                LLVMConstPointerNull(pointer), LLVMConstPointerNull(pointer)};
            LLVMValueRef status = LLVMBuildCall2(builder, register_type,
                register_image, register_args, 3, "registration");
            LLVMTypeRef bind_params[] = {pointer, result};
            LLVMTypeRef bind_type = LLVMFunctionType(result, bind_params,
                                                      2, false);
            LLVMValueRef bind = LLVMAddFunction(module,
                "n00b_abi_v1_bind_runtime_function_addresses", bind_type);
            LLVMValueRef bind_args[] = {function_addresses,
                LLVMConstInt(result, function_address_count, false)};
            LLVMValueRef bound = LLVMBuildCall2(builder, bind_type, bind,
                bind_args, 2, "function.addresses");
            LLVMValueRef failed = LLVMBuildICmp(builder, LLVMIntNE, status,
                LLVMConstInt(result, 0, false), "registration.failed");
            LLVMValueRef bind_failed = LLVMBuildICmp(builder, LLVMIntNE,
                bound, LLVMConstNull(result), "function.addresses.failed");
            failed = LLVMBuildOr(builder, failed, bind_failed,
                                 "initialization.failed");
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

    LLVMAttributeRef frame_pointer = LLVMCreateStringAttribute(context,
        "frame-pointer", sizeof("frame-pointer") - 1,
        "all", sizeof("all") - 1);
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
         function = LLVMGetNextFunction(function)) {
        if (LLVMCountBasicBlocks(function))
            LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex,
                                    frame_pointer);
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
        backend->cpu_name ? backend->cpu_name : "generic",
        backend->cpu_features ? backend->cpu_features : "",
        LLVMCodeGenLevelDefault, LLVMRelocDefault,
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
    LLVMPassBuilderOptionsRef pass_options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef pass_error = LLVMRunPasses(module, "default<O2>", machine,
                                            pass_options);
    LLVMDisposePassBuilderOptions(pass_options);
    if (pass_error) {
        char *pass_message = LLVMGetErrorMessage(pass_error);
        backend_error(diagnostics, pass_message ? pass_message
            : "LLVM optimization failed");
        LLVMDisposeErrorMessage(pass_message);
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (!comptime && backend->emit_frame_maps
        && !emit_frame_sentinels(module, diagnostics)) {
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (!comptime && backend->emit_frame_maps
        && LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) {
        backend_error(diagnostics, message ? message
            : "invalid LLVM frame map module");
        if (message) LLVMDisposeMessage(message);
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (message) {
        LLVMDisposeMessage(message);
        message = nullptr;
    }
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
