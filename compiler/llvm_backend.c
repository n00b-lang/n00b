#include "llvm_backend.h"

#include "ir/layout.h"
#include "ir/doms.h"
#include "ir/range.h"
#include "ir/shape.h"
#include "ir/typelang.h"
#include "th/environment.h"
#include "abi_string.h"
#include "frame_roots.h"
#include "llvm_stack_maps.h"
#include "llvm_native_cache.h"
#include "n00b-abi-v1.h"
#include "runtime_native_roots.h"
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/indexed_table.h"
#include "lib/list.h"
#include "lib/string.h"

typedef struct {
    ir_func_t *handler;
    th_type *callback_type;
    th_type *handler_signature;
} callback_binding_t;
core_list_decl(callback_binding_t);

typedef struct {
    ir_module_t *module;
    th_type *type;
    LLVMContextRef context;
} native_type_key_t;

static core_hash_value_t
native_type_key_hash(void *opaque)
{
    const native_type_key_t *key = opaque;
    uintptr_t parts[] = {(uintptr_t)key->module, (uintptr_t)key->type,
                         (uintptr_t)key->context};
    return core_hash_raw(parts, sizeof parts);
}

static bool
native_type_key_eq(const void *left, const void *right)
{
    const native_type_key_t *a = left, *b = right;
    return a->module == b->module && a->type == b->type
        && a->context == b->context;
}

static _Thread_local core_dict_t *active_native_types;

struct n00bb_llvm_backend_t {
    LLVMContextRef context;
    core_dict_t native_types;
    bool native_types_ready;
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
    n00bb_native_cache_t *native_cache;
    core_list_t(callback_binding_t) callback_bindings;
};

typedef struct {
    const ir_value_t *source;
    LLVMValueRef target;
    LLVMValueRef dynamic_type_index;
} lowered_value_t;

typedef struct {
    const ir_instr_t *call;
    uint32_t ordinal;
    LLVMValueRef storage;
    th_type *type;
} lowered_call_output_t;
core_list_decl(lowered_call_output_t);

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
    bool with_mutator;
    LLVMBasicBlockRef *blocks;
    LLVMBasicBlockRef *block_tails;
    LLVMValueRef *locals;
    LLVMValueRef *param_addresses;
    LLVMValueRef mutator;
    const core_list_t(inline_function_ptr_t) *mutator_functions;
    lowered_value_t *values;
    size_t value_count;
    size_t value_capacity;
    core_list_t(lowered_call_output_t) call_outputs;
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
    ir_index_proofs_t *index_proofs;
    core_indexed_table_t *frame_roots;
    LLVMValueRef *frame_slots;
    size_t frame_slot_count;
    core_list_t(layout_match_t) *layout_matches;
    core_list_t(member_match_t) *member_matches;
    const core_list_t(callback_binding_t) *callback_bindings;
    const char *failure_detail;
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
    if (state->failure_detail)
        core_buffer_printf(&message, ": %s", state->failure_detail);
    if (instruction->op == IR_OP_EXTRACT && instruction->n_operands == 2
        && instruction->operands[0].kind == IR_OPD_VALUE
        && instruction->operands[0].value
        && instruction->operands[1].kind == IR_OPD_NAME
        && instruction->operands[1].name)
        core_buffer_printf(&message, "; source %s, field %s", th_print(
            ir_module_types(state->function->owner),
            instruction->operands[0].value->type),
            instruction->operands[1].name);
    if (instruction->op == IR_OP_AGGREGATE && instruction->dest
        && instruction->dest->type)
        core_buffer_printf(&message, "; type %s", th_print(
            ir_module_types(state->function->owner), instruction->dest->type));
    if (instruction->op == IR_OP_TYPE_TEST && instruction->n_operands == 2
        && instruction->operands[0].kind == IR_OPD_VALUE
        && instruction->operands[0].value
        && instruction->operands[1].kind == IR_OPD_TYPE)
        core_buffer_printf(&message, "; subject %s, pattern %s", th_print(
            ir_module_types(state->function->owner),
            instruction->operands[0].value->type), th_print(
            ir_module_types(state->function->owner),
            instruction->operands[1].type));
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
    if (machine.class == IR_CLASS_FLOAT && machine.width == 32)
        return LLVMFloatTypeInContext(context);
    if (machine.class == IR_CLASS_PTR)
        return LLVMPointerTypeInContext(context, 0);
    return nullptr;
}

static bool
floating_kind(LLVMTypeKind kind)
{
    return kind == LLVMFloatTypeKind || kind == LLVMDoubleTypeKind;
}

static bool
floating_type(LLVMTypeRef type)
{
    return type && floating_kind(LLVMGetTypeKind(type));
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

static LLVMTypeRef
bitptr_llvm_type(LLVMContextRef context)
{
    LLVMTypeRef fields[] = {
        LLVMPointerTypeInContext(context, 0),
        LLVMInt32TypeInContext(context),
        LLVMInt16TypeInContext(context),
        LLVMInt16TypeInContext(context),
    };
    return LLVMStructTypeInContext(context, fields, 4, false);
}

static bool
uses_bitptr(ir_module_t *module, th_type *type)
{
    if (th_kind_of(th_resolve(type)) != TH_INDIRECTION) return false;
    th_type *referent = th_ind_referent(type);
    if (referent && ir_machine_of(module, referent).class == IR_CLASS_BOOL)
        return true;
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    return layout && layout->intrinsic_bits == 128;
}

enum {
    N00BB_PACKED_MAX_BITS = 128,
    N00BB_PACKED_SCRATCH_BYTES =
        (N00BB_PACKED_MAX_BITS + 2 * N00B_ABI_ARRAY_ORIGIN_BIT_MASK)
        / CHAR_BIT,
};

static uint64_t
packed_array_width(ir_module_t *module, th_type *type)
{
    if (!type) return 0;
    if (ir_machine_of(module, type).class == IR_CLASS_BOOL) return 1;
    th_type *definition = ir_type_definition(module, type);
    if (!definition) definition = th_resolve(type);
    uint64_t width = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    return definition && th_kind_of(definition) == TH_BITINT
        && th_bitint_info(definition, &width, &flavor)
        && flavor == TH_BITINT_BITS && width
        && width <= N00BB_PACKED_MAX_BITS ? width : 0;
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
shared_option_value_type(ir_module_t *module, th_type *type,
                         LLVMContextRef context, uint8_t *mask)
{
    const th_arg *argument = th_named_arg(type, 0);
    th_type *child = argument && argument->kind == TH_ARG_TYPE
        ? th_resolve(argument->type) : nullptr;
    const char *name = nullptr;
    size_t arguments = 0;
    if (!child || !th_named_info(child, &name, nullptr, nullptr, &arguments)
        || !name || strcmp(name, "option") || arguments != 1)
        return nullptr;
    const char *target = ir_module_target(module);
    const ir_final_layout_t *outer = ir_layout_final(module, type, target);
    const ir_final_layout_t *inner = ir_layout_final_visible(module, child,
                                                               target);
    if (!outer || !inner || !outer->has_discriminant
        || !inner->has_discriminant || outer->discriminant_is_niche
        || inner->discriminant_is_niche
        || outer->discriminant.unit_width_bits != 8
        || inner->discriminant.unit_width_bits != 8
        || outer->discriminant.unit_bit_offset
            != inner->discriminant.unit_bit_offset
        || outer->discriminant.lsb >= 8
        || outer->discriminant.lsb == inner->discriminant.lsb)
        return nullptr;
    LLVMTypeRef representation = value_type(module, child, context);
    if (!representation
        || LLVMGetTypeKind(representation) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(representation) != 2
        || LLVMGetTypeKind(LLVMStructGetTypeAtIndex(representation, 1))
            != LLVMIntegerTypeKind
        || LLVMGetIntTypeWidth(LLVMStructGetTypeAtIndex(representation, 1))
            != 8) return nullptr;
    if (mask) *mask = (uint8_t)(1u << outer->discriminant.lsb);
    return representation;
}

static LLVMTypeRef
packed_option_type(ir_module_t *module, th_type *type,
                   LLVMContextRef context, uint64_t *presence_bit)
{
    const th_arg *argument = th_named_arg(type, 0);
    th_type *child = argument && argument->kind == TH_ARG_TYPE
        ? argument->type : nullptr;
    const char *name = nullptr;
    size_t count = 0;
    if (!child || !th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "option") || count != 1
        || !th_is_closed(ir_module_types(module), type)) return nullptr;
    const char *target = ir_module_target(module);
    const ir_final_layout_t *outer = ir_layout_final(module, type, target);
    const ir_final_layout_t *inner = ir_layout_final_visible(module, child,
                                                               target);
    LLVMTypeRef payload = value_type(module, child, context);
    bool aggregate_payload = payload
        && (LLVMGetTypeKind(payload) == LLVMArrayTypeKind
            || LLVMGetTypeKind(payload) == LLVMStructTypeKind);
    uint64_t outer_bytes = 0, inner_bytes = 0;
    if (!outer || !inner || !payload
        || (!aggregate_payload
            && LLVMGetTypeKind(payload) != LLVMIntegerTypeKind)
        || !outer->has_discriminant || outer->discriminant_is_niche
        || !ir_layout_final_byte_extent(outer, &outer_bytes)
        || !ir_layout_final_byte_extent(inner, &inner_bytes)
        || outer_bytes != inner_bytes || !outer_bytes
        || (!aggregate_payload && outer_bytes > 16)
        || outer->intrinsic_bits > outer_bytes * 8
        || inner->intrinsic_bits > inner_bytes * 8
        || outer->alignment_bytes != inner->alignment_bytes
        || outer->discriminant.unit_width_bits != 8
        || outer->discriminant.value_width_bits != 1
        || outer->discriminant.lsb >= 8
        || outer->discriminant.unit_bit_offset
            > outer_bytes * 8 - 8
        || (!aggregate_payload
            && LLVMGetIntTypeWidth(payload) > outer_bytes * 8))
        return nullptr;
    uint64_t outer_stride = 0, inner_stride = 0;
    if (aggregate_payload
        && (!ir_layout_final_stride_bits(outer, &outer_stride)
            || !ir_layout_final_stride_bits(inner, &inner_stride)
            || outer_stride != inner_stride || outer_stride > UINT_MAX))
        return nullptr;
    uint64_t bit = outer->discriminant.unit_bit_offset
        + outer->discriminant.lsb;
    if (presence_bit) *presence_bit = bit;
    if (aggregate_payload && outer->alignment_bytes == 8
        && outer_stride > 64 && outer_stride % 64 == 0
        && outer_stride / 64 <= UINT32_MAX)
        return LLVMArrayType(LLVMInt64TypeInContext(context),
                             (unsigned)(outer_stride / 64));
    return LLVMIntTypeInContext(context, (unsigned)(aggregate_payload
        ? outer_stride : outer_bytes * 8));
}

static LLVMTypeRef
aggregate_option_type(ir_module_t *module, th_type *type,
                      LLVMContextRef context)
{
    type = th_resolve(type);
    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "option") || count != 1) return nullptr;
    const th_arg *argument = th_named_arg(type, 0);
    if (!argument || argument->kind != TH_ARG_TYPE) return nullptr;
    if (!th_is_closed(ir_module_types(module), type)) {
        LLVMTypeRef element = value_type(module, argument->type, context);
        if (!element || LLVMGetTypeKind(element) == LLVMVoidTypeKind)
            return nullptr;
        LLVMTypeRef fields[] = {element, LLVMInt8TypeInContext(context)};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    if (n00bb_frame_pointer_option(type)) return nullptr;
    LLVMTypeRef packed = packed_option_type(module, type, context, nullptr);
    if (packed) return packed;
    const char *target = ir_module_target(module);
    const ir_final_layout_t *option = ir_layout_final(module, type, target);
    const ir_final_layout_t *payload = ir_layout_final(module,
        argument->type, target);
    LLVMTypeRef shared = shared_option_value_type(module, type, context,
                                                 nullptr);
    if (shared) return shared;
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
value_type_uncached(ir_module_t *module, th_type *type,
                    LLVMContextRef context);

static LLVMTypeRef
value_type(ir_module_t *module, th_type *type, LLVMContextRef context)
{
    th_type *resolved = th_resolve(type);
    if (!active_native_types || !resolved)
        return value_type_uncached(module, type, context);
    native_type_key_t lookup = {module, resolved, context};
    bool found = false;
    LLVMTypeRef native = core_dict_get(active_native_types, &lookup, &found);
    if (found) return native;
    native = value_type_uncached(module, resolved, context);
    if (!native) return nullptr;
    native_type_key_t *key = core_alloc(native_type_key_t);
    *key = lookup;
    if (!core_dict_add(active_native_types, key, native)) core_free(key);
    return native;
}

static LLVMTypeRef
value_type_uncached(ir_module_t *module, th_type *type,
                    LLVMContextRef context)
{
    type = th_resolve(type);
    th_kind kind = th_kind_of(th_resolve(type));
    if (kind == TH_VAR || kind == TH_TRAIT || kind == TH_SUM)
        return box_llvm_type(context);
    if (kind == TH_SEQUENCE
        && !th_is_closed(ir_module_types(module), type))
        return box_llvm_type(context);
    if (kind == TH_INDIRECTION
        && !th_is_closed(ir_module_types(module), type))
        return box_llvm_type(context);
    bool open_variant = false;
    if (kind == TH_NAMED
        && th_named_info(type, nullptr, nullptr, &open_variant, nullptr)
        && open_variant
        && !th_is_closed(ir_module_types(module), type))
        return box_llvm_type(context);
    if (kind == TH_INDIRECTION && uses_bitptr(module, type))
        return bitptr_llvm_type(context);
    th_type *definition = ir_type_definition(module, type);
    if (type && !definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    if ((kind == TH_NAMED || kind == TH_RECORD) && definition
        && th_kind_of(definition) == TH_RECORD
        && !th_is_closed(ir_module_types(module), type))
        return box_llvm_type(context);
    if (definition && th_kind_of(definition) == TH_RECORD) {
        const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
            ir_module_target(module));
        if (layout) {
            for (size_t i = 0; i < layout->n_members; i++)
                if (layout->members[i].bit_offset & 7)
                    return box_llvm_type(context);
        }
    }
    if (definition && th_kind_of(definition) == TH_BITINT) {
        const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
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
        th_type *element = th_seq_elem(th_resolve(type));
        if (element && value_type(module, element, context)
            == box_llvm_type(context)) return box_llvm_type(context);
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
        const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
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
    if (n00bb_frame_pointer_option(type)
        && th_is_closed(ir_module_types(module), type))
        return LLVMPointerTypeInContext(context, 0);
    if (!strcmp(name, "option") && arguments == 1)
        return aggregate_option_type(module, type, context);
    if (!strcmp(name, TH_TUPLE) && arguments <= UINT32_MAX) {
        if (!th_is_closed(ir_module_types(module), type))
            return box_llvm_type(context);
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
        const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
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
            if (field && !(member->bit_offset & 7)
                && layout->intrinsic_bits - member->bit_offset >= 8
                && ir_machine_of(module, field).class == IR_CLASS_BOOL) {
                bool shared_byte = false;
                for (size_t j = 0; j < count && !shared_byte; j++) {
                    if (i == j) continue;
                    const ir_final_member_layout_t *other =
                        &layout->members[j];
                    for (size_t k = 0; k < other->n_occupied; k++) {
                        const ir_layout_bit_run_t *run =
                            &other->occupied[k];
                        if (run->offset < member->bit_offset + 8
                            && run->offset + run->width
                                > member->bit_offset) {
                            shared_byte = true;
                            break;
                        }
                    }
                }
                if (!shared_byte) fields[i] = LLVMInt8TypeInContext(context);
            }
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
native_outcome_type(LLVMModuleRef target, ir_module_t *module, th_type *success,
                    th_type *failure, LLVMContextRef context)
{
    LLVMTypeRef result = success
        ? value_type(module, success, context) : nullptr;
    if (!result || !failure
        || ir_machine_of(module, failure).class == IR_CLASS_VOID)
        return result;
    LLVMTypeRef error = value_type(module, failure, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(target);
    if (!error || !layout) return nullptr;
    bool void_success = LLVMGetTypeKind(result) == LLVMVoidTypeKind;
    bool void_failure = LLVMGetTypeKind(error) == LLVMVoidTypeKind;
    uint64_t success_bytes = void_success ? 0
        : LLVMABISizeOfType(layout, result);
    uint64_t failure_bytes = void_failure ? 0
        : LLVMABISizeOfType(layout, error);
    uint64_t bytes = success_bytes > failure_bytes ? success_bytes
                                                   : failure_bytes;
    uint32_t success_align = void_success ? 1
        : LLVMABIAlignmentOfType(layout, result);
    uint32_t failure_align = void_failure ? 1
        : LLVMABIAlignmentOfType(layout, error);
    uint32_t alignment = success_align > failure_align ? success_align
                                                       : failure_align;
    if (!alignment || alignment > 16 || bytes > UINT32_MAX)
        return nullptr;
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
native_result_type(LLVMModuleRef target, ir_module_t *module,
                   th_type *signature,
                   LLVMContextRef context)
{
    return signature ? native_outcome_type(target, module,
        th_func_ret(signature), th_func_error(signature), context) : nullptr;
}

static LLVMTypeRef function_type(LLVMModuleRef target, ir_func_t *function);
static LLVMTypeRef mutator_function_type(LLVMModuleRef target,
                                        ir_func_t *function);
static LLVMValueRef mutator_function(LLVMModuleRef target,
                                    ir_func_t *function);
static LLVMTypeRef native_function_type(LLVMModuleRef target,
                                        ir_module_t *owner,
                                        th_type *signature,
                                        bool native_binding);
static LLVMTypeRef optional_record_type(LLVMModuleRef target,
                                        ir_func_t *function);
static void emit_trap_if(lower_state_t *state, LLVMValueRef condition);
static uint32_t runtime_type_index(lower_state_t *state, th_type *type);
static LLVMValueRef runtime_type_value(lower_state_t *state, th_type *type);
static LLVMValueRef box_value(lower_state_t *state, LLVMValueRef value,
                              th_type *type);
static LLVMValueRef dynamic_field_box(lower_state_t *state,
    LLVMValueRef box_address, const char *name, uint32_t ordinal,
    bool read_value);

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

static ir_local_t *
direct_local_address(const ir_value_t *address)
{
    while (address && address->kind == IR_VAL_INSTR && address->def) {
        const ir_instr_t *definition = address->def;
        if (definition->op == IR_OP_ADDRESS_OF
            && definition->n_operands == 1
            && definition->operands[0].kind == IR_OPD_VALUE
            && definition->operands[0].value
            && definition->operands[0].value->kind == IR_VAL_LOCAL)
            return definition->operands[0].value->local;
        if ((definition->op != IR_OP_COPY
             && definition->op != IR_OP_CAST
             && definition->op != IR_OP_BITCAST
             && definition->op != IR_OP_NOMINAL_CAST)
            || definition->n_operands != 1
            || definition->operands[0].kind != IR_OPD_VALUE)
            return nullptr;
        address = definition->operands[0].value;
    }
    return nullptr;
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
            if (!record_type || last <= (unsigned)state->with_mutator || !type)
                return nullptr;
            LLVMValueRef record = LLVMGetParam(state->target_function,
                                               last - 1 - (unsigned)state->with_mutator);
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

static LLVMValueRef
lowered_dynamic_type_index(lower_state_t *state, const ir_value_t *value)
{
    for (size_t i = 0; value && i < state->value_count; i++)
        if (state->values[i].source == value)
            return state->values[i].dynamic_type_index;
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
static th_type *option_payload_type(th_type *type);

static LLVMValueRef
frame_root_pointer(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    th_type *option_element = option_payload_type(type);
    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE) {
        LLVMTypeRef native = value_type(state->function->owner, type, context);
        if (LLVMTypeOf(value) != native) return nullptr;
        if (native == box_llvm_type(context)) {
            LLVMValueRef bits = LLVMBuildExtractValue(state->builder, value,
                3, "gc.array.box.payload");
            return LLVMBuildIntToPtr(state->builder, bits,
                LLVMPointerTypeInContext(context, 0),
                "gc.array.box.pointer");
        }
        return LLVMBuildExtractValue(state->builder, value, 0,
            "gc.array.data");
    }
    if (option_element
        && th_kind_of(th_resolve(option_element)) == TH_INDIRECTION
        && !th_is_closed(ir_module_types(state->function->owner), type)
        && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMStructTypeKind
        && LLVMCountStructElementTypes(LLVMTypeOf(value)) == 2) {
        LLVMValueRef element = LLVMBuildExtractValue(state->builder, value,
            0, "gc.option.reference");
        LLVMValueRef present = LLVMBuildExtractValue(state->builder, value,
            1, "gc.option.present");
        LLVMValueRef pointer = element
            ? frame_root_pointer(state, element, option_element) : nullptr;
        if (!pointer || !present
            || LLVMGetTypeKind(LLVMTypeOf(present)) != LLVMIntegerTypeKind)
            return nullptr;
        LLVMValueRef condition = LLVMBuildICmp(state->builder, LLVMIntNE,
            present, LLVMConstNull(LLVMTypeOf(present)),
            "gc.option.has.reference");
        return LLVMBuildSelect(state->builder, condition, pointer,
            LLVMConstNull(LLVMTypeOf(pointer)), "gc.option.root");
    }
    if ((th_kind_of(th_resolve(type)) == TH_INDIRECTION
            || (option_element
                && th_kind_of(th_resolve(option_element)) == TH_INDIRECTION))
        && !th_is_closed(ir_module_types(state->function->owner), type)
        && LLVMTypeOf(value) == box_llvm_type(context)) {
        LLVMValueRef bits = LLVMBuildExtractValue(state->builder, value,
            3, "gc.reference.box.payload");
        return LLVMBuildIntToPtr(state->builder, bits,
            LLVMPointerTypeInContext(context, 0),
            "gc.reference.box.pointer");
    }
    if (uses_bitptr(state->function->owner, type)) {
        if (LLVMTypeOf(value) != bitptr_llvm_type(context)) return nullptr;
        return LLVMBuildExtractValue(state->builder, value, 0,
            "gc.bitptr.base");
    }
    return LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMPointerTypeKind
        ? value : nullptr;
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
                if (state->frame_roots) {
                    const core_indexed_entry_t *root = core_indexed_table_find(
                        state->frame_roots, instruction->dest_local);
                    if (root) {
                        LLVMValueRef pointer = frame_root_pointer(state,
                            value, instruction->dest_local->type);
                        if (!pointer) return false;
                        LLVMValueRef write = LLVMBuildStore(state->builder,
                            pointer, state->frame_slots[root->index]);
                        LLVMSetVolatile(write, true);
                    }
                }
                return true;
            }
        return false;
    }
    if (instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || instruction->dest->kind != IR_VAL_INSTR
        || state->value_count == state->value_capacity) return false;
    state->values[state->value_count++] = (lowered_value_t){
        .source = instruction->dest, .target = value,
    };
    if (state->frame_roots && instruction->op != IR_OP_PHI) {
        const core_indexed_entry_t *root = core_indexed_table_find(
            state->frame_roots, instruction->dest);
        if (root) {
            LLVMValueRef stored = LLVMIsUndef(value) || LLVMIsPoison(value)
                ? LLVMConstNull(LLVMTypeOf(value)) : value;
            LLVMValueRef pointer = frame_root_pointer(state, stored,
                instruction->dest->type);
            if (!pointer) return false;
            LLVMValueRef write = LLVMBuildStore(state->builder, pointer,
                state->frame_slots[root->index]);
            LLVMSetVolatile(write, true);
        }
    }
    return true;
}

static bool
save_result_with_dynamic_type(lower_state_t *state,
                              const ir_instr_t *instruction,
                              LLVMValueRef value, LLVMValueRef type_index)
{
    if (!save_result(state, instruction, value)) return false;
    for (size_t i = 0; i < state->value_count; i++)
        if (state->values[i].source == instruction->dest) {
            state->values[i].dynamic_type_index = type_index;
            return true;
        }
    return false;
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
        LLVMValueRef pointer = frame_root_pointer(state, value,
            instruction->dest->type);
        if (!pointer) return false;
        LLVMValueRef write = LLVMBuildStore(state->builder, pointer,
            state->frame_slots[root->index]);
        LLVMSetVolatile(write, true);
    }
    return true;
}

static bool
save_call_output(lower_state_t *state, const ir_instr_t *call,
                 uint32_t ordinal, LLVMValueRef storage, th_type *type)
{
    return core_list_try_push(state->call_outputs,
                              ((lowered_call_output_t){call, ordinal,
                                                       storage, type}));
}

static LLVMValueRef
trap_function(lower_state_t *state)
{
    unsigned id = LLVMLookupIntrinsicID("llvm.trap", sizeof("llvm.trap") - 1);
    return LLVMGetIntrinsicDeclaration(state->module, id, nullptr, 0);
}

static void
emit_trap_reason(lower_state_t *state, const char *reason)
{
    if (!reason || ir_func_execution_phase(state->function)
            == IR_PHASE_COMPTIME) {
        LLVMValueRef function = trap_function(state);
        LLVMBuildCall2(state->builder, LLVMGlobalGetValueType(function),
                       function, nullptr, 0, "");
        LLVMBuildUnreachable(state->builder);
        return;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef parameters[] = {pointer, i64};
    LLVMTypeRef trap_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameters, 2, false);
    LLVMValueRef trap = LLVMGetNamedFunction(state->module,
                                             "n00bb_runtime_trap");
    if (!trap) {
        trap = LLVMAddFunction(state->module, "n00bb_runtime_trap",
                               trap_type);
        unsigned kinds[] = {
            LLVMGetEnumAttributeKindForName("noreturn", 8),
            LLVMGetEnumAttributeKindForName("cold", 4),
        };
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
            LLVMAddAttributeAtIndex(trap, LLVMAttributeFunctionIndex,
                LLVMCreateEnumAttribute(context, kinds[i], 0));
    }
    size_t length = strlen(reason);
    LLVMValueRef text = LLVMBuildGlobalString(state->builder, reason,
                                              "trap.reason");
    LLVMValueRef arguments[] = {text, LLVMConstInt(i64, length, false)};
    LLVMBuildCall2(state->builder, trap_type, trap, arguments, 2, "");
    LLVMBuildUnreachable(state->builder);
}

static void
emit_trap(lower_state_t *state)
{
    emit_trap_reason(state, "runtime check failed");
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
array_helper_index(lower_state_t *state, LLVMValueRef index, th_type *type)
{
    if (!index || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind)
        return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef source = LLVMTypeOf(index);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    bool signed_index = ir_machine_of(state->function->owner, type).class
        == IR_CLASS_SINT;
    if (!signed_index && LLVMGetIntTypeWidth(source) >= 64) {
        LLVMValueRef too_large = LLVMBuildICmp(state->builder, LLVMIntUGT,
            index, LLVMConstInt(source, INT64_MAX, false),
            "array.index.too.large");
        emit_trap_if(state, too_large);
    }
    return LLVMBuildIntCast2(state->builder, index, i64, signed_index,
                             "generic.array.index");
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

static LLVMValueRef entry_alloca(lower_state_t *state, LLVMTypeRef type);

static LLVMValueRef
make_packed_bitptr(lower_state_t *state, LLVMValueRef data,
                   LLVMValueRef bit_position, th_type *element)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i16 = LLVMInt16TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    uint32_t type_index = runtime_type_index(state, element);
    uint64_t width = packed_array_width(state->function->owner, element);
    if (!type_index || !width) {
        backend_error(state->diagnostics,
            "packed element has no runtime type index or semantic width");
        return nullptr;
    }
    if (!data || !bit_position
        || LLVMGetTypeKind(LLVMTypeOf(bit_position)) != LLVMIntegerTypeKind
        || LLVMGetIntTypeWidth(LLVMTypeOf(bit_position)) < 64)
        return nullptr;
    LLVMTypeRef position_type = LLVMTypeOf(bit_position);
    LLVMValueRef byte_offset = LLVMBuildIntCast2(state->builder,
        LLVMBuildLShr(state->builder, bit_position,
            LLVMConstInt(position_type, 3, false), "bitptr.byte.offset.wide"),
        i64, false, "bitptr.byte.offset");
    LLVMValueRef base = LLVMBuildGEP2(state->builder,
        LLVMInt8TypeInContext(context), data, &byte_offset, 1,
        "bitptr.base");
    LLVMValueRef low = LLVMBuildTrunc(state->builder,
        LLVMBuildAnd(state->builder, bit_position,
            LLVMConstInt(position_type, N00B_ABI_ARRAY_ORIGIN_BIT_MASK,
                         false), "bitptr.low"), i16,
        "bitptr.start");
    LLVMValueRef end = LLVMBuildAdd(state->builder, low,
        LLVMConstInt(i16, width, false), "bitptr.end");
    LLVMValueRef value = LLVMGetUndef(bitptr_llvm_type(context));
    value = LLVMBuildInsertValue(state->builder, value, base, 0,
        "bitptr.with.base");
    value = LLVMBuildInsertValue(state->builder, value,
        LLVMConstInt(i32, type_index, false), 1, "bitptr.with.type");
    value = LLVMBuildInsertValue(state->builder, value, low, 2,
        "bitptr.with.start");
    return LLVMBuildInsertValue(state->builder, value, end, 3,
        "bitptr.with.end");
}

static LLVMValueRef
make_bool_bitptr(lower_state_t *state, LLVMValueRef data,
                 LLVMValueRef bit_position, th_type *element)
{
    return make_packed_bitptr(state, data, bit_position, element);
}

static LLVMValueRef
load_bool_bitptr(lower_state_t *state, LLVMValueRef bitptr)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMValueRef base = LLVMBuildExtractValue(state->builder, bitptr, 0,
        "bitptr.load.base");
    LLVMValueRef start = LLVMBuildExtractValue(state->builder, bitptr, 2,
        "bitptr.load.start");
    LLVMValueRef byte = LLVMBuildLoad2(state->builder, i8, base,
        "bitptr.load.byte");
    LLVMValueRef shift = LLVMBuildTrunc(state->builder, start, i8,
        "bitptr.load.shift");
    LLVMValueRef shifted = LLVMBuildLShr(state->builder, byte, shift,
        "bitptr.load.shifted");
    return LLVMBuildTrunc(state->builder, shifted,
        LLVMInt1TypeInContext(context), "bitptr.load.value");
}

static bool
store_bool_bitptr(lower_state_t *state, LLVMValueRef bitptr,
                  LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    if (!value || LLVMTypeOf(value) != LLVMInt1TypeInContext(context))
        return false;
    LLVMValueRef base = LLVMBuildExtractValue(state->builder, bitptr, 0,
        "bitptr.store.base");
    LLVMValueRef start = LLVMBuildExtractValue(state->builder, bitptr, 2,
        "bitptr.store.start");
    LLVMValueRef shift = LLVMBuildTrunc(state->builder, start, i8,
        "bitptr.store.shift");
    LLVMValueRef mask = LLVMBuildShl(state->builder,
        LLVMConstInt(i8, 1, false), shift, "bitptr.store.mask");
    LLVMValueRef previous = LLVMBuildLoad2(state->builder, i8, base,
        "bitptr.store.previous");
    LLVMValueRef cleared = LLVMBuildAnd(state->builder, previous,
        LLVMBuildNot(state->builder, mask, "bitptr.store.inverse"),
        "bitptr.store.cleared");
    LLVMValueRef set = LLVMBuildSelect(state->builder, value, mask,
        LLVMConstNull(i8), "bitptr.store.set");
    LLVMValueRef updated = LLVMBuildOr(state->builder, cleared, set,
        "bitptr.store.updated");
    return LLVMBuildStore(state->builder, updated, base) != nullptr;
}

typedef struct {
    LLVMValueRef base;
    LLVMValueRef scratch;
    LLVMValueRef start;
    LLVMValueRef bytes;
    LLVMTypeRef wide;
} packed_bitptr_io_t;

static bool
packed_bitptr_prepare(lower_state_t *state, LLVMValueRef bitptr,
                      uint64_t width, packed_bitptr_io_t *out)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i16 = LLVMInt16TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef wide = LLVMIntTypeInContext(context,
        N00BB_PACKED_SCRATCH_BYTES * CHAR_BIT);
    LLVMValueRef base = LLVMBuildExtractValue(state->builder, bitptr, 0,
        "bitptr.io.base");
    LLVMValueRef start = LLVMBuildExtractValue(state->builder, bitptr, 2,
        "bitptr.io.start");
    LLVMValueRef end = LLVMBuildExtractValue(state->builder, bitptr, 3,
        "bitptr.io.end");
    if (!base || !start || !end || LLVMTypeOf(start) != i16
        || LLVMTypeOf(end) != i16) return false;
    emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
        LLVMBuildSub(state->builder, end, start, "bitptr.io.width"),
        LLVMConstInt(i16, width, false), "bitptr.io.invalid.width"));
    LLVMValueRef bytes = LLVMBuildLShr(state->builder,
        LLVMBuildAdd(state->builder, end,
            LLVMConstInt(i16, N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
            "bitptr.io.rounded"), LLVMConstInt(i16, 3, false),
        "bitptr.io.bytes");
    LLVMValueRef scratch = entry_alloca(state, wide);
    if (!scratch) return false;
    LLVMBuildMemSet(state->builder, scratch,
        LLVMConstNull(LLVMInt8TypeInContext(context)),
        LLVMConstInt(i64, N00BB_PACKED_SCRATCH_BYTES, false), 1);
    LLVMBuildMemCpy(state->builder, scratch, 1, base, 1,
        LLVMBuildZExt(state->builder, bytes, i64, "bitptr.io.bytes64"));
    *out = (packed_bitptr_io_t){
        .base = base, .scratch = scratch, .start = start,
        .bytes = LLVMBuildZExt(state->builder, bytes, i64,
            "bitptr.io.copy.bytes"), .wide = wide,
    };
    return true;
}

static LLVMValueRef
load_packed_bitptr(lower_state_t *state, LLVMValueRef bitptr,
                   th_type *element)
{
    uint64_t width = packed_array_width(state->function->owner, element);
    if (width == 1) return load_bool_bitptr(state, bitptr);
    if (!width) return nullptr;
    packed_bitptr_io_t io = {0};
    if (!packed_bitptr_prepare(state, bitptr, width, &io)) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef narrow = LLVMIntTypeInContext(context, (unsigned)width);
    LLVMTypeRef native = value_type(state->function->owner, element, context);
    if (!native || LLVMGetTypeKind(native) != LLVMIntegerTypeKind
        || LLVMGetIntTypeWidth(native) < width) return nullptr;
    LLVMValueRef bits = LLVMBuildLShr(state->builder,
        LLVMBuildLoad2(state->builder, io.wide, io.scratch,
            "bitptr.load.bits"),
        LLVMBuildZExt(state->builder, io.start, io.wide,
            "bitptr.load.shift"), "bitptr.load.shifted");
    LLVMValueRef value = LLVMBuildTrunc(state->builder, bits, narrow,
        "bitptr.load.narrow");
    return native == narrow ? value
        : LLVMBuildZExt(state->builder, value, native,
            "bitptr.load.value");
}

static bool
store_packed_bitptr(lower_state_t *state, LLVMValueRef bitptr,
                    LLVMValueRef value, th_type *element)
{
    uint64_t width = packed_array_width(state->function->owner, element);
    if (width == 1) return store_bool_bitptr(state, bitptr, value);
    if (!width || !value
        || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
        || LLVMGetIntTypeWidth(LLVMTypeOf(value)) < width) return false;
    packed_bitptr_io_t io = {0};
    if (!packed_bitptr_prepare(state, bitptr, width, &io)) return false;
    LLVMValueRef one = LLVMConstInt(io.wide, 1, false);
    LLVMValueRef low_mask = LLVMBuildSub(state->builder,
        LLVMBuildShl(state->builder, one,
            LLVMConstInt(io.wide, width, false), "bitptr.store.mask.top"),
        one, "bitptr.store.mask.low");
    LLVMValueRef shift = LLVMBuildZExt(state->builder, io.start, io.wide,
        "bitptr.store.shift");
    LLVMValueRef mask = LLVMBuildShl(state->builder, low_mask, shift,
        "bitptr.store.mask");
    LLVMValueRef previous = LLVMBuildLoad2(state->builder, io.wide,
        io.scratch, "bitptr.store.previous");
    LLVMValueRef narrowed = LLVMBuildAnd(state->builder,
        LLVMBuildIntCast2(state->builder, value, io.wide, false,
            "bitptr.store.value.wide"),
        low_mask, "bitptr.store.narrowed");
    LLVMValueRef updated = LLVMBuildOr(state->builder,
        LLVMBuildAnd(state->builder, previous,
            LLVMBuildNot(state->builder, mask, "bitptr.store.inverse"),
            "bitptr.store.cleared"),
        LLVMBuildShl(state->builder, narrowed, shift,
            "bitptr.store.shifted"), "bitptr.store.updated");
    LLVMBuildStore(state->builder, updated, io.scratch);
    return LLVMBuildMemCpy(state->builder, io.base, 1, io.scratch, 1,
        io.bytes) != nullptr;
}

static bool
copy_bool_bits(lower_state_t *state, LLVMValueRef target,
               LLVMValueRef target_origin, LLVMValueRef source,
               LLVMValueRef source_origin, LLVMValueRef count,
               th_type *element)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
    LLVMValueRef target_start = LLVMBuildAdd(state->builder,
        LLVMBuildShl(state->builder,
            LLVMBuildPtrToInt(state->builder, target, i128,
                "bitcopy.target.address"), LLVMConstInt(i128, 3, false),
            "bitcopy.target.byte.bits"),
        LLVMBuildZExt(state->builder, target_origin, i128,
            "bitcopy.target.origin"), "bitcopy.target.start");
    LLVMValueRef source_start = LLVMBuildAdd(state->builder,
        LLVMBuildShl(state->builder,
            LLVMBuildPtrToInt(state->builder, source, i128,
                "bitcopy.source.address"), LLVMConstInt(i128, 3, false),
            "bitcopy.source.byte.bits"),
        LLVMBuildZExt(state->builder, source_origin, i128,
            "bitcopy.source.origin"), "bitcopy.source.start");
    LLVMValueRef reverse = LLVMBuildICmp(state->builder, LLVMIntUGT,
        target_start, source_start, "bitcopy.reverse");
    LLVMValueRef function = LLVMGetBasicBlockParent(
        LLVMGetInsertBlock(state->builder));
    LLVMBasicBlockRef forward = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.forward");
    LLVMBasicBlockRef forward_loop = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.forward.loop");
    LLVMBasicBlockRef forward_body = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.forward.body");
    LLVMBasicBlockRef backward = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.backward");
    LLVMBasicBlockRef backward_loop = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.backward.loop");
    LLVMBasicBlockRef backward_body = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.backward.body");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        function, "bitcopy.done");
    LLVMBuildCondBr(state->builder, reverse, backward, forward);

    LLVMPositionBuilderAtEnd(state->builder, forward);
    LLVMBuildBr(state->builder, forward_loop);
    LLVMPositionBuilderAtEnd(state->builder, forward_loop);
    LLVMValueRef f_index = LLVMBuildPhi(state->builder, i64,
        "bitcopy.forward.index");
    LLVMValueRef more = LLVMBuildICmp(state->builder, LLVMIntULT,
        f_index, count, "bitcopy.forward.more");
    LLVMBuildCondBr(state->builder, more, forward_body, done);
    LLVMPositionBuilderAtEnd(state->builder, forward_body);
    LLVMValueRef f_source_bit = LLVMBuildAdd(state->builder,
        LLVMBuildZExt(state->builder, f_index, i128,
            "bitcopy.forward.source.index"),
        LLVMBuildZExt(state->builder, source_origin, i128,
            "bitcopy.forward.source.origin"), "bitcopy.forward.source.bit");
    LLVMValueRef f_target_bit = LLVMBuildAdd(state->builder,
        LLVMBuildZExt(state->builder, f_index, i128,
            "bitcopy.forward.target.index"),
        LLVMBuildZExt(state->builder, target_origin, i128,
            "bitcopy.forward.target.origin"), "bitcopy.forward.target.bit");
    LLVMValueRef f_source = make_bool_bitptr(state, source,
        f_source_bit, element);
    LLVMValueRef f_target = make_bool_bitptr(state, target,
        f_target_bit, element);
    if (!f_source || !f_target
        || !store_bool_bitptr(state, f_target,
            load_bool_bitptr(state, f_source))) return false;
    LLVMValueRef f_next = LLVMBuildAdd(state->builder, f_index,
        LLVMConstInt(i64, 1, false), "bitcopy.forward.next");
    LLVMBuildBr(state->builder, forward_loop);
    LLVMValueRef f_inputs[] = {LLVMConstNull(i64), f_next};
    LLVMBasicBlockRef f_blocks[] = {forward, forward_body};
    LLVMAddIncoming(f_index, f_inputs, f_blocks, 2);

    LLVMPositionBuilderAtEnd(state->builder, backward);
    LLVMBuildBr(state->builder, backward_loop);
    LLVMPositionBuilderAtEnd(state->builder, backward_loop);
    LLVMValueRef b_index = LLVMBuildPhi(state->builder, i64,
        "bitcopy.backward.index");
    LLVMValueRef remains = LLVMBuildICmp(state->builder, LLVMIntNE,
        b_index, LLVMConstNull(i64), "bitcopy.backward.more");
    LLVMBuildCondBr(state->builder, remains, backward_body, done);
    LLVMPositionBuilderAtEnd(state->builder, backward_body);
    LLVMValueRef b_next = LLVMBuildSub(state->builder, b_index,
        LLVMConstInt(i64, 1, false), "bitcopy.backward.next");
    LLVMValueRef b_source_bit = LLVMBuildAdd(state->builder,
        LLVMBuildZExt(state->builder, b_next, i128,
            "bitcopy.backward.source.index"),
        LLVMBuildZExt(state->builder, source_origin, i128,
            "bitcopy.backward.source.origin"), "bitcopy.backward.source.bit");
    LLVMValueRef b_target_bit = LLVMBuildAdd(state->builder,
        LLVMBuildZExt(state->builder, b_next, i128,
            "bitcopy.backward.target.index"),
        LLVMBuildZExt(state->builder, target_origin, i128,
            "bitcopy.backward.target.origin"), "bitcopy.backward.target.bit");
    LLVMValueRef b_source = make_bool_bitptr(state, source,
        b_source_bit, element);
    LLVMValueRef b_target = make_bool_bitptr(state, target,
        b_target_bit, element);
    if (!b_source || !b_target
        || !store_bool_bitptr(state, b_target,
            load_bool_bitptr(state, b_source))) return false;
    LLVMBuildBr(state->builder, backward_loop);
    LLVMValueRef b_inputs[] = {count, b_next};
    LLVMBasicBlockRef b_blocks[] = {backward, backward_body};
    LLVMAddIncoming(b_index, b_inputs, b_blocks, 2);
    LLVMPositionBuilderAtEnd(state->builder, done);
    return true;
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
    type = th_resolve(type);
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
        && (LLVMGetIntTypeWidth(native) == 1
            || LLVMGetIntTypeWidth(native) == 8)
        && LLVMABISizeOfType(target, native) == sizeof(uint8_t)
        && LLVMABIAlignmentOfType(target, native) == 1;
}

static bool
literal_element_layout_matches(ir_module_t *module, th_type *type,
                               LLVMTypeRef native, LLVMTargetDataRef target)
{
    type = th_resolve(type);
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    uint64_t stride_bits = 0;
    if (!layout || !native || !target
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8
        || LLVMABISizeOfType(target, native) != stride_bits / 8
        || LLVMABIAlignmentOfType(target, native)
            != layout->alignment_bytes) return false;
    bool variant = false;
    (void)th_named_info(type, nullptr, nullptr, &variant, nullptr);
    if (variant) {
        uint64_t unit_bits = (uint64_t)layout->alignment_bytes * 8;
        return layout->has_discriminant && unit_bits
            && unit_bits <= UINT_MAX && !(stride_bits % unit_bits)
            && stride_bits / unit_bits <= UINT32_MAX
            && native == LLVMArrayType(LLVMIntTypeInContext(
                LLVMGetTypeContext(native), (unsigned)unit_bits),
                (unsigned)(stride_bits / unit_bits));
    }
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
    if (machine.class == IR_CLASS_FLOAT && machine.width == 32)
        return LLVMGetTypeKind(native) == LLVMFloatTypeKind
            && layout->intrinsic_bits == 32;
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
    if (th_kind_of(th_resolve(type)) == TH_SUM)
        return layout->intrinsic_bits == 128
            && native == box_llvm_type(LLVMGetTypeContext(native));

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
    if (n00bb_frame_pointer_option(type))
        return LLVMGetTypeKind(native) == LLVMPointerTypeKind
            && layout->intrinsic_bits == sizeof(void *) * 8
            && layout->has_discriminant
            && layout->discriminant_is_niche
            && layout->discriminant.unit_bit_offset == 0
            && layout->discriminant.unit_width_bits == sizeof(void *) * 8;
    if (!strcmp(name, "option") && count == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        uint64_t presence_bit = 0;
        LLVMTypeRef packed = packed_option_type(module, type,
            LLVMGetTypeContext(native), &presence_bit);
        if (packed && native == packed)
            return argument && argument->kind == TH_ARG_TYPE
                && presence_bit < stride_bits
                && literal_element_layout_matches(module, argument->type,
                    value_type(module, argument->type,
                        LLVMGetTypeContext(native)), target);
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

static bool sequence_value_transfers_storage(ir_value_t *source);
static LLVMValueRef copy_sequence_value(lower_state_t *state,
                                        LLVMValueRef value, th_type *type);

static LLVMValueRef
aggregate_operand_value(lower_state_t *state, ir_operand_t *operand)
{
    ir_value_t *source = operand && operand->kind == IR_OPD_VALUE
        ? operand->value : nullptr;
    LLVMValueRef value = source ? lowered_value(state, source) : nullptr;
    return value && th_kind_of(th_resolve(source->type)) == TH_SEQUENCE
        && !sequence_value_transfers_storage(source)
        ? copy_sequence_value(state, value, source->type) : value;
}

static LLVMValueRef
new_boxed_aggregate_slot(lower_state_t *state, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
        allocator_params, 5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, allocator_type);
    LLVMTypeRef parameters[] = {i32, pointer, pointer};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameters, 3, false);
    LLVMValueRef create = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_aggregate_literal_box");
    if (!create) create = LLVMAddFunction(state->module,
        "n00bb_bootstrap_aggregate_literal_box", signature);
    LLVMValueRef type_index = runtime_type_value(state, type);
    LLVMValueRef slot = entry_alloca(state, box_llvm_type(context));
    if (!allocator || !create || !type_index || !slot
        || LLVMGlobalGetValueType(allocator) != allocator_type
        || LLVMGlobalGetValueType(create) != signature) return nullptr;
    LLVMValueRef args[] = {type_index, allocator, slot};
    LLVMBuildCall2(state->builder, signature, create, args, 3, "");
    return slot;
}

static bool
write_boxed_record_field(lower_state_t *state, LLVMValueRef record_slot,
                         const char *name, LLVMValueRef value,
                         th_type *value_type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer, pointer};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameters, 2, false);
    LLVMValueRef write = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_ref_write_box");
    if (!write) write = LLVMAddFunction(state->module,
        "n00bb_bootstrap_ref_write_box", signature);
    LLVMValueRef field = name
        ? dynamic_field_box(state, record_slot, name, UINT32_MAX, false)
        : nullptr;
    LLVMValueRef boxed = value
        ? box_value(state, value, value_type) : nullptr;
    LLVMValueRef field_slot = entry_alloca(state, box);
    LLVMValueRef value_slot = entry_alloca(state, box);
    if (!write || !field || !boxed || !field_slot || !value_slot
        || LLVMGlobalGetValueType(write) != signature) return false;
    LLVMBuildStore(state->builder, field, field_slot);
    LLVMBuildStore(state->builder, boxed, value_slot);
    LLVMValueRef args[] = {field_slot, value_slot};
    return LLVMBuildCall2(state->builder, signature, write, args, 2, "")
        != nullptr;
}

static LLVMValueRef
tuple_aggregate(lower_state_t *state, ir_instr_t *instruction)
{
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    const char *name = nullptr;
    size_t count = 0;
    if (!type || !th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, TH_TUPLE)) return nullptr;
    if (count != instruction->n_operands) {
        state->failure_detail = "tuple definition and operands disagree";
        return nullptr;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(state->function->owner, type, context);
    if (native == box_llvm_type(context)) {
        LLVMValueRef slot = new_boxed_aggregate_slot(state, type);
        if (!slot) {
            state->failure_detail = "generic tuple has no runtime type index";
            return nullptr;
        }
        for (size_t i = 0; i < count; i++) {
            ir_operand_t *operand = &instruction->operands[i];
            LLVMValueRef field = aggregate_operand_value(state, operand);
            char field_name[3 * sizeof(size_t) + 2];
            int length = snprintf(field_name, sizeof(field_name), "%zu", i);
            if (length < 0 || (size_t)length >= sizeof(field_name)
                || operand->kind != IR_OPD_VALUE || !operand->value
                || !write_boxed_record_field(state, slot, field_name,
                    field, operand->value->type)) {
                state->failure_detail = "generic tuple field cannot be written";
                return nullptr;
            }
        }
        return LLVMBuildLoad2(state->builder, native, slot,
            "tuple.literal.box");
    }
    if (!cached_literal_element_layout_matches(state, type, native)) {
        state->failure_detail = "tuple fields differ from finalized layout";
        return nullptr;
    }
    LLVMValueRef *fields = count ? calloc(count, sizeof(*fields)) : nullptr;
    if (count && !fields) return nullptr;
    bool constant = true;
    bool valid = true;
    for (size_t i = 0; valid && i < count; i++) {
        ir_operand_t *operand = &instruction->operands[i];
        fields[i] = aggregate_operand_value(state, operand);
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
    if (!valid) state->failure_detail = "tuple operand LLVM types disagree";
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
        || (layout && layout->n_members != count)
        || count != instruction->n_operands || count > UINT32_MAX) {
        state->failure_detail = !definition
            ? "record type has no definition"
            : th_kind_of(definition) != TH_RECORD
                ? "record definition is not a record"
            : variant ? "record literal has a variant type"
            : layout && layout->n_members != count
                ? "record layout and definition have different fields"
            : count != instruction->n_operands
                ? "record definition and literal have different fields"
                : "record has too many fields";
        return nullptr;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(module, type, context);
    if (native == box_llvm_type(context)) {
        LLVMValueRef result_slot = new_boxed_aggregate_slot(state, type);
        if (!result_slot) {
            state->failure_detail = "generic record has no runtime type index";
            return nullptr;
        }
        for (size_t i = 0; i < count; i++) {
            const ir_final_member_layout_t *member = layout
                ? &layout->members[i] : nullptr;
            uint32_t ordinal = member ? member->ordinal : (uint32_t)i;
            const th_member *semantic = th_record_member(definition,
                ordinal);
            ir_operand_t *operand = ordinal < count
                ? &instruction->operands[ordinal] : nullptr;
            LLVMValueRef field_value = aggregate_operand_value(state,
                operand);
            if (!operand || operand->kind != IR_OPD_VALUE || !operand->value
                || !write_boxed_record_field(state, result_slot,
                    member ? member->name : semantic->name, field_value,
                    operand->value->type)) {
                state->failure_detail = "generic record field cannot be written";
                return nullptr;
            }
        }
        return LLVMBuildLoad2(state->builder, native, result_slot,
            "record.literal.box");
    }
    if (!layout || !native || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != count
        || !cached_literal_element_layout_matches(state, type, native)) {
        state->failure_detail = !layout
            ? "record has no finalized layout"
            : !native ? "record has no LLVM value type"
            : LLVMGetTypeKind(native) != LLVMStructTypeKind
                ? "record LLVM value type is not a struct"
            : LLVMCountStructElementTypes(native) != count
                ? "record LLVM field count differs from its definition"
            : "record LLVM fields differ from finalized layout";
        return nullptr;
    }
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
        fields[i] = aggregate_operand_value(state, operand);
        LLVMTypeRef storage = LLVMStructGetTypeAtIndex(native,
            (unsigned)i);
        if (fields[i] && operand && operand->kind == IR_OPD_VALUE
            && operand->value
            && ir_machine_of(module, operand->value->type).class
                == IR_CLASS_BOOL
            && LLVMTypeOf(fields[i]) == LLVMInt1TypeInContext(context)
            && storage == LLVMInt8TypeInContext(context))
            fields[i] = LLVMGetValueKind(fields[i])
                == LLVMConstantIntValueKind
                ? LLVMConstInt(storage,
                    LLVMConstIntGetZExtValue(fields[i]), false)
                : LLVMBuildZExt(state->builder, fields[i], storage,
                    "record.bool.storage");
        valid = fields[i] && LLVMTypeOf(fields[i])
            == storage;
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
    if (!result)
        state->failure_detail = "record field values do not match storage types";
    return result;
}

static bool
record_member_index(lower_state_t *state, th_type *record_type,
                    const char *name, LLVMTypeRef *native_out,
                    unsigned *index_out)
{
    record_type = th_resolve(record_type);
    if (!record_type) return false;
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
    if (!definition && th_kind_of(th_resolve(record_type)) == TH_RECORD)
        definition = th_resolve(record_type);
    const ir_final_layout_t *layout = ir_layout_final(module, record_type,
        ir_module_target(module));
    LLVMTypeRef native = value_type(module, record_type,
        LLVMGetModuleContext(state->module));
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    bool valid = definition && th_kind_of(definition) == TH_RECORD
        && layout && native && target;
    if (valid && !cached_literal_element_layout_matches(state, record_type,
                                                          native)) {
        state->failure_detail = "record ABI layout differs from LLVM type";
        valid = false;
    }
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

static uint32_t
record_member_ordinal(ir_module_t *module, th_type *record_type,
                      const char *name)
{
    if (!module || !record_type || !name) return UINT32_MAX;
    record_type = th_resolve(record_type);
    th_type *definition = ir_type_definition(module, record_type);
    if (!definition && th_kind_of(record_type) == TH_RECORD)
        definition = record_type;
    if (!definition || th_kind_of(definition) != TH_RECORD)
        return UINT32_MAX;
    size_t count = th_record_len(definition);
    if (count >= UINT32_MAX) return UINT32_MAX;
    for (size_t i = 0; i < count; i++) {
        const th_member *member = th_record_member(definition, i);
        if (member && member->name && !strcmp(member->name, name))
            return (uint32_t)i;
    }
    return UINT32_MAX;
}

static const ir_final_member_layout_t *
known_byte_record_member(ir_module_t *module, th_type *record_type,
                         const char *name)
{
    const ir_final_layout_t *layout = ir_layout_final(module, record_type,
        ir_module_target(module));
    if (!layout || !name) return nullptr;
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        if (!member->name || strcmp(member->name, name)
            || !member->type || member->n_occupied != 1
            || (member->bit_offset & 7)) continue;
        const ir_final_layout_t *field = ir_layout_final(module,
            member->type, ir_module_target(module));
        if (field && member->occupied[0].offset == member->bit_offset
            && member->occupied[0].width == field->intrinsic_bits)
            return member;
    }
    return nullptr;
}

static LLVMValueRef
nominal_record_value(lower_state_t *state, LLVMValueRef source,
                     th_type *source_type, th_type *target_type,
                     th_type *source_shape, th_type *target_shape,
                     LLVMTypeRef target)
{
    size_t count = th_record_len(target_shape);
    if (target == box_llvm_type(LLVMGetModuleContext(state->module))) {
        LLVMTypeRef box = box_llvm_type(LLVMGetModuleContext(state->module));
        bool boxed_source = LLVMTypeOf(source) == box;
        if (count != th_record_len(source_shape) || count > UINT32_MAX
            || (!boxed_source
                && (LLVMGetTypeKind(LLVMTypeOf(source))
                        != LLVMStructTypeKind
                    || LLVMCountStructElementTypes(LLVMTypeOf(source))
                        != count)))
            return nullptr;
        LLVMValueRef slot = new_boxed_aggregate_slot(state, target_type);
        LLVMValueRef source_slot = boxed_source ? entry_alloca(state, box)
            : nullptr;
        if (!slot) return nullptr;
        if (boxed_source && !source_slot) return nullptr;
        if (boxed_source) LLVMBuildStore(state->builder, source, source_slot);
        for (size_t i = 0; i < count; i++) {
            const th_member *member = th_record_member(target_shape, i);
            th_type *source_field = member && member->name
                ? th_record_lookup(source_shape, member->name) : nullptr;
            LLVMTypeRef source_native = nullptr;
            unsigned source_index = 0;
            if (!source_field || !ir_type_eq(state->function->owner,
                    source_field, member->type)) return nullptr;
            LLVMValueRef field = nullptr;
            if (boxed_source)
                field = dynamic_field_box(state, source_slot, member->name,
                    record_member_ordinal(state->function->owner, source_type,
                                          member->name), true);
            else {
                if (!record_member_index(state, source_type, member->name,
                        &source_native, &source_index)
                    || source_native != LLVMTypeOf(source)) return nullptr;
                field = LLVMBuildExtractValue(state->builder, source,
                    source_index, "nominal.field");
            }
            if (!write_boxed_record_field(state, slot, member->name,
                    field, source_field)) return nullptr;
        }
        return LLVMBuildLoad2(state->builder, target, slot,
            "nominal.record.box");
    }
    if (count != th_record_len(source_shape) || count > UINT32_MAX
        || LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMStructTypeKind
        || LLVMGetTypeKind(target) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(LLVMTypeOf(source)) != count
        || LLVMCountStructElementTypes(target) != count) return nullptr;
    LLVMValueRef result = LLVMGetUndef(target);
    for (size_t i = 0; i < count; i++) {
        const th_member *member = th_record_member(target_shape, i);
        th_type *source_field = member && member->name
            ? th_record_lookup(source_shape, member->name) : nullptr;
        LLVMTypeRef source_native = nullptr, target_native = nullptr;
        unsigned source_index = 0, target_index = 0;
        if (!source_field || !ir_type_eq(state->function->owner,
                source_field, member->type)
            || !record_member_index(state, source_type, member->name,
                &source_native, &source_index)
            || !record_member_index(state, target_type, member->name,
                &target_native, &target_index)
            || source_native != LLVMTypeOf(source)
            || target_native != target) return nullptr;
        LLVMValueRef field = LLVMBuildExtractValue(state->builder, source,
            source_index, "nominal.field");
        if (!field || LLVMTypeOf(field)
                != LLVMStructGetTypeAtIndex(target, target_index))
            return nullptr;
        result = LLVMBuildInsertValue(state->builder, result, field,
            target_index, "nominal.value");
        if (!result) return nullptr;
    }
    return result;
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
cached_array_value(lower_state_t *state, ir_module_t *source, th_type *element,
                   const n00bb_llvm_phase_array_t *array)
{
    const ir_final_layout_t *layout = element
        ? ir_layout_final(source, element, ir_module_target(source)) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = element
        ? value_type(source, element, context) : nullptr;
    uint64_t stride_bits = 0;
    if (!array || !element || !layout || !layout->alignment_bytes
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || !literal_element_layout_matches(source, element,
            element_type, LLVMGetModuleDataLayout(state->module)))
        return nullptr;
    uint64_t alignment = layout->alignment_bytes;
    uint64_t stride = stride_bits / 8;
    if (!stride || stride > UINT32_MAX) return nullptr;
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

static LLVMValueRef
materialized_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    if (!identifier || !element) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *array = &state->phase_arrays[i];
        if (array->kind == N00BB_LLVM_LITERAL_ARRAY
            && array->identifier && !strcmp(identifier, array->identifier))
            return cached_array_value(state, state->function->owner,
                                      element, array);
    }
    return nullptr;
}

static LLVMValueRef
generic_array_new(lower_state_t *state, th_type *type,
                  LLVMValueRef count, uint32_t rank, uint32_t flat_rank,
                  LLVMValueRef extents)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef type_index = runtime_type_value(state, type);
    if (!type_index || !count || LLVMTypeOf(count) != i64 || !extents
        || LLVMGetTypeKind(LLVMTypeOf(extents)) != LLVMPointerTypeKind)
        return nullptr;
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
        allocator_params, 5, false);
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, allocator_type);
    LLVMTypeRef parameters[] = {i32, i64, i32, i32, pointer,
        pointer, pointer};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameters, 7, false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_new_array_box");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_new_array_box", signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != allocator_type
        || !helper || LLVMGlobalGetValueType(helper) != signature)
        return nullptr;
    LLVMValueRef result_slot = LLVMBuildAlloca(state->builder, box,
        "array.new.box.slot");
    LLVMValueRef args[] = {type_index, count,
        LLVMConstInt(i32, rank, false),
        LLVMConstInt(i32, flat_rank, false), extents, allocator, result_slot};
    LLVMBuildCall2(state->builder, signature, helper, args, 7, "");
    return LLVMBuildLoad2(state->builder, box, result_slot,
        "array.new.box");
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
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (!instruction->n_operands
        && value_type(source, type, context) == box_llvm_type(context)) {
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMValueRef extent = LLVMBuildAlloca(state->builder, i64,
                                               "array.empty.extent");
        LLVMBuildStore(state->builder, LLVMConstNull(i64), extent);
        return generic_array_new(state, type, LLVMConstNull(i64), 1, 1,
                                 extent);
    }
    const ir_final_layout_t *layout = ir_layout_final(source, element,
        ir_module_target(source));
    LLVMTypeRef element_type = value_type(source, element, context);
    LLVMTypeRef result_type = value_type(source, type, context);
    uint64_t stride_bits = 0;
    uint64_t packed_width = packed_array_width(source, element);
    if (!layout || !layout->alignment_bytes || !element_type || !result_type
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || (!packed_width && !literal_element_layout_matches(source, element,
            element_type, LLVMGetModuleDataLayout(state->module))))
        return nullptr;
    uint64_t element_align = layout->alignment_bytes;
    uint64_t stride = stride_bits / 8;
    uint64_t alignment = element_align > 8 ? element_align : 8;
    uint64_t descriptor_bytes = sizeof(n00b_abi_array_descriptor_header_v1)
        + sizeof(uint64_t);
    uint64_t data_offset = ((descriptor_bytes + alignment - 1) / alignment)
        * alignment;
    uint64_t count = instruction->n_operands;
    if ((!packed_width && !stride) || alignment > UINT32_MAX
        || (packed_width && count > (UINT64_MAX - 7) / packed_width)
        || (!packed_width
            && count > (UINT64_MAX - data_offset) / stride))
        return nullptr;
    uint64_t payload_bytes = packed_width
        ? (count * packed_width + 7) / 8 : count * stride;
    if (payload_bytes > UINT64_MAX - data_offset) return nullptr;
    uint64_t byte_size = data_offset + (payload_bytes ? payload_bytes : 1);
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
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator)
        allocator = LLVMAddFunction(state->module, allocator_name,
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
    if (packed_width)
        LLVMBuildMemSet(state->builder, data, LLVMConstNull(
            LLVMInt8TypeInContext(context)),
            LLVMConstInt(i64, payload_bytes ? payload_bytes : 1, false), 1);
    for (size_t i = 0; i < instruction->n_operands; i++) {
        ir_operand_t *operand = &instruction->operands[i];
        LLVMValueRef item = aggregate_operand_value(state, operand);
        if (!item || LLVMTypeOf(item) != element_type) return nullptr;
        if (packed_width) {
            LLVMValueRef position = LLVMConstInt(i64,
                (uint64_t)i * packed_width, false);
            LLVMValueRef slot = make_packed_bitptr(state, data, position,
                                                   element);
            if (!slot || !store_packed_bitptr(state, slot, item, element))
                return nullptr;
            continue;
        }
        LLVMValueRef position = LLVMConstInt(i64, (uint64_t)i * stride, false);
        LLVMValueRef slot = LLVMBuildGEP2(state->builder,
            LLVMInt8TypeInContext(context), data, &position, 1,
            "array.element");
        if (!store_gc_barrier(state, item, element)) return nullptr;
        LLVMBuildStore(state->builder, item, slot);
    }
    bool boxed_result = result_type == box_llvm_type(context);
    LLVMTypeRef array_type = result_type;
    if (boxed_result) {
        LLVMTypeRef fields[] = {pointer, pointer};
        array_type = LLVMStructTypeInContext(context, fields, 2, false);
    }
    LLVMValueRef array = LLVMGetUndef(array_type);
    array = LLVMBuildInsertValue(state->builder, array, data, 0,
                                 "array.with.data");
    array = LLVMBuildInsertValue(state->builder, array, storage, 1,
                                 "array.with.descriptor");
    return boxed_result ? box_value(state, array, type) : array;
}

static LLVMValueRef
new_array(lower_state_t *state, ir_instr_t *instruction)
{
    if (!instruction->dest) return nullptr;
    ir_shape_t *shape = ir_value_shape(instruction->dest);
    ir_shape_storage_t storage_shape = {0};
    ir_array_init_t initialization;
    uint32_t rank = shape ? ir_shape_rank(shape) : 0;
    if (!shape || !rank
        || !ir_shape_storage(shape, &storage_shape)
        || !storage_shape.classification_known
        || !storage_shape.flat_prefix_rank
        || storage_shape.flat_prefix_rank > rank
        || instruction->n_operands != storage_shape.flat_prefix_rank
        || !ir_instr_array_init(instruction, &initialization)
        || initialization != IR_ARRAY_INIT_LANGUAGE_ZERO) return nullptr;
    for (uint32_t i = 0; i < instruction->n_operands; i++)
        if (instruction->operands[i].kind != IR_OPD_VALUE) return nullptr;

    ir_module_t *source = state->function->owner;
    th_type *type = th_resolve(instruction->dest->type);
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (value_type(source, type, context) == box_llvm_type(context)) {
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        ir_shape_measure_t measure = {0};
        LLVMValueRef count = ir_shape_element_count(shape, &measure)
            && measure.value ? lowered_value(state, measure.value) : nullptr;
        if (!count || LLVMTypeOf(count) != i64) return nullptr;
        LLVMTypeRef extents_type = LLVMArrayType(i64,
            storage_shape.flat_prefix_rank);
        LLVMValueRef extents = LLVMBuildAlloca(state->builder, extents_type,
            "array.new.extents");
        for (uint32_t i = 0; i < storage_shape.flat_prefix_rank; i++) {
            LLVMValueRef extent = lowered_value(state,
                instruction->operands[i].value);
            if (!extent || LLVMTypeOf(extent) != i64) return nullptr;
            LLVMValueRef indexes[] = {LLVMConstNull(i32),
                LLVMConstInt(i32, i, false)};
            LLVMValueRef slot = LLVMBuildInBoundsGEP2(state->builder,
                extents_type, extents, indexes, 2, "array.new.extent.slot");
            LLVMBuildStore(state->builder, extent, slot);
        }
        return generic_array_new(state, type, count, rank,
                                 storage_shape.flat_prefix_rank, extents);
    }
    th_type *element = type;
    for (uint32_t i = 0; element && i < storage_shape.flat_prefix_rank; i++)
        element = th_kind_of(th_resolve(element)) == TH_SEQUENCE
            ? th_seq_elem(th_resolve(element)) : nullptr;
    const ir_final_layout_t *layout = element
        ? ir_layout_final(source, element, ir_module_target(source)) : nullptr;
    uint64_t stride_bits = 0;
    LLVMTypeRef element_type = element
        ? value_type(source, element, context) : nullptr;
    LLVMTypeRef result_type = value_type(source, type, context);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    uint64_t packed_width = packed_array_width(source, element);
    if (!layout || !layout->alignment_bytes || !element_type || !result_type
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || !stride_bits || stride_bits % 8
        || (!packed_width && !literal_element_layout_matches(source, element,
            element_type, target))) return nullptr;
    uint64_t alignment = layout->alignment_bytes > 8
        ? layout->alignment_bytes : 8;
    uint64_t stride = stride_bits / 8;
    uint64_t descriptor_bytes = sizeof(n00b_abi_array_descriptor_header_v1)
        + (uint64_t)rank * sizeof(uint64_t);
    uint64_t data_offset = (descriptor_bytes + alignment - 1)
        / alignment * alignment;
    uint32_t type_index = runtime_type_index(state, type);
    if (!type_index || !stride || alignment > UINT32_MAX
        || (alignment & (alignment - 1)) || data_offset < descriptor_bytes)
        return nullptr;

    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    ir_shape_measure_t measure = {0};
    if (!ir_shape_element_count(shape, &measure) || !measure.value)
        return nullptr;
    LLVMValueRef count = lowered_value(state, measure.value);
    if (!count || LLVMTypeOf(count) != i64) return nullptr;
    LLVMTypeRef descriptor_type = array_descriptor_type(context);
    LLVMValueRef descriptor = LLVMBuildArrayAlloca(state->builder, i8,
        LLVMConstInt(i64, descriptor_bytes, false), "array.new.descriptor");
    LLVMSetAlignment(descriptor, 8);
    LLVMBuildMemSet(state->builder, descriptor, LLVMConstNull(i8),
        LLVMConstInt(i64, descriptor_bytes, false), 8);
    LLVMValueRef total_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 0, "array.new.total.slot");
    LLVMValueRef rank_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 1, "array.new.rank.slot");
    LLVMValueRef flat_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 2, "array.new.flat.slot");
    LLVMValueRef extent_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 5, "array.new.extent.slot");
    LLVMBuildStore(state->builder, count, total_slot);
    LLVMBuildStore(state->builder, LLVMConstInt(i32, rank, false), rank_slot);
    LLVMBuildStore(state->builder, LLVMConstInt(i32,
        storage_shape.flat_prefix_rank, false), flat_slot);
    for (uint32_t i = 0; i < storage_shape.flat_prefix_rank; i++) {
        LLVMValueRef extent = lowered_value(state,
            instruction->operands[i].value);
        if (!extent || LLVMTypeOf(extent) != i64) return nullptr;
        LLVMValueRef offset = LLVMConstInt(i64, i, false);
        LLVMValueRef slot = LLVMBuildGEP2(state->builder, i64,
            extent_slot, &offset, 1, "array.new.extent");
        LLVMBuildStore(state->builder, extent, slot);
    }

    LLVMValueRef data_bytes;
    if (packed_width) {
        LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
        LLVMValueRef bit_extent = LLVMBuildMul(state->builder,
            LLVMBuildZExt(state->builder, count, i128,
                "array.new.count.wide"),
            LLVMConstInt(i128, packed_width, false),
            "array.new.bit.extent");
        LLVMValueRef bytes_wide = LLVMBuildLShr(state->builder,
            LLVMBuildAdd(state->builder, bit_extent,
                LLVMConstInt(i128, N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.new.rounded.bits"),
            LLVMConstInt(i128, 3, false), "array.new.bytes.wide");
        data_bytes = LLVMBuildTrunc(state->builder, bytes_wide, i64,
            "array.new.data.bytes");
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntUGT,
            bytes_wide,
            LLVMConstInt(i128, UINT64_MAX - data_offset, false),
            "array.new.too.large"));
    } else {
        LLVMValueRef limit = LLVMConstInt(i64,
            (UINT64_MAX - data_offset) / stride, false);
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntUGT,
            count, limit, "array.new.too.large"));
        data_bytes = LLVMBuildMul(state->builder, count,
            LLVMConstInt(i64, stride, false), "array.new.data.bytes");
    }
    LLVMValueRef allocated_bytes = LLVMBuildSelect(state->builder,
        LLVMBuildICmp(state->builder, LLVMIntEQ, count,
            LLVMConstNull(i64), "array.new.empty"),
        LLVMConstInt(i64, 1, false), data_bytes, "array.new.allocated.bytes");
    LLVMValueRef bytes = LLVMBuildAdd(state->builder,
        LLVMConstInt(i64, data_offset, false), allocated_bytes,
        "array.new.total.bytes");
    LLVMTypeRef parameters[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef signature = LLVMFunctionType(pointer, parameters, 5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        LLVMConstInt(i32, type_index, false), bytes,
        LLVMConstInt(i32, alignment, false), descriptor,
    };
    LLVMValueRef storage = LLVMBuildCall2(state->builder, signature,
        allocator, arguments, 5, "array.new.storage");
    LLVMBuildMemCpy(state->builder, storage, 8, descriptor, 8,
        LLVMConstInt(i64, descriptor_bytes, false));
    LLVMValueRef offset = LLVMConstInt(i64, data_offset, false);
    LLVMValueRef data = LLVMBuildGEP2(state->builder, i8, storage, &offset,
        1, "array.new.data");
    LLVMBuildMemSet(state->builder, data, LLVMConstNull(i8), data_bytes,
        (unsigned)alignment);
    LLVMValueRef array = LLVMBuildInsertValue(state->builder,
        LLVMGetUndef(result_type), data, 0, "array.new.with.data");
    return LLVMBuildInsertValue(state->builder, array, storage, 1,
                                "array.new.with.descriptor");
}

static LLVMValueRef
array_flat_item_stride(lower_state_t *state, th_type *type,
                       LLVMValueRef flat_rank, uint32_t first_depth,
                       uint64_t *alignment)
{
    ir_module_t *module = state->function->owner;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMValueRef stride = LLVMConstNull(i64);
    th_type *element = th_resolve(type);
    uint32_t depth = 0;
    *alignment = 8;
    while (element && th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        depth++;
        if (depth < first_depth) continue;
        const ir_final_layout_t *layout = ir_layout_final(module,
            element, ir_module_target(module));
        uint64_t bits = 0;
        if (packed_array_width(module, element)) {
            LLVMValueRef selected = LLVMBuildICmp(state->builder, LLVMIntEQ,
                flat_rank, LLVMConstInt(LLVMTypeOf(flat_rank), depth, false),
                "array.packed.rank.matches");
            stride = LLVMBuildSelect(state->builder, selected,
                LLVMConstNull(i64), stride, "array.packed.item.stride");
            continue;
        }
        if (!layout || !layout->alignment_bytes
            || !ir_layout_final_stride_bits(layout, &bits)
            || !bits || bits % 8
            || !literal_element_layout_matches(module, element,
                value_type(module, element, context), target)) return nullptr;
        if (layout->alignment_bytes > *alignment)
            *alignment = layout->alignment_bytes;
        LLVMValueRef selected = LLVMBuildICmp(state->builder, LLVMIntEQ,
            flat_rank, LLVMConstInt(LLVMTypeOf(flat_rank), depth, false),
            "array.flat.rank.matches");
        stride = LLVMBuildSelect(state->builder, selected,
            LLVMConstInt(i64, bits / 8, false), stride,
            "array.flat.item.stride");
    }
    return stride;
}

static LLVMValueRef
rectangular_suffix_address(lower_state_t *state, LLVMValueRef data,
                           LLVMValueRef descriptor, LLVMValueRef position,
                           LLVMValueRef flat_rank, th_type *parent_type)
{
    ir_module_t *module = state->function->owner;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef header = array_descriptor_type(context);
    th_type *element = th_resolve(parent_type);
    uint32_t rank = 0;
    while (element && th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        rank++;
    }
    th_type *child = th_seq_elem(th_resolve(parent_type));
    uint32_t child_index = child ? runtime_type_index(state, child) : 0;
    if (rank < 2 || !child_index) return nullptr;

    LLVMValueRef rank_slot = LLVMBuildStructGEP2(state->builder, header,
        descriptor, 1, "array.parent.rank.slot");
    LLVMValueRef actual_rank = LLVMBuildLoad2(state->builder, i32,
        rank_slot, "array.parent.rank");
    emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
        actual_rank, LLVMConstInt(i32, rank, false), "array.rank.mismatch"));
    LLVMValueRef total_slot = LLVMBuildStructGEP2(state->builder, header,
        descriptor, 0, "array.parent.total.slot");
    LLVMValueRef total = LLVMBuildLoad2(state->builder, i64,
        total_slot, "array.parent.total");
    LLVMValueRef parent_extents = LLVMBuildStructGEP2(state->builder,
        header, descriptor, 5, "array.parent.extents");
    LLVMValueRef outer_extent = LLVMBuildLoad2(state->builder, i64,
        parent_extents, "array.parent.outer.extent");
    LLVMValueRef row_count = LLVMBuildUDiv(state->builder, total,
        outer_extent, "array.row.count");

    uint64_t packed_width = packed_array_width(module, element);
    LLVMValueRef origin = LLVMConstNull(i32);
    LLVMValueRef offset;
    if (packed_width) {
        LLVMValueRef flags_slot = LLVMBuildStructGEP2(state->builder, header,
            descriptor, 3, "array.parent.flags.slot");
        origin = LLVMBuildLoad2(state->builder, i32, flags_slot,
            "array.parent.bit.origin");
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
            LLVMBuildAnd(state->builder, origin,
                LLVMConstInt(i32, ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.parent.invalid.flags"), LLVMConstNull(i32),
            "array.parent.flags.invalid"));
        LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
        LLVMValueRef bit_offset = LLVMBuildAdd(state->builder,
            LLVMBuildMul(state->builder,
                LLVMBuildMul(state->builder,
                    LLVMBuildZExt(state->builder, position, i128,
                        "array.row.position.wide"),
                    LLVMBuildZExt(state->builder, row_count, i128,
                        "array.row.count.wide"), "array.row.items"),
                LLVMConstInt(i128, packed_width, false), "array.row.bits"),
            LLVMBuildZExt(state->builder, origin, i128,
                "array.row.origin.wide"), "array.row.bit.offset");
        offset = LLVMBuildTrunc(state->builder,
            LLVMBuildLShr(state->builder, bit_offset,
                LLVMConstInt(i128, 3, false), "array.row.byte.offset.wide"),
            i64, "array.row.byte.offset");
        origin = LLVMBuildTrunc(state->builder,
            LLVMBuildAnd(state->builder, bit_offset,
                LLVMConstInt(i128, N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.row.bit.origin.wide"), i32,
            "array.row.bit.origin");
    } else {
        uint64_t alignment = 0;
        LLVMValueRef stride = array_flat_item_stride(state, parent_type,
            flat_rank, 2, &alignment);
        if (!stride) return nullptr;
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntEQ,
            stride, LLVMConstNull(i64), "array.flat.rank.invalid"));
        LLVMValueRef row_bytes = LLVMBuildMul(state->builder, row_count,
            stride, "array.row.bytes");
        offset = LLVMBuildMul(state->builder, position,
            row_bytes, "array.row.offset");
    }
    LLVMValueRef row_data = LLVMBuildGEP2(state->builder, i8, data,
        &offset, 1, "array.row.data");

    uint64_t descriptor_bytes = sizeof(n00b_abi_array_descriptor_header_v1)
        + (uint64_t)(rank - 1) * sizeof(uint64_t);
    uint64_t value_bytes = sizeof(n00b_abi_array_value_v1);
    LLVMTypeRef parameters[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef signature = LLVMFunctionType(pointer, parameters, 5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        LLVMConstInt(i32, child_index, false),
        LLVMConstInt(i64, value_bytes + descriptor_bytes, false),
        LLVMConstInt(i32, 8, false), LLVMConstNull(pointer),
    };
    LLVMValueRef wrapper = LLVMBuildCall2(state->builder, signature,
        allocator, arguments, 5, "array.suffix.storage");
    LLVMValueRef descriptor_offset = LLVMConstInt(i64, value_bytes, false);
    LLVMValueRef suffix = LLVMBuildGEP2(state->builder, i8, wrapper,
        &descriptor_offset, 1, "array.suffix.descriptor");
    LLVMBuildMemSet(state->builder, suffix, LLVMConstNull(i8),
        LLVMConstInt(i64, descriptor_bytes, false), 8);
    LLVMValueRef slot = LLVMBuildStructGEP2(state->builder, header, suffix,
        0, "array.suffix.total.slot");
    LLVMBuildStore(state->builder, row_count, slot);
    slot = LLVMBuildStructGEP2(state->builder, header, suffix,
        1, "array.suffix.rank.slot");
    LLVMBuildStore(state->builder, LLVMConstInt(i32, rank - 1, false), slot);
    slot = LLVMBuildStructGEP2(state->builder, header, suffix,
        2, "array.suffix.flat.slot");
    LLVMValueRef suffix_flat = LLVMBuildSub(state->builder, flat_rank,
        LLVMConstInt(i32, 1, false), "array.suffix.flat.rank");
    LLVMBuildStore(state->builder, suffix_flat, slot);
    if (packed_width) {
        slot = LLVMBuildStructGEP2(state->builder, header, suffix, 3,
            "array.suffix.flags.slot");
        LLVMBuildStore(state->builder, origin, slot);
    }
    LLVMValueRef suffix_extents = LLVMBuildStructGEP2(state->builder,
        header, suffix, 5, "array.suffix.extents");
    for (uint32_t i = 1; i < rank; i++) {
        LLVMValueRef source_offset = LLVMConstInt(i64, i, false);
        LLVMValueRef source = LLVMBuildGEP2(state->builder, i64,
            parent_extents, &source_offset, 1, "array.parent.extent");
        LLVMValueRef extent = LLVMBuildLoad2(state->builder, i64,
            source, "array.parent.extent.value");
        LLVMValueRef target_offset = LLVMConstInt(i64, i - 1, false);
        LLVMValueRef target = LLVMBuildGEP2(state->builder, i64,
            suffix_extents, &target_offset, 1, "array.suffix.extent");
        LLVMBuildStore(state->builder, extent, target);
    }
    LLVMTypeRef child_type = value_type(module, child, context);
    if (!child_type) return nullptr;
    LLVMValueRef value = LLVMBuildInsertValue(state->builder,
        LLVMGetUndef(child_type), row_data, 0, "array.suffix.data");
    value = LLVMBuildInsertValue(state->builder, value, suffix, 1,
                                 "array.suffix.value");
    LLVMBuildStore(state->builder, value, wrapper);
    return wrapper;
}

static bool
sequence_value_transfers_storage(ir_value_t *source)
{
    if (!source || !source->def) return false;
    if (source->def->op == IR_OP_AGGREGATE
        || source->def->op == IR_OP_ARRAY_NEW
        || source->def->op == IR_OP_CONSUME
        || source->def->op == IR_OP_CALL
        || source->def->op == IR_OP_PHASE_RESULT) return true;
    return false;
}

static bool
return_value_transfers_storage(ir_func_t *function, ir_value_t *source)
{
    if (sequence_value_transfers_storage(source)) return true;
    if (!function || !source || source->kind != IR_VAL_PARAM
        || source->index >= th_func_nparams(function->type)) return false;
    const th_param *formal = th_func_param(function->type, source->index);
    return formal && formal->mode == TH_CONSUME;
}

static LLVMValueRef copy_boxed_array(lower_state_t *state,
                                     LLVMValueRef value);
static LLVMValueRef copy_literal_value(lower_state_t *state,
                                       LLVMValueRef value, th_type *type);

static LLVMValueRef
copy_sequence_value(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    ir_module_t *owner = state->function->owner;
    th_type *sequence = th_resolve(type);
    if (!value || !sequence || th_kind_of(sequence) != TH_SEQUENCE)
        return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef array_type = value_type(owner, type, context);
    if (!array_type || LLVMTypeOf(value) != array_type) return nullptr;
    if (array_type == box_llvm_type(context))
        return copy_boxed_array(state, value);
    uint32_t type_index = runtime_type_index(state, type);
    uint64_t alignment = 0;
    if (!type_index) return nullptr;

    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef descriptor_type = array_descriptor_type(context);
    LLVMValueRef source_data = LLVMBuildExtractValue(state->builder, value,
        0, "array.copy.source");
    LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder, value,
        1, "array.copy.descriptor");
    if (!source_data || !descriptor) return nullptr;
    LLVMValueRef count_address = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 0, "array.copy.count.address");
    LLVMValueRef rank_address = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 1, "array.copy.rank.address");
    LLVMValueRef count = LLVMBuildLoad2(state->builder, i64, count_address,
        "array.copy.count");
    LLVMValueRef rank = LLVMBuildLoad2(state->builder, i32, rank_address,
        "array.copy.rank");
    LLVMValueRef flat_address = LLVMBuildStructGEP2(state->builder,
        descriptor_type, descriptor, 2, "array.copy.flat.address");
    LLVMValueRef flat_rank = LLVMBuildLoad2(state->builder, i32,
        flat_address, "array.copy.flat.rank");
    LLVMValueRef stride = array_flat_item_stride(state, type, flat_rank,
        1, &alignment);
    if (!stride || alignment > UINT32_MAX || (alignment & (alignment - 1)))
        return nullptr;
    th_type *leaf = sequence;
    while (leaf && th_kind_of(leaf) == TH_SEQUENCE)
        leaf = th_resolve(th_seq_elem(leaf));
    uint64_t packed_width = packed_array_width(owner, leaf);
    LLVMValueRef packed = LLVMBuildICmp(state->builder, LLVMIntEQ,
        stride, LLVMConstNull(i64), "array.copy.packed");
    if (!packed_width) emit_trap_if(state, packed);
    LLVMValueRef descriptor_bytes = LLVMBuildAdd(state->builder,
        LLVMConstInt(i64, sizeof(n00b_abi_array_descriptor_header_v1),
                     false),
        LLVMBuildMul(state->builder,
            LLVMBuildZExt(state->builder, rank, i64, "array.copy.rank64"),
            LLVMConstInt(i64, sizeof(uint64_t), false),
            "array.copy.extents.bytes"), "array.copy.descriptor.bytes");
    LLVMValueRef data_offset = LLVMBuildAnd(state->builder,
        LLVMBuildAdd(state->builder, descriptor_bytes,
            LLVMConstInt(i64, alignment - 1, false),
            "array.copy.aligned.bytes"),
        LLVMConstInt(i64, ~(alignment - 1), false),
        "array.copy.data.offset");
    LLVMValueRef safe_stride = packed_width
        ? LLVMBuildSelect(state->builder, packed,
            LLVMConstInt(i64, 1, false), stride,
            "array.copy.safe.stride") : stride;
    LLVMValueRef maximum = LLVMBuildUDiv(state->builder,
        LLVMBuildSub(state->builder,
            LLVMConstInt(i64, UINT64_MAX, false), data_offset,
            "array.copy.available.bytes"),
        safe_stride, "array.copy.maximum");
    LLVMValueRef regular_bytes = LLVMBuildMul(state->builder, count,
        safe_stride, "array.copy.regular.bytes");
    LLVMValueRef data_bytes = regular_bytes;
    if (packed_width) {
        LLVMValueRef flags_address = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 3, "array.copy.flags.address");
        LLVMValueRef flags = LLVMBuildLoad2(state->builder, i32,
            flags_address, "array.copy.flags");
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
            LLVMBuildAnd(state->builder, flags,
                LLVMConstInt(i32, ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.copy.invalid.flags"), LLVMConstNull(i32),
            "array.copy.flags.invalid"));
        LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
        LLVMValueRef bit_extent = LLVMBuildAdd(state->builder,
            LLVMBuildMul(state->builder,
                LLVMBuildZExt(state->builder, count, i128,
                    "array.copy.count.wide"),
                LLVMConstInt(i128, packed_width, false),
                "array.copy.element.bits"),
            LLVMBuildZExt(state->builder, flags, i128,
                "array.copy.origin.wide"), "array.copy.bit.extent");
        LLVMValueRef packed_bytes = LLVMBuildTrunc(state->builder,
            LLVMBuildLShr(state->builder,
                LLVMBuildAdd(state->builder, bit_extent,
                    LLVMConstInt(i128, N00B_ABI_ARRAY_ORIGIN_BIT_MASK,
                                 false), "array.copy.rounded.bits"),
                LLVMConstInt(i128, 3, false), "array.copy.packed.bytes.wide"),
            i64, "array.copy.packed.bytes");
        data_bytes = LLVMBuildSelect(state->builder, packed, packed_bytes,
            regular_bytes, "array.copy.data.bytes");
    }
    emit_trap_if(state, LLVMBuildOr(state->builder,
        LLVMBuildAnd(state->builder,
            LLVMBuildNot(state->builder, packed,
                "array.copy.not.packed"),
            LLVMBuildICmp(state->builder, LLVMIntUGT,
                count, maximum, "array.copy.regular.too.large"),
            "array.copy.regular.overflow"),
        LLVMBuildICmp(state->builder, LLVMIntUGT, data_bytes,
            LLVMBuildSub(state->builder,
                LLVMConstInt(i64, UINT64_MAX, false), data_offset,
                "array.copy.available.data"),
            "array.copy.data.too.large"), "array.copy.too.large"));
    LLVMValueRef extent = LLVMBuildAdd(state->builder, data_offset,
        LLVMBuildSelect(state->builder,
            LLVMBuildICmp(state->builder, LLVMIntEQ, count,
                LLVMConstNull(i64), "array.copy.empty"),
            LLVMConstInt(i64, 1, false), data_bytes,
            "array.copy.allocated.bytes"), "array.copy.total.bytes");
    LLVMTypeRef parameters[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef signature = LLVMFunctionType(pointer, parameters, 5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                   allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
                                                 allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        LLVMConstInt(i32, type_index, false), extent,
        LLVMConstInt(i32, alignment, false), descriptor,
    };
    LLVMValueRef storage = LLVMBuildCall2(state->builder, signature,
        allocator, arguments, 5, "array.copy.storage");
    if (!storage) return nullptr;
    LLVMBuildMemCpy(state->builder, storage, 8, descriptor, 8,
                    descriptor_bytes);
    LLVMValueRef data = LLVMBuildGEP2(state->builder,
        LLVMInt8TypeInContext(context), storage, &data_offset, 1,
        "array.copy.data");
    LLVMBuildMemCpy(state->builder, data, 1, source_data, 1, data_bytes);
    LLVMValueRef copied = LLVMBuildInsertValue(state->builder, value, data,
        0, "array.copy.with.data");
    return LLVMBuildInsertValue(state->builder, copied, storage, 1,
                                "array.copy.with.descriptor");
}

static LLVMValueRef
copy_literal_value(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    if (!value || !type) return nullptr;
    ir_module_t *module = state->function->owner;
    th_type *resolved = th_resolve(type);
    if (th_kind_of(resolved) == TH_SEQUENCE)
        return copy_sequence_value(state, value, type);
    th_type *definition = th_kind_of(resolved) == TH_NAMED
        ? ir_type_definition(module, resolved) : resolved;
    if (!definition || th_kind_of(definition) != TH_RECORD)
        return value;
    const ir_final_layout_t *layout = ir_layout_final(module, type,
        ir_module_target(module));
    LLVMTypeRef native = LLVMTypeOf(value);
    if (!layout || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != layout->n_members)
        return nullptr;
    size_t *order = calloc(layout->n_members ? layout->n_members : 1,
                           sizeof(*order));
    if (!order) return nullptr;
    record_layout_order(layout, order);
    LLVMValueRef copied = value;
    for (size_t i = 0; i < layout->n_members; i++) {
        th_type *field_type = layout->members[order[i]].type;
        LLVMValueRef field = LLVMBuildExtractValue(state->builder, copied,
            (unsigned)i, "literal.field");
        LLVMValueRef new_field = copy_literal_value(state, field, field_type);
        if (!new_field) {
            copied = nullptr;
            break;
        }
        if (new_field != field)
            copied = LLVMBuildInsertValue(state->builder, copied, new_field,
                (unsigned)i, "literal.field.copy");
    }
    free(order);
    return copied;
}

static bool
copy_rectangular_row(lower_state_t *state, LLVMValueRef address,
                     LLVMValueRef value, th_type *type)
{
    ir_module_t *module = state->function->owner;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef native = value_type(module, type, context);
    LLVMTypeRef descriptor_type = array_descriptor_type(context);
    if (!native || LLVMTypeOf(value) != native) return false;
    th_type *element = th_resolve(type);
    uint32_t rank = 0;
    while (element && th_kind_of(element) == TH_SEQUENCE) {
        element = th_resolve(th_seq_elem(element));
        rank++;
    }
    const ir_final_layout_t *layout = element
        ? ir_layout_final(module, element, ir_module_target(module)) : nullptr;
    uint64_t stride_bits = 0;
    uint64_t packed_width = packed_array_width(module, element);
    if (!rank || !layout || !layout->alignment_bytes
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || !stride_bits || stride_bits % 8
        || (!packed_width && !literal_element_layout_matches(module, element,
            value_type(module, element, context),
            LLVMGetModuleDataLayout(state->module)))) return false;

    LLVMValueRef target = LLVMBuildLoad2(state->builder, native, address,
        "array.row.target");
    LLVMValueRef target_data = LLVMBuildExtractValue(state->builder,
        target, 0, "array.row.target.data");
    LLVMValueRef source_data = LLVMBuildExtractValue(state->builder,
        value, 0, "array.row.source.data");
    LLVMValueRef target_descriptor = LLVMBuildExtractValue(state->builder,
        target, 1, "array.row.target.descriptor");
    LLVMValueRef source_descriptor = LLVMBuildExtractValue(state->builder,
        value, 1, "array.row.source.descriptor");
    LLVMValueRef target_count_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, target_descriptor, 0, "array.row.target.count.slot");
    LLVMValueRef source_count_slot = LLVMBuildStructGEP2(state->builder,
        descriptor_type, source_descriptor, 0, "array.row.source.count.slot");
    LLVMValueRef count = LLVMBuildLoad2(state->builder, i64,
        target_count_slot, "array.row.count");
    LLVMValueRef source_count = LLVMBuildLoad2(state->builder, i64,
        source_count_slot, "array.row.source.count");
    LLVMValueRef invalid = LLVMBuildICmp(state->builder, LLVMIntNE,
        count, source_count, "array.row.count.mismatch");
    for (uint32_t which = 0; which < 2; which++) {
        LLVMValueRef descriptor = which ? source_descriptor : target_descriptor;
        LLVMValueRef rank_slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 1, "array.row.rank.slot");
        LLVMValueRef flat_slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 2, "array.row.flat.slot");
        LLVMValueRef actual_rank = LLVMBuildLoad2(state->builder, i32,
            rank_slot, "array.row.rank");
        LLVMValueRef flat_rank = LLVMBuildLoad2(state->builder, i32,
            flat_slot, "array.row.flat.rank");
        invalid = LLVMBuildOr(state->builder, invalid,
            LLVMBuildICmp(state->builder, LLVMIntNE, actual_rank,
                LLVMConstInt(i32, rank, false), "array.row.rank.mismatch"),
            "array.row.invalid.rank");
        invalid = LLVMBuildOr(state->builder, invalid,
            LLVMBuildICmp(state->builder, LLVMIntNE, flat_rank,
                LLVMConstInt(i32, rank, false), "array.row.flat.mismatch"),
            "array.row.invalid.flat");
    }
    LLVMValueRef target_extents = LLVMBuildStructGEP2(state->builder,
        descriptor_type, target_descriptor, 5, "array.row.target.extents");
    LLVMValueRef source_extents = LLVMBuildStructGEP2(state->builder,
        descriptor_type, source_descriptor, 5, "array.row.source.extents");
    for (uint32_t i = 0; i < rank; i++) {
        LLVMValueRef offset = LLVMConstInt(i64, i, false);
        LLVMValueRef target_extent = LLVMBuildLoad2(state->builder, i64,
            LLVMBuildGEP2(state->builder, i64, target_extents, &offset, 1,
                "array.row.target.extent.slot"), "array.row.target.extent");
        LLVMValueRef source_extent = LLVMBuildLoad2(state->builder, i64,
            LLVMBuildGEP2(state->builder, i64, source_extents, &offset, 1,
                "array.row.source.extent.slot"), "array.row.source.extent");
        invalid = LLVMBuildOr(state->builder, invalid,
            LLVMBuildICmp(state->builder, LLVMIntNE, target_extent,
                source_extent, "array.row.extent.mismatch"),
            "array.row.invalid.extent");
    }
    emit_trap_if(state, invalid);
    if (packed_width) {
        LLVMValueRef target_flags_slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, target_descriptor, 3, "array.row.target.flags.slot");
        LLVMValueRef source_flags_slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, source_descriptor, 3, "array.row.source.flags.slot");
        LLVMValueRef target_origin = LLVMBuildLoad2(state->builder, i32,
            target_flags_slot, "array.row.target.origin");
        LLVMValueRef source_origin = LLVMBuildLoad2(state->builder, i32,
            source_flags_slot, "array.row.source.origin");
        LLVMValueRef flags = LLVMBuildOr(state->builder,
            target_origin, source_origin, "array.row.flags");
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
            LLVMBuildAnd(state->builder, flags,
                LLVMConstInt(i32, ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.row.invalid.flags"), LLVMConstNull(i32),
            "array.row.flags.invalid"));
        LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
        LLVMValueRef bit_count = LLVMBuildMul(state->builder,
            LLVMBuildZExt(state->builder, count, i128,
                "array.row.count.wide"),
            LLVMConstInt(i128, packed_width, false),
            "array.row.bit.count");
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntUGT,
            bit_count, LLVMConstInt(i128, UINT64_MAX, false),
            "array.row.bit.count.too.large"));
        return copy_bool_bits(state, target_data, target_origin,
            source_data, source_origin,
            LLVMBuildTrunc(state->builder, bit_count, i64,
                "array.row.bit.count64"),
            th_name(ir_module_types(module), "bool"));
    }
    if (!store_gc_barrier(state, value, type)) return false;
    LLVMValueRef bytes = LLVMBuildMul(state->builder, count,
        LLVMConstInt(i64, stride_bits / 8, false), "array.row.bytes");
    return LLVMBuildMemMove(state->builder, target_data,
        layout->alignment_bytes, source_data, layout->alignment_bytes,
        bytes) != nullptr;
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
cached_reloc_array_value(lower_state_t *state, ir_module_t *source,
                         th_type *element,
                         const n00bb_llvm_phase_array_t *cached)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    LLVMTypeRef native = element
        ? value_type(source, element, context) : nullptr;
    const ir_final_layout_t *layout = element
        ? ir_layout_final(source, element, ir_module_target(source)) : nullptr;
    if (!cached || !native || !layout || !layout->alignment_bytes
        || !literal_element_layout_matches(source,
            element, native, target)) return nullptr;
    if (!cached->bytes || !cached->element_stride
        || !cached->string_offsets || !cached->string_offset_count
        || cached->string_offset_count > (UINT32_MAX - 1) / 2
        || cached->count > UINT32_MAX
        || LLVMABISizeOfType(target, native) != cached->element_stride)
        return nullptr;
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

static LLVMValueRef
materialized_reloc_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    th_type *element = type && th_kind_of(type) == TH_SEQUENCE
        ? th_seq_elem(type) : nullptr;
    if (!identifier || !element) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (cached->kind == N00BB_LLVM_LITERAL_RELOC
            && cached->identifier
            && !strcmp(identifier, cached->identifier))
            return cached_reloc_array_value(state, state->function->owner,
                                            element, cached);
    }
    return nullptr;
}

static LLVMValueRef
cached_record_fields_array_value(lower_state_t *state, ir_module_t *source,
                                 const ir_final_layout_t *layout,
                                 LLVMTypeRef native,
                                 const n00bb_llvm_phase_array_t *cached)
{
    if (!layout || !native || !cached || !cached->bytes
        || !cached->element_stride
        || layout->n_members < 2
        || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != layout->n_members)
        return nullptr;
    uint64_t stride_bits = 0;
    if (!ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8 || stride_bits / 8 > cached->byte_len)
        return nullptr;
    size_t record_size = (size_t)(stride_bits / 8);
    bool reloc = cached->kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC;
    n00bb_llvm_phase_array_t array = *cached;
    array.bytes += record_size;
    array.byte_len -= record_size;
    array.kind = reloc ? N00BB_LLVM_LITERAL_RELOC
                       : N00BB_LLVM_LITERAL_ARRAY;
    if (!reloc && (cached->count > SIZE_MAX / cached->element_stride
        || array.byte_len != cached->count * cached->element_stride))
        return nullptr;
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    size_t *order = calloc(layout->n_members, sizeof(*order));
    LLVMValueRef *fields = calloc(layout->n_members, sizeof(*fields));
    if (!order || !fields) {
        free(order);
        free(fields);
        return nullptr;
    }
    record_layout_order(layout, order);
    bool valid = true;
    size_t arrays = 0;
    for (size_t i = 0; valid && i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[order[i]];
        LLVMTypeRef field_type = LLVMStructGetTypeAtIndex(native,
                                                          (unsigned)i);
        th_type *field = member->type ? th_resolve(member->type) : nullptr;
        if (!field || member->bit_offset % 8
            || LLVMOffsetOfElement(target, native, (unsigned)i)
                != member->bit_offset / 8) {
            valid = false;
            break;
        }
        size_t offset = (size_t)(member->bit_offset / 8);
        uint64_t field_bytes = LLVMABISizeOfType(target, field_type);
        if (offset > record_size || field_bytes > record_size - offset) {
            valid = false;
            break;
        }
        if (th_kind_of(field) == TH_SEQUENCE) {
            arrays++;
            th_type *element = th_seq_elem(field);
            fields[i] = reloc
                ? cached_reloc_array_value(state, source, element, &array)
                : cached_array_value(state, source, element, &array);
            valid = fields[i] && LLVMTypeOf(fields[i]) == field_type;
            continue;
        }
        if (LLVMGetTypeKind(field_type) == LLVMIntegerTypeKind) {
            unsigned width = LLVMGetIntTypeWidth(field_type);
            if (!width || width > 128 || field_bytes > 16) {
                valid = false;
                break;
            }
            uint64_t words[2] = {0};
            for (size_t b = 0; b < field_bytes; b++)
                words[b / 8] |= (uint64_t)cached->bytes[offset + b]
                    << ((b % 8) * 8);
            fields[i] = width <= 64
                ? LLVMConstInt(field_type, words[0], false)
                : LLVMConstIntOfArbitraryPrecision(field_type, 2, words);
        } else if (LLVMGetTypeKind(field_type) == LLVMDoubleTypeKind
                   && field_bytes == 8) {
            uint64_t bits = 0;
            memcpy(&bits, cached->bytes + offset, 8);
            fields[i] = LLVMConstBitCast(LLVMConstInt(
                LLVMInt64TypeInContext(context), bits, false), field_type);
        } else if (LLVMGetTypeKind(field_type) == LLVMFloatTypeKind
                   && field_bytes == 4) {
            uint32_t bits = 0;
            memcpy(&bits, cached->bytes + offset, 4);
            fields[i] = LLVMConstBitCast(LLVMConstInt(
                LLVMInt32TypeInContext(context), bits, false), field_type);
        } else valid = false;
    }
    LLVMValueRef record = valid && arrays == 1
        ? LLVMConstStructInContext(context, fields,
            (unsigned)layout->n_members, false) : nullptr;
    free(fields);
    free(order);
    return record && LLVMTypeOf(record) == native ? record : nullptr;
}

static LLVMValueRef
cached_record_array_value(lower_state_t *state, ir_module_t *source,
                          th_type *type, const char *identifier)
{
    const ir_final_layout_t *layout = type
        ? ir_layout_final(source, type, ir_module_target(source)) : nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = type ? value_type(source, type, context) : nullptr;
    th_type *field = layout && layout->n_members == 1
        ? th_resolve(layout->members[0].type) : nullptr;
    th_type *element = field && th_kind_of(field) == TH_SEQUENCE
        ? th_seq_elem(field) : nullptr;
    if (!identifier || !native || !layout)
        return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (!cached->identifier || strcmp(identifier, cached->identifier))
            continue;
        if (cached->kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
            || cached->kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC)
            return cached_record_fields_array_value(state, source,
                                                    layout, native, cached);
        if (!element || layout->members[0].bit_offset
            || layout->intrinsic_bits
                != sizeof(n00b_abi_array_value_v1) * 8)
            return nullptr;
        LLVMValueRef array = cached->kind == N00BB_LLVM_LITERAL_RECORD_ARRAY
            ? cached_array_value(state, source, element, cached)
            : cached->kind == N00BB_LLVM_LITERAL_RECORD_RELOC
                ? cached_reloc_array_value(state, source, element, cached)
                : nullptr;
        LLVMValueRef record = array
            ? LLVMConstStructInContext(context, &array, 1, false) : nullptr;
        return record && LLVMTypeOf(record) == native ? record : nullptr;
    }
    return nullptr;
}

static LLVMValueRef
materialized_record_array(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    return cached_record_array_value(state, state->function->owner, type,
                                     identifier);
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
materialized_graph_value(lower_state_t *state, ir_instr_t *instruction)
{
    const char *identifier = ir_instr_phase_result(instruction);
    if (!identifier || !instruction->dest) return nullptr;
    for (size_t i = 0; i < state->phase_array_count; i++) {
        const n00bb_llvm_phase_array_t *cached = &state->phase_arrays[i];
        if (cached->kind != N00BB_LLVM_LITERAL_GRAPH
            || !cached->identifier
            || strcmp(identifier, cached->identifier)) continue;
        size_t count = cached->graph_node_count;
        if (!cached->graph_nodes || !count || count > UINT32_MAX
            || cached->graph_edge_count > UINT32_MAX
            || (cached->graph_edge_count && !cached->graph_edges)
            || cached->graph_root_index >= count) return nullptr;
        size_t *order = calloc(count, sizeof(*order));
        size_t *local = calloc(count, sizeof(*local));
        if (!order || !local) {
            free(order);
            free(local);
            return nullptr;
        }
        for (size_t n = 0; n < count; n++) local[n] = SIZE_MAX;
        size_t used = 1;
        order[0] = cached->graph_root_index;
        local[order[0]] = 0;
        for (size_t read = 0; read < used; read++)
            for (size_t e = 0; e < cached->graph_edge_count; e++) {
                const n00bb_static_graph_edge_t *edge =
                    &cached->graph_edges[e];
                if (edge->source >= count || edge->target >= count) {
                    used = 0;
                    break;
                }
                if (edge->source == order[read]
                    && local[edge->target] == SIZE_MAX) {
                    local[edge->target] = used;
                    order[used++] = edge->target;
                }
            }
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef node_fields[] = {pointer, i64, i32, i32, i32, i32};
        LLVMTypeRef edge_fields[] = {i32, i32, i64, i64};
        LLVMTypeRef node_type = LLVMStructTypeInContext(context,
            node_fields, 6, false);
        LLVMTypeRef edge_type = LLVMStructTypeInContext(context,
            edge_fields, 4, false);
        LLVMValueRef *nodes = used
            ? calloc(used, sizeof(*nodes)) : nullptr;
        LLVMValueRef *edges = cached->graph_edge_count
            ? calloc(cached->graph_edge_count, sizeof(*edges)) : nullptr;
        bool valid = used && nodes
            && (!cached->graph_edge_count || edges);
        for (size_t n = 0; valid && n < used; n++) {
            const n00bb_llvm_graph_node_t *node =
                &cached->graph_nodes[order[n]];
            const n00bb_llvm_type_index_t *type_index = nullptr;
            for (size_t t = 0; t < state->type_index_count; t++)
                if (state->type_indices[t].typehash == node->typehash) {
                    type_index = &state->type_indices[t];
                    break;
                }
            LLVMValueRef source = node->symbol
                ? LLVMGetNamedGlobal(state->module, node->symbol) : nullptr;
            if (!source && node->symbol)
                source = LLVMAddGlobal(state->module,
                    LLVMInt8TypeInContext(context), node->symbol);
            valid = source && (node->immutable
                || (type_index && type_index->index))
                && node->size && node->alignment;
            if (!valid) break;
            LLVMValueRef fields[] = {
                source,
                LLVMConstInt(i64, node->size, false),
                LLVMConstInt(i32, type_index ? type_index->index : 0, false),
                LLVMConstInt(i32, node->alignment, false),
                LLVMConstInt(i32,
                    node->allocation && type_index
                        && type_index->array_type, false),
                LLVMConstInt(i32, node->immutable, false),
            };
            nodes[n] = LLVMConstStructInContext(context, fields, 6, false);
        }
        size_t edge_count = 0;
        for (size_t e = 0; valid && e < cached->graph_edge_count; e++) {
            const n00bb_static_graph_edge_t *edge =
                &cached->graph_edges[e];
            if (local[edge->source] == SIZE_MAX) continue;
            if (local[edge->target] == SIZE_MAX) {
                valid = false;
                break;
            }
            LLVMValueRef fields[] = {
                LLVMConstInt(i32, local[edge->source], false),
                LLVMConstInt(i32, local[edge->target], false),
                LLVMConstInt(i64, edge->source_offset, false),
                LLVMConstInt(i64, edge->target_offset, false),
            };
            edges[edge_count++] = LLVMConstStructInContext(context,
                fields, 4, false);
        }
        LLVMValueRef result = nullptr;
        if (valid) {
            LLVMValueRef node_values = LLVMConstArray(node_type, nodes,
                (unsigned)used);
            LLVMValueRef node_global = LLVMAddGlobal(state->module,
                LLVMTypeOf(node_values), "literal.graph.nodes");
            LLVMSetInitializer(node_global, node_values);
            LLVMSetGlobalConstant(node_global, true);
            LLVMSetLinkage(node_global, LLVMPrivateLinkage);
            LLVMValueRef edge_global = LLVMConstNull(pointer);
            if (edge_count) {
                LLVMValueRef edge_values = LLVMConstArray(edge_type, edges,
                    (unsigned)edge_count);
                edge_global = LLVMAddGlobal(state->module,
                    LLVMTypeOf(edge_values), "literal.graph.edges");
                LLVMSetInitializer(edge_global, edge_values);
                LLVMSetGlobalConstant(edge_global, true);
                LLVMSetLinkage(edge_global, LLVMPrivateLinkage);
            }
            LLVMTypeRef parameters[] = {pointer, i32, pointer, i32, i32};
            LLVMTypeRef signature = LLVMFunctionType(pointer,
                parameters, 5, false);
            LLVMValueRef clone = LLVMGetNamedFunction(state->module,
                "n00bb_literal_graph_clone");
            if (!clone) clone = LLVMAddFunction(state->module,
                "n00bb_literal_graph_clone", signature);
            LLVMTypeRef value = value_type(state->function->owner,
                instruction->dest->type, context);
            if (clone && value) {
                LLVMValueRef arguments[] = {
                    node_global,
                    LLVMConstInt(i32, used, false),
                    edge_global,
                    LLVMConstInt(i32, edge_count, false),
                    LLVMConstNull(i32),
                };
                LLVMValueRef copied = LLVMBuildCall2(state->builder,
                    signature, clone, arguments, 5, "literal.graph.copy");
                result = LLVMBuildLoad2(state->builder, value, copied,
                    "literal.graph");
            }
        }
        free(edges);
        free(nodes);
        free(local);
        free(order);
        return result;
    }
    return nullptr;
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
runtime_trait_index(lower_state_t *state, const char *qualified_name)
{
    th_ctx *types = ir_module_types(state->function->owner);
    const th_env *environment = th_ctx_environment(types);
    if (!environment || !qualified_name) return 0;
    for (size_t i = 0; i < th_env_declaration_count(environment); i++) {
        th_env_declaration_view_t declaration;
        if (!th_env_declaration_at(environment, i, &declaration)) return 0;
        if (declaration.kind == TH_ENV_DECLARATION_TRAIT
            && declaration.qualified_name
            && !strcmp(declaration.qualified_name, qualified_name)
            && declaration.contract)
            return runtime_type_index(state, declaration.contract);
    }
    return 0;
}

static LLVMValueRef
runtime_type_component(lower_state_t *state, LLVMValueRef type_index,
                       uint32_t ordinal)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef parameters[] = {i32, i32};
    LLVMTypeRef signature = LLVMFunctionType(i32, parameters, 2, false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_type_component");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_type_component", signature);
    if (!type_index || !helper || LLVMGlobalGetValueType(helper) != signature)
        return nullptr;
    LLVMValueRef args[] = {type_index, LLVMConstInt(i32, ordinal, false)};
    return LLVMBuildCall2(state->builder, signature, helper, args, 2,
                          "generic.component");
}

static bool
type_contains_variable(th_type *pattern, th_type *variable)
{
    pattern = th_resolve(pattern);
    variable = th_resolve(variable);
    if (!pattern || !variable) return false;
    if (th_kind_of(pattern) == TH_VAR) return pattern == variable;
    if (th_kind_of(pattern) == TH_SEQUENCE)
        return type_contains_variable(th_seq_elem(pattern), variable);
    if (th_kind_of(pattern) == TH_NAMED) {
        for (size_t i = 0; i < th_named_nargs(pattern); i++) {
            const th_arg *argument = th_named_arg(pattern, i);
            if (argument && argument->kind == TH_ARG_TYPE
                && type_contains_variable(argument->type, variable))
                return true;
        }
    }
    return false;
}

static LLVMValueRef
variable_from_pattern(lower_state_t *state, th_type *pattern,
                      th_type *variable, LLVMValueRef actual_type)
{
    pattern = th_resolve(pattern);
    variable = th_resolve(variable);
    if (!pattern || !variable) return nullptr;
    if (th_kind_of(pattern) == TH_VAR)
        return pattern == variable ? actual_type : nullptr;
    if (th_kind_of(pattern) == TH_SEQUENCE) {
        th_type *child = th_seq_elem(pattern);
        return type_contains_variable(child, variable)
            ? variable_from_pattern(state, child, variable,
                runtime_type_component(state, actual_type, 0)) : nullptr;
    }
    if (th_kind_of(pattern) == TH_NAMED) {
        for (size_t i = 0; i < th_named_nargs(pattern); i++) {
            const th_arg *argument = th_named_arg(pattern, i);
            if (argument && argument->kind == TH_ARG_TYPE
                && type_contains_variable(argument->type, variable))
                return variable_from_pattern(state, argument->type, variable,
                    runtime_type_component(state, actual_type, (uint32_t)i));
        }
    }
    return nullptr;
}

static LLVMValueRef
variable_from_inputs(lower_state_t *state, th_type *variable)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    size_t native = 0;
    for (size_t i = 0; i < th_func_nparams(state->function->type); i++) {
        const th_param *formal = th_func_param(state->function->type, i);
        if (!formal || ir_func_parameter_is_optional(state->function, i))
            continue;
        LLVMValueRef input = LLVMGetParam(state->target_function,
                                          (unsigned)native++);
        if (!th_mode_accepts_input(formal->mode)
            || !type_contains_variable(formal->type, variable)
            || value_type(state->function->owner, formal->type, context)
                != box) continue;
        if (th_mode_produces_output(formal->mode))
            input = LLVMBuildLoad2(state->builder, box, input,
                                   "generic.input.box");
        LLVMValueRef actual = LLVMBuildExtractValue(state->builder, input, 2,
                                                     "generic.input.type");
        return variable_from_pattern(state, formal->type, variable, actual);
    }
    return nullptr;
}

static LLVMValueRef
runtime_type_value(lower_state_t *state, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    type = th_resolve(type);
    if (th_is_closed(ir_module_types(state->function->owner), type)) {
        uint32_t index = runtime_type_index(state, type);
        return index ? LLVMConstInt(i32, index, false) : nullptr;
    }
    if (th_kind_of(type) == TH_VAR)
        return variable_from_inputs(state, type);
    if (th_kind_of(type) == TH_SEQUENCE) {
        LLVMValueRef element = runtime_type_value(state, th_seq_elem(type));
        LLVMTypeRef parameters[] = {i32};
        LLVMTypeRef signature = LLVMFunctionType(i32, parameters, 1, false);
        LLVMValueRef helper = LLVMGetNamedFunction(state->module,
            "n00bb_bootstrap_array_type");
        if (!helper) helper = LLVMAddFunction(state->module,
            "n00bb_bootstrap_array_type", signature);
        if (!element || !helper || LLVMGlobalGetValueType(helper) != signature)
            return nullptr;
        return LLVMBuildCall2(state->builder, signature, helper,
            &element, 1, "generic.array.type");
    }
    if (th_kind_of(type) == TH_INDIRECTION) {
        LLVMValueRef element = runtime_type_value(state,
            th_ind_referent(type));
        LLVMTypeRef parameters[] = {i32};
        LLVMTypeRef signature = LLVMFunctionType(i32, parameters, 1, false);
        LLVMValueRef helper = LLVMGetNamedFunction(state->module,
            "n00bb_bootstrap_reference_type");
        if (!helper) helper = LLVMAddFunction(state->module,
            "n00bb_bootstrap_reference_type", signature);
        if (!element || !helper || LLVMGlobalGetValueType(helper) != signature)
            return nullptr;
        return LLVMBuildCall2(state->builder, signature, helper,
            &element, 1, "generic.reference.type");
    }
    const char *name = nullptr;
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || !count || count > UINT32_MAX) return nullptr;
    LLVMTypeRef array = LLVMArrayType(i32, (unsigned)count);
    LLVMValueRef values = entry_alloca(state, array);
    if (!values) return nullptr;
    for (size_t i = 0; i < count; i++) {
        const th_arg *argument = th_named_arg(type, i);
        LLVMValueRef child = argument && argument->kind == TH_ARG_TYPE
            ? runtime_type_value(state, argument->type) : nullptr;
        if (!child) return nullptr;
        LLVMValueRef indexes[] = {LLVMConstNull(i32),
            LLVMConstInt(i32, i, false)};
        LLVMValueRef slot = LLVMBuildInBoundsGEP2(state->builder, array,
            values, indexes, 2, "generic.argument.slot");
        LLVMBuildStore(state->builder, child, slot);
    }
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer, pointer, i32};
    LLVMTypeRef signature = LLVMFunctionType(i32, parameters, 3, false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_named_type");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_named_type", signature);
    if (!helper || LLVMGlobalGetValueType(helper) != signature)
        return nullptr;
    LLVMValueRef args[] = {LLVMBuildGlobalStringPtr(state->builder, name,
        "generic.constructor"), values, LLVMConstInt(i32, count, false)};
    return LLVMBuildCall2(state->builder, signature, helper, args, 3,
                          "generic.named.type");
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
    if (LLVMTypeOf(value) == box_llvm_type(context)) {
        LLVMTypeRef parameters[] = {
            LLVMInt32TypeInContext(context),
            LLVMInt64TypeInContext(context),
        };
        LLVMTypeRef signature = LLVMFunctionType(
            LLVMVoidTypeInContext(context), parameters, 2, false);
        LLVMValueRef barrier = LLVMGetNamedFunction(state->module,
            "n00bb_bootstrap_barrier_box");
        if (!barrier) barrier = LLVMAddFunction(state->module,
            "n00bb_bootstrap_barrier_box", signature);
        if (!barrier || LLVMGlobalGetValueType(barrier) != signature)
            return false;
        LLVMValueRef arguments[] = {
            LLVMBuildExtractValue(state->builder, value, 2, "box.type"),
            LLVMBuildExtractValue(state->builder, value, 3, "box.payload"),
        };
        return LLVMBuildCall2(state->builder, signature, barrier,
                              arguments, 2, "") != nullptr;
    }
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    if (LLVMTypeOf(value) == pointer
        && (th_kind_of(th_resolve(type)) == TH_INDIRECTION
            || n00bb_frame_pointer_option(type))) {
        LLVMTypeRef params[] = {pointer};
        LLVMTypeRef signature = LLVMFunctionType(
            LLVMVoidTypeInContext(context), params, 1, false);
        LLVMValueRef barrier = LLVMGetNamedFunction(state->module,
            "sys.internal.gc.alloc.barrier_pointer_runtime");
        if (!barrier) barrier = LLVMAddFunction(state->module,
            "sys.internal.gc.alloc.barrier_pointer_runtime", signature);
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
        "sys.internal.gc.alloc.barrier_runtime");
    if (!barrier) barrier = LLVMAddFunction(state->module,
        "sys.internal.gc.alloc.barrier_runtime", signature);
    LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(value));
    if (!slot) return false;
    LLVMBuildStore(state->builder, value, slot);
    LLVMValueRef args[] = {slot, LLVMConstInt(i32, index, false)};
    return LLVMBuildCall2(state->builder, signature, barrier, args, 2,
                          "") != nullptr;
}

static LLVMValueRef
gc_references_equal(lower_state_t *state, LLVMValueRef old_value,
                    LLVMValueRef new_value, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = LLVMTypeOf(new_value);
    if (native != LLVMTypeOf(old_value)) return nullptr;
    if (LLVMGetTypeKind(native) == LLVMPointerTypeKind
        && (th_kind_of(th_resolve(type)) == TH_INDIRECTION
            || n00bb_frame_pointer_option(type)))
        return LLVMBuildICmp(state->builder, LLVMIntEQ, old_value,
                             new_value, "gc.same.pointer");
    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE
        && LLVMGetTypeKind(native) == LLVMStructTypeKind
        && LLVMCountStructElementTypes(native) == 2) {
        LLVMValueRef same = LLVMConstInt(LLVMInt1TypeInContext(context),
                                         1, false);
        for (unsigned i = 0; i < 2; i++) {
            LLVMValueRef old_field = LLVMBuildExtractValue(state->builder,
                old_value, i, "gc.old.array.pointer");
            LLVMValueRef new_field = LLVMBuildExtractValue(state->builder,
                new_value, i, "gc.new.array.pointer");
            if (!old_field || !new_field
                || LLVMGetTypeKind(LLVMTypeOf(old_field))
                    != LLVMPointerTypeKind) return nullptr;
            same = LLVMBuildAnd(state->builder, same,
                LLVMBuildICmp(state->builder, LLVMIntEQ, old_field,
                    new_field, "gc.same.array.pointer"), "gc.same.array");
        }
        return same;
    }
    th_type *definition = ir_type_definition(state->function->owner, type);
    if (!definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    bool variant = false;
    th_named_info(th_resolve(type), nullptr, nullptr, &variant, nullptr);
    if (variant || !definition || th_kind_of(definition) != TH_RECORD
        || LLVMGetTypeKind(native) != LLVMStructTypeKind) return nullptr;
    size_t count = th_record_len(definition);
    const ir_final_layout_t *layout = ir_layout_final(
        state->function->owner, type,
        ir_module_target(state->function->owner));
    if (!layout || count != layout->n_members
        || count != LLVMCountStructElementTypes(native)) return nullptr;
    size_t *order = count ? calloc(count, sizeof(*order)) : nullptr;
    if (count && !order) return nullptr;
    record_layout_order(layout, order);
    LLVMValueRef same = LLVMConstInt(LLVMInt1TypeInContext(context), 1,
                                     false);
    for (size_t i = 0; i < count; i++) {
        const ir_final_member_layout_t *member = &layout->members[order[i]];
        th_type *field = member->name && !member->inline_element_count
            ? th_record_lookup(definition, member->name) : nullptr;
        if (!field) { same = nullptr; break; }
        ir_class_t category = ir_machine_of(state->function->owner,
                                             field).class;
        if (category == IR_CLASS_VOID || category == IR_CLASS_BOOL
            || category == IR_CLASS_SINT || category == IR_CLASS_UINT
            || category == IR_CLASS_FLOAT) continue;
        LLVMValueRef old_field = LLVMBuildExtractValue(state->builder,
            old_value, (unsigned)i, "gc.old.field");
        LLVMValueRef new_field = LLVMBuildExtractValue(state->builder,
            new_value, (unsigned)i, "gc.new.field");
        LLVMValueRef field_same = old_field && new_field
            ? gc_references_equal(state, old_field, new_field, field)
            : nullptr;
        if (!field_same) { same = nullptr; break; }
        same = LLVMBuildAnd(state->builder, same, field_same,
                            "gc.same.references");
    }
    free(order);
    return same;
}

static bool
store_gc_barrier_if_changed(lower_state_t *state, LLVMValueRef address,
                            LLVMValueRef value, th_type *type)
{
    LLVMValueRef prior = LLVMBuildLoad2(state->builder, LLVMTypeOf(value),
                                        address, "gc.prior.value");
    LLVMValueRef same = gc_references_equal(state, prior, value, type);
    if (!same) return store_gc_barrier(state, value, type);
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef function = LLVMGetBasicBlockParent(
        LLVMGetInsertBlock(state->builder));
    LLVMBasicBlockRef barrier = LLVMAppendBasicBlockInContext(context,
        function, "gc.changed.references");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        function, "gc.store.continue");
    LLVMBuildCondBr(state->builder, same, done, barrier);
    LLVMPositionBuilderAtEnd(state->builder, barrier);
    if (!store_gc_barrier(state, value, type)) return false;
    LLVMBuildBr(state->builder, done);
    LLVMPositionBuilderAtEnd(state->builder, done);
    return true;
}

static LLVMValueRef
assemble_box(lower_state_t *state, uint32_t index, LLVMValueRef bits)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef box = LLVMConstNull(box_llvm_type(context));
    box = LLVMBuildInsertValue(state->builder, box,
        LLVMConstInt(LLVMInt32TypeInContext(context), index, false),
        2, "box.type");
    return LLVMBuildInsertValue(state->builder, box, bits, 3, "box.value");
}

static LLVMValueRef
box_value(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box_type = box_llvm_type(context);
    LLVMTypeRef source_type = LLVMTypeOf(value);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!box_type) return nullptr;
    if (source_type == box_type) return value;
    uint32_t index = runtime_type_index(state, type);
    if (!layout || !index || index >= (1u << 31)
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
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef parameters[] = {i32, i32, word_type, i32, pointer};
        LLVMTypeRef signature = LLVMFunctionType(pointer, parameters,
                                                  5, false);
        const char *allocator_name = ir_func_execution_phase(state->function)
            == IR_PHASE_COMPTIME ? "n00b_comptime_allocate"
                                 : "n00b_allocate";
        LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                      allocator_name);
        if (!allocator)
            allocator = LLVMAddFunction(state->module, allocator_name,
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
    return assemble_box(state, index, bits);
}

static LLVMValueRef
box_fresh_storage(lower_state_t *state, LLVMValueRef storage, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef concrete = value_type(state->function->owner, type, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    uint32_t index = runtime_type_index(state, type);
    if (!storage || !concrete || !layout || !index
        || index >= (1u << 31)) return nullptr;
    LLVMTypeRef word = LLVMInt64TypeInContext(context);
    LLVMValueRef bits = LLVMABISizeOfType(layout, concrete) <= sizeof(uint64_t)
        ? LLVMConstNull(word)
        : LLVMBuildPtrToInt(state->builder, storage, word,
                            "fresh.box.pointer");
    return assemble_box(state, index, bits);
}

static LLVMValueRef
unbox_value(lower_state_t *state, LLVMValueRef box, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef target = value_type(state->function->owner, type, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!target || !layout || LLVMGetTypeKind(target) == LLVMVoidTypeKind)
        return nullptr;
    if (target == box_llvm_type(context)) return box;
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
dynamic_box_load(lower_state_t *state, LLVMValueRef address,
                 LLVMValueRef type_index)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer, LLVMInt32TypeInContext(context),
                                pointer};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                             parameters, 3, false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_load_box");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_load_box", signature);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMValueRef result_slot = entry_alloca(state, box);
    if (!address || !type_index || !helper || !result_slot
        || LLVMGlobalGetValueType(helper) != signature) return nullptr;
    LLVMValueRef arguments[] = {address, type_index, result_slot};
    LLVMBuildCall2(state->builder, signature, helper, arguments, 3, "");
    return LLVMBuildLoad2(state->builder, box, result_slot,
                          "generic.loaded");
}

static bool
value_has_one_immediate_use(const ir_func_t *function,
                            const ir_instr_t *producer, int operation)
{
    if (!function || !producer || !producer->dest || !producer->block
        || producer->index + 1 >= producer->block->n_instrs) return false;
    const ir_instr_t *next = producer->block->instrs[producer->index + 1];
    if (!next || next->op != operation || !next->n_operands
        || ir_operand_value(&next->operands[0]) != producer->dest) return false;
    size_t uses = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; block && i < block->n_instrs; i++) {
            const ir_instr_t *instruction = block->instrs[i];
            for (uint32_t j = 0; instruction && j < instruction->n_operands;
                 j++) {
                if (ir_operand_value(&instruction->operands[j])
                    != producer->dest) continue;
                if (instruction != next || j != 0 || ++uses != 1)
                    return false;
            }
        }
    }
    return uses == 1;
}

static LLVMValueRef
copy_boxed_array(lower_state_t *state, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer, allocator_params,
                                                   5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                  allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
                                                allocator_name,
                                                allocator_type);
    LLVMTypeRef helper_params[] = {pointer, pointer, pointer};
    LLVMTypeRef helper_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context), helper_params, 3, false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_copy_array_box");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_copy_array_box", helper_type);
    LLVMValueRef source = entry_alloca(state, box);
    LLVMValueRef result = entry_alloca(state, box);
    if (!value || LLVMTypeOf(value) != box || !allocator || !helper
        || !source || !result
        || LLVMGlobalGetValueType(allocator) != allocator_type
        || LLVMGlobalGetValueType(helper) != helper_type) return nullptr;
    LLVMBuildStore(state->builder, value, source);
    LLVMValueRef args[] = {source, allocator, result};
    LLVMBuildCall2(state->builder, helper_type, helper, args, 3, "");
    return LLVMBuildLoad2(state->builder, box, result,
                          "generic.array.copy");
}

static LLVMValueRef
dynamic_field_box(lower_state_t *state, LLVMValueRef box_address,
                  const char *name, uint32_t ordinal, bool read_value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
        allocator_params, 5, false);
    const char *allocator_name = ir_func_execution_phase(state->function)
        == IR_PHASE_COMPTIME ? "n00b_comptime_allocate" : "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, allocator_type);
    LLVMTypeRef parameters[] = {pointer,
        ordinal == UINT32_MAX ? pointer : i32, pointer, pointer};
    LLVMTypeRef signature = LLVMFunctionType(
        LLVMVoidTypeInContext(context), parameters, 4, false);
    const char *helper_name = ordinal == UINT32_MAX
        ? (read_value ? "n00bb_bootstrap_field_read_box"
                      : "n00bb_bootstrap_field_ref_box")
        : (read_value ? "n00bb_bootstrap_field_read_box_index"
                      : "n00bb_bootstrap_field_ref_box_index");
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        helper_name);
    if (!helper) helper = LLVMAddFunction(state->module,
        helper_name, signature);
    LLVMValueRef field_key = ordinal == UINT32_MAX
        ? name ? LLVMBuildGlobalStringPtr(state->builder, name,
                                          "generic.field.name") : nullptr
        : LLVMConstInt(i32, ordinal, false);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMValueRef result_slot = entry_alloca(state, box);
    if (box_address && LLVMTypeOf(box_address) == box) {
        LLVMValueRef slot = entry_alloca(state, box);
        if (!slot) return nullptr;
        LLVMBuildStore(state->builder, box_address, slot);
        box_address = slot;
    }
    if (!box_address || !allocator || !helper || !field_key || !result_slot
        || LLVMGlobalGetValueType(allocator) != allocator_type
        || LLVMGlobalGetValueType(helper) != signature) return nullptr;
    LLVMValueRef arguments[] = {box_address, field_key, allocator,
        result_slot};
    LLVMBuildCall2(state->builder, signature, helper, arguments, 4, "");
    return LLVMBuildLoad2(state->builder, box, result_slot,
        read_value ? "generic.field.read" : "generic.field.reference");
}

static LLVMValueRef
boxed_reference_address(lower_state_t *state, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    if (!value || LLVMTypeOf(value) != box) return nullptr;
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer};
    LLVMTypeRef signature = LLVMFunctionType(pointer, parameters, 1,
                                             false);
    LLVMValueRef helper = LLVMGetNamedFunction(state->module,
        "n00bb_bootstrap_ref_address_box");
    if (!helper) helper = LLVMAddFunction(state->module,
        "n00bb_bootstrap_ref_address_box", signature);
    LLVMValueRef slot = entry_alloca(state, box);
    if (!helper || !slot || LLVMGlobalGetValueType(helper) != signature)
        return nullptr;
    LLVMBuildStore(state->builder, value, slot);
    return LLVMBuildCall2(state->builder, signature, helper, &slot, 1,
                          "generic.reference.address");
}

static th_type *
option_payload_type(th_type *type)
{
    const char *name = nullptr;
    size_t count = 0;
    type = th_resolve(type);
    if (!th_named_info(type, &name, nullptr, nullptr, &count)
        || !name || strcmp(name, "option") || count != 1) return nullptr;
    const th_arg *argument = th_named_arg(type, 0);
    return argument && argument->kind == TH_ARG_TYPE
        ? argument->type : nullptr;
}

static LLVMValueRef
packed_option_mask(LLVMTypeRef type, uint64_t bit)
{
    if (!type || LLVMGetTypeKind(type) != LLVMIntegerTypeKind
        || bit >= LLVMGetIntTypeWidth(type)) return nullptr;
    unsigned count = (LLVMGetIntTypeWidth(type) + 63) / 64;
    uint64_t *words = calloc(count, sizeof(*words));
    if (!words) return nullptr;
    words[bit / 64] = UINT64_C(1) << (bit % 64);
    LLVMValueRef mask = LLVMConstIntOfArbitraryPrecision(type, count, words);
    free(words);
    return mask;
}

static LLVMTypeRef
packed_option_bits_type(LLVMTypeRef storage, LLVMContextRef context)
{
    if (!storage) return nullptr;
    if (LLVMGetTypeKind(storage) == LLVMIntegerTypeKind) return storage;
    if (LLVMGetTypeKind(storage) != LLVMArrayTypeKind
        || LLVMGetTypeKind(LLVMGetElementType(storage)) != LLVMIntegerTypeKind
        || LLVMGetIntTypeWidth(LLVMGetElementType(storage)) != 64)
        return nullptr;
    unsigned count = LLVMGetArrayLength(storage);
    return count && count <= UINT_MAX / 64
        ? LLVMIntTypeInContext(context, count * 64) : nullptr;
}

static LLVMValueRef
packed_option_reinterpret(lower_state_t *state, LLVMValueRef value,
                          LLVMTypeRef target)
{
    if (!value || !target) return nullptr;
    if (LLVMTypeOf(value) == target) return value;
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    LLVMTypeRef source = LLVMTypeOf(value);
    LLVMValueRef slot = layout
        && LLVMStoreSizeOfType(layout, source)
            == LLVMStoreSizeOfType(layout, target)
        ? entry_alloca(state, source) : nullptr;
    if (!slot) return nullptr;
    LLVMValueRef store = LLVMBuildStore(state->builder, value, slot);
    if (!store) return nullptr;
    LLVMSetAlignment(store, 1);
    LLVMValueRef loaded = LLVMBuildLoad2(state->builder, target, slot,
                                         "option.reinterpret");
    if (loaded) LLVMSetAlignment(loaded, 1);
    return loaded;
}

static LLVMValueRef
packed_option_presence(lower_state_t *state, LLVMValueRef value,
                       uint64_t bit)
{
    LLVMTypeRef bits = packed_option_bits_type(LLVMTypeOf(value),
        LLVMGetModuleContext(state->module));
    LLVMValueRef integer = bits
        ? packed_option_reinterpret(state, value, bits) : nullptr;
    LLVMValueRef mask = packed_option_mask(bits, bit);
    return integer && mask
        ? LLVMBuildAnd(state->builder, integer, mask, "option.presence")
        : nullptr;
}

static LLVMValueRef
pack_option_integer(lower_state_t *state, LLVMValueRef payload,
                    th_type *option_type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    uint64_t bit = 0;
    LLVMTypeRef target = packed_option_type(state->function->owner,
        option_type, context, &bit);
    LLVMTypeRef bits = packed_option_bits_type(target, context);
    LLVMValueRef mask = packed_option_mask(bits, bit);
    if (!payload || !mask) return nullptr;
    LLVMTypeRef source = LLVMTypeOf(payload);
    if (LLVMGetTypeKind(source) == LLVMArrayTypeKind
        || LLVMGetTypeKind(source) == LLVMStructTypeKind) {
        payload = packed_option_reinterpret(state, payload, bits);
        if (!payload) return nullptr;
    }
    if (LLVMGetTypeKind(LLVMTypeOf(payload)) != LLVMIntegerTypeKind)
        return nullptr;
    unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(payload));
    unsigned target_bits = LLVMGetIntTypeWidth(bits);
    if (source_bits > target_bits) return nullptr;
    LLVMValueRef expanded = source_bits == target_bits ? payload
        : LLVMBuildZExt(state->builder, payload, bits, "option.payload.bits");
    LLVMValueRef clear = LLVMBuildAnd(state->builder, expanded,
        LLVMConstNot(mask), "option.payload.clear.tag");
    LLVMValueRef result = LLVMBuildOr(state->builder, clear, mask,
                                      "option.some.bits");
    return packed_option_reinterpret(state, result, target);
}

static LLVMValueRef
unpack_option_integer(lower_state_t *state, LLVMValueRef value,
                      th_type *option_type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    uint64_t bit = 0;
    LLVMTypeRef packed = packed_option_type(state->function->owner,
        option_type, context, &bit);
    LLVMTypeRef bits = packed_option_bits_type(packed, context);
    LLVMValueRef mask = packed_option_mask(bits, bit);
    th_type *child = option_payload_type(option_type);
    LLVMTypeRef target = child ? value_type(state->function->owner,
                                           child, context) : nullptr;
    if (!value || !mask || LLVMTypeOf(value) != packed || !target)
        return nullptr;
    LLVMValueRef integer = packed_option_reinterpret(state, value, bits);
    if (!integer) return nullptr;
    LLVMValueRef clear = LLVMBuildAnd(state->builder, integer,
        LLVMConstNot(mask), "option.payload.bits");
    if (LLVMGetTypeKind(target) == LLVMArrayTypeKind
        || LLVMGetTypeKind(target) == LLVMStructTypeKind) {
        return packed_option_reinterpret(state, clear, target);
    }
    if (LLVMGetTypeKind(target) != LLVMIntegerTypeKind) return nullptr;
    unsigned target_bits = LLVMGetIntTypeWidth(target);
    if (target_bits > LLVMGetIntTypeWidth(bits)) return nullptr;
    th_type *definition = ir_type_definition(state->function->owner, child);
    uint64_t semantic_bits = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    bool signed_narrow = definition
        && th_bitint_info(definition, &semantic_bits, &flavor)
        && flavor == TH_BITINT_SIGNED && semantic_bits
        && semantic_bits < target_bits;
    if (signed_narrow) {
        LLVMTypeRef narrow = LLVMIntTypeInContext(context,
                                                  (unsigned)semantic_bits);
        LLVMValueRef clipped = LLVMBuildTrunc(state->builder, clear, narrow,
                                              "option.payload.narrow");
        return LLVMBuildSExt(state->builder, clipped, target,
                            "option.payload.signed");
    }
    return target == bits ? clear
        : LLVMBuildTrunc(state->builder, clear, target, "option.payload");
}

static LLVMValueRef
adapt_call_value(lower_state_t *state, LLVMValueRef value,
                 th_type *source_type, th_type *target_type)
{
    if (!value || !source_type || !target_type) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef target = value_type(state->function->owner, target_type,
                                   context);
    if (!target) return nullptr;
    if (LLVMTypeOf(value) == target) return value;
    th_type *source_payload = option_payload_type(source_type);
    th_type *target_payload = option_payload_type(target_type);
    LLVMTypeRef source = LLVMTypeOf(value);
    uint64_t packed_bit = 0;
    LLVMTypeRef packed_target = target_payload
        ? packed_option_type(state->function->owner, target_type,
                             context, &packed_bit) : nullptr;
    if (source_payload && target_payload && packed_target
        && LLVMGetTypeKind(source) == LLVMStructTypeKind
        && LLVMCountStructElementTypes(source) == 2) {
        uint8_t source_mask = 1;
        bool source_shared = shared_option_value_type(
            state->function->owner, source_type, context,
            &source_mask) != nullptr;
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, value, 1,
                                                  "option.call.tag");
        LLVMValueRef present = LLVMBuildICmp(state->builder, LLVMIntNE,
            LLVMBuildAnd(state->builder, tag,
                LLVMConstInt(LLVMTypeOf(tag), source_mask, false),
                "option.call.presence"),
            LLVMConstNull(LLVMTypeOf(tag)), "option.call.present");
        LLVMBasicBlockRef absent = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.absent");
        LLVMBasicBlockRef selected = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.present");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.done");
        LLVMBuildCondBr(state->builder, present, selected, absent);
        LLVMPositionBuilderAtEnd(state->builder, absent);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, selected);
        LLVMValueRef payload = nullptr;
        if (source_shared) {
            LLVMValueRef inner_tag = LLVMBuildAnd(state->builder, tag,
                LLVMConstInt(LLVMTypeOf(tag), ~source_mask & 0xffu, false),
                "option.call.inner.tag");
            payload = LLVMBuildInsertValue(state->builder, value, inner_tag,
                1, "option.call.inner");
        } else payload = LLVMBuildExtractValue(state->builder, value, 0,
                                                "option.call.payload");
        LLVMValueRef converted = adapt_call_value(state, payload,
            source_payload, target_payload);
        LLVMValueRef some = converted
            ? pack_option_integer(state, converted, target_type) : nullptr;
        if (!some) return nullptr;
        LLVMBasicBlockRef some_end = LLVMGetInsertBlock(state->builder);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, done);
        LLVMValueRef result = LLVMBuildPhi(state->builder, packed_target,
                                           "option.call.result");
        LLVMValueRef incoming[] = {LLVMConstNull(packed_target), some};
        LLVMBasicBlockRef blocks[] = {absent, some_end};
        LLVMAddIncoming(result, incoming, blocks, 2);
        return result;
    }
    uint64_t source_bit = 0;
    LLVMTypeRef packed_source = source_payload
        ? packed_option_type(state->function->owner, source_type,
                             context, &source_bit) : nullptr;
    if (source_payload && target_payload && packed_source == source
        && LLVMGetTypeKind(target) == LLVMStructTypeKind
        && LLVMCountStructElementTypes(target) == 2) {
        LLVMValueRef presence = packed_option_presence(state, value,
                                                        source_bit);
        if (!presence) return nullptr;
        LLVMValueRef present = LLVMBuildICmp(state->builder, LLVMIntNE,
            presence, LLVMConstNull(LLVMTypeOf(presence)),
            "option.call.present");
        LLVMBasicBlockRef absent = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.absent");
        LLVMBasicBlockRef selected = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.present");
        LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.done");
        LLVMBuildCondBr(state->builder, present, selected, absent);
        LLVMPositionBuilderAtEnd(state->builder, absent);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, selected);
        LLVMValueRef payload = unpack_option_integer(state, value,
                                                     source_type);
        LLVMValueRef converted = payload ? adapt_call_value(state, payload,
            source_payload, target_payload) : nullptr;
        uint8_t target_mask = 1;
        bool target_shared = shared_option_value_type(
            state->function->owner, target_type, context,
            &target_mask) != nullptr;
        LLVMValueRef some = nullptr;
        if (converted && target_shared && LLVMTypeOf(converted) == target) {
            LLVMValueRef tag = LLVMBuildExtractValue(state->builder,
                converted, 1, "option.call.inner.tag");
            tag = LLVMBuildOr(state->builder, tag,
                LLVMConstInt(LLVMTypeOf(tag), target_mask, false),
                "option.call.outer.tag");
            some = LLVMBuildInsertValue(state->builder, converted, tag, 1,
                                        "option.call.some");
        } else if (converted && !target_shared
            && LLVMTypeOf(converted)
                == LLVMStructGetTypeAtIndex(target, 0)) {
            some = LLVMBuildInsertValue(state->builder,
                LLVMConstNull(target), converted, 0, "option.call.value");
            some = LLVMBuildInsertValue(state->builder, some,
                LLVMConstInt(LLVMStructGetTypeAtIndex(target, 1), 1, false),
                1, "option.call.some");
        }
        if (!some) return nullptr;
        LLVMBasicBlockRef some_end = LLVMGetInsertBlock(state->builder);
        LLVMBuildBr(state->builder, done);
        LLVMPositionBuilderAtEnd(state->builder, done);
        LLVMValueRef result = LLVMBuildPhi(state->builder, target,
                                           "option.call.result");
        LLVMValueRef incoming[] = {LLVMConstNull(target), some};
        LLVMBasicBlockRef blocks[] = {absent, some_end};
        LLVMAddIncoming(result, incoming, blocks, 2);
        return result;
    }
    if (source_payload && target_payload
        && LLVMGetTypeKind(source) == LLVMStructTypeKind
        && LLVMGetTypeKind(target) == LLVMStructTypeKind
        && LLVMCountStructElementTypes(source) == 2
        && LLVMCountStructElementTypes(target) == 2
        && LLVMStructGetTypeAtIndex(source, 1)
            == LLVMStructGetTypeAtIndex(target, 1)) {
        uint8_t source_mask = 1, target_mask = 1;
        bool source_shared = shared_option_value_type(
            state->function->owner, source_type, context,
            &source_mask) != nullptr;
        bool target_shared = shared_option_value_type(
            state->function->owner, target_type, context,
            &target_mask) != nullptr;
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, value, 1,
            "option.call.tag");
        LLVMValueRef source_presence = LLVMBuildAnd(state->builder, tag,
            LLVMConstInt(LLVMTypeOf(tag), source_mask, false),
            "option.call.presence");
        LLVMValueRef present = LLVMBuildICmp(state->builder, LLVMIntNE,
            source_presence, LLVMConstNull(LLVMTypeOf(tag)),
            "option.call.present");
        LLVMBasicBlockRef absent_block = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.absent");
        LLVMBasicBlockRef present_block = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.present");
        LLVMBasicBlockRef done_block = LLVMAppendBasicBlockInContext(context,
            state->target_function, "option.call.done");
        LLVMBuildCondBr(state->builder, present, present_block, absent_block);
        LLVMPositionBuilderAtEnd(state->builder, absent_block);
        LLVMBuildBr(state->builder, done_block);
        LLVMPositionBuilderAtEnd(state->builder, present_block);
        LLVMValueRef payload = nullptr;
        if (source_shared) {
            LLVMValueRef inner_tag = LLVMBuildAnd(state->builder, tag,
                LLVMConstInt(LLVMTypeOf(tag), ~source_mask & 0xffu, false),
                "option.call.inner.tag");
            payload = LLVMBuildInsertValue(state->builder, value, inner_tag,
                1, "option.call.inner");
        } else payload = LLVMBuildExtractValue(state->builder, value, 0,
                                               "option.call.payload");
        LLVMValueRef converted = adapt_call_value(state, payload,
            source_payload, target_payload);
        if (!converted) return nullptr;
        LLVMValueRef some = nullptr;
        if (target_shared) {
            if (LLVMTypeOf(converted) != target) return nullptr;
            LLVMValueRef inner_tag = LLVMBuildExtractValue(state->builder,
                converted, 1, "option.call.inner.tag");
            LLVMValueRef outer_tag = LLVMBuildOr(state->builder, inner_tag,
                LLVMConstInt(LLVMTypeOf(inner_tag), target_mask, false),
                "option.call.outer.tag");
            some = LLVMBuildInsertValue(state->builder, converted, outer_tag,
                1, "option.call.some");
        } else {
            if (LLVMTypeOf(converted)
                != LLVMStructGetTypeAtIndex(target, 0)) return nullptr;
            some = LLVMBuildInsertValue(state->builder,
                LLVMConstNull(target), converted, 0, "option.call.value");
            some = LLVMBuildInsertValue(state->builder, some,
                LLVMConstInt(LLVMTypeOf(tag), target_mask, false), 1,
                "option.call.some");
        }
        LLVMBasicBlockRef some_end = LLVMGetInsertBlock(state->builder);
        LLVMBuildBr(state->builder, done_block);
        LLVMPositionBuilderAtEnd(state->builder, done_block);
        LLVMValueRef result = LLVMBuildPhi(state->builder, target,
            "option.call.result");
        LLVMValueRef incoming[] = {LLVMConstNull(target), some};
        LLVMBasicBlockRef blocks[] = {absent_block, some_end};
        LLVMAddIncoming(result, incoming, blocks, 2);
        return result;
    }
    LLVMTypeRef box = box_llvm_type(context);
    if (LLVMTypeOf(value) == box
        && LLVMGetTypeKind(target) == LLVMPointerTypeKind
        && th_kind_of(th_resolve(source_type)) == TH_INDIRECTION
        && th_kind_of(th_resolve(target_type)) == TH_INDIRECTION)
        return boxed_reference_address(state, value);
    if (LLVMTypeOf(value) == box) return unbox_value(state, value,
                                                     target_type);
    if (target == box) return box_value(state, value, source_type);
    return nullptr;
}

static LLVMValueRef
adapt_call_result(lower_state_t *state, LLVMValueRef value,
                  th_type *source_signature, th_type *target_signature)
{
    if (!value || !source_signature || !target_signature) return nullptr;
    ir_module_t *owner = state->function->owner;
    th_type *source_success = th_func_ret(source_signature);
    th_type *source_failure = th_func_error(source_signature);
    th_type *target_success = th_func_ret(target_signature);
    th_type *target_failure = th_func_error(target_signature);
    if (!source_failure) source_failure = th_name(ir_module_types(owner),
                                                  "nil");
    if (!target_failure) target_failure = th_name(ir_module_types(owner),
                                                  "nil");
    if (!source_success || !source_failure || !target_success
        || !target_failure) return nullptr;
    bool source_fails = ir_machine_of(owner, source_failure).class
        != IR_CLASS_VOID;
    bool target_fails = ir_machine_of(owner, target_failure).class
        != IR_CLASS_VOID;
    if (source_fails != target_fails) return nullptr;
    if (!source_fails) return adapt_call_value(state, value, source_success,
                                               target_success);

    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef source_result = native_outcome_type(state->module, owner,
        source_success, source_failure, context);
    LLVMTypeRef target_result = native_outcome_type(state->module, owner,
        target_success, target_failure, context);
    LLVMTypeRef source_ok = value_type(owner, source_success, context);
    LLVMTypeRef source_error = value_type(owner, source_failure, context);
    LLVMTypeRef target_ok = value_type(owner, target_success, context);
    LLVMTypeRef target_error = value_type(owner, target_failure, context);
    if (!source_result || !target_result || !source_ok || !source_error
        || !target_ok || !target_error
        || LLVMTypeOf(value) != source_result) return nullptr;
    bool void_success = LLVMGetTypeKind(source_ok) == LLVMVoidTypeKind;
    if (void_success != (LLVMGetTypeKind(target_ok) == LLVMVoidTypeKind))
        return nullptr;
    if (source_ok == target_ok && source_error == target_error)
        return value;

    LLVMValueRef source = entry_alloca(state, source_result);
    LLVMValueRef target = entry_alloca(state, target_result);
    if (!source || !target) return nullptr;
    LLVMBuildStore(state->builder, value, source);
    LLVMBuildStore(state->builder, LLVMConstNull(target_result), target);
    LLVMValueRef tag = LLVMBuildExtractValue(state->builder, value, 1,
                                             "call.result.tag");
    LLVMValueRef target_tag = LLVMBuildStructGEP2(state->builder,
        target_result, target, 1, "call.target.tag");
    LLVMBuildStore(state->builder, tag, target_tag);
    LLVMValueRef is_success = LLVMBuildICmp(state->builder, LLVMIntEQ,
        tag, LLVMConstNull(LLVMTypeOf(tag)), "call.result.ok");
    LLVMBasicBlockRef success = LLVMAppendBasicBlockInContext(context,
        state->target_function, "call.result.success");
    LLVMBasicBlockRef failure = LLVMAppendBasicBlockInContext(context,
        state->target_function, "call.result.failure");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        state->target_function, "call.result.done");
    LLVMBuildCondBr(state->builder, is_success, success, failure);

    LLVMPositionBuilderAtEnd(state->builder, success);
    LLVMValueRef source_payload = nullptr;
    LLVMValueRef source_value = nullptr;
    LLVMValueRef target_payload = nullptr;
    LLVMValueRef converted = nullptr;
    LLVMValueRef write = nullptr;
    if (!void_success) {
        source_payload = LLVMBuildStructGEP2(state->builder, source_result,
            source, 0, "call.source.ok");
        source_value = LLVMBuildLoad2(state->builder, source_ok,
            source_payload, "call.ok");
        LLVMSetAlignment(source_value, 1);
        converted = adapt_call_value(state, source_value,
            source_success, target_success);
        if (!converted) return nullptr;
        target_payload = LLVMBuildStructGEP2(state->builder, target_result,
            target, 0, "call.target.ok");
        write = LLVMBuildStore(state->builder, converted, target_payload);
        LLVMSetAlignment(write, 1);
    }
    LLVMBuildBr(state->builder, done);

    LLVMPositionBuilderAtEnd(state->builder, failure);
    source_payload = LLVMBuildStructGEP2(state->builder, source_result,
        source, 0, "call.source.error");
    source_value = LLVMBuildLoad2(state->builder, source_error,
        source_payload, "call.error");
    LLVMSetAlignment(source_value, 1);
    converted = adapt_call_value(state, source_value, source_failure,
        target_failure);
    if (!converted) return nullptr;
    target_payload = LLVMBuildStructGEP2(state->builder, target_result,
        target, 0, "call.target.error");
    write = LLVMBuildStore(state->builder, converted, target_payload);
    LLVMSetAlignment(write, 1);
    LLVMBuildBr(state->builder, done);

    LLVMPositionBuilderAtEnd(state->builder, done);
    return LLVMBuildLoad2(state->builder, target_result, target,
                          "call.result");
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

static LLVMValueRef
checked_array_index(lower_state_t *state, LLVMValueRef index,
                    LLVMValueRef limit, bool signed_index,
                    bool check_bounds)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (!index || !limit
        || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind
        || LLVMTypeOf(limit) != LLVMInt64TypeInContext(context))
        return nullptr;
    unsigned width = LLVMGetIntTypeWidth(LLVMTypeOf(index));
    LLVMTypeRef arithmetic = LLVMIntTypeInContext(context,
        width > 64 ? width : 64);
    LLVMValueRef position = LLVMBuildIntCast2(state->builder, index,
        arithmetic, signed_index, "index.extended");
    LLVMValueRef bound = arithmetic == LLVMTypeOf(limit) ? limit
        : LLVMBuildZExt(state->builder, limit, arithmetic,
            "array.length.extended");
    if (signed_index) {
        LLVMValueRef negative = LLVMBuildICmp(state->builder, LLVMIntSLT,
            position, LLVMConstNull(arithmetic), "index.negative");
        LLVMValueRef from_end = LLVMBuildAdd(state->builder, position,
            bound, "index.from.end");
        position = LLVMBuildSelect(state->builder, negative, from_end,
            position, "index.position");
    }
    if (check_bounds) {
        LLVMValueRef invalid = LLVMBuildICmp(state->builder, LLVMIntUGE,
            position, bound, "index.out.of.bounds");
        emit_trap_if(state, invalid);
    }
    return position;
}

static LLVMValueRef
native_array_slot(lower_state_t *state, LLVMValueRef array_address,
                  LLVMValueRef index, LLVMTypeRef array_type,
                  LLVMTypeRef element_type, bool signed_index)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMValueRef array = LLVMBuildLoad2(state->builder, array_type,
        array_address, "array.value");
    LLVMValueRef data = LLVMBuildExtractValue(state->builder, array, 0,
        "array.data");
    LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder, array, 1,
        "array.descriptor");
    LLVMValueRef length_address = LLVMBuildStructGEP2(state->builder,
        array_descriptor_type(context), descriptor, 5,
        "array.length.address");
    LLVMValueRef length = LLVMBuildLoad2(state->builder, i64,
        length_address, "array.length");
    LLVMValueRef position = checked_array_index(state, index, length,
        signed_index, true);
    if (!position) return nullptr;
    return LLVMBuildInBoundsGEP2(state->builder, element_type, data,
        &position, 1, "array.element.address");
}

/* Specialization can make a trait dispatch's type test constant. Exclude
 * impossible edges before lowering so their values never enter LLVM phis. */
static th_type *
type_test_source_type(lower_state_t *state, ir_value_t *value)
{
    th_ctx *types = ir_module_types(state->function->owner);
    for (size_t depth = 0; value && depth < 8; depth++) {
        if (th_is_closed(types, value->type)) return value->type;
        ir_instr_t *narrow = value->kind == IR_VAL_INSTR
            ? value->def : nullptr;
        if (!narrow || narrow->op != IR_OP_TYPE_NARROW
            || narrow->n_operands != 2
            || narrow->operands[0].kind != IR_OPD_VALUE
            || narrow->operands[1].kind != IR_OPD_TYPE)
            break;
        ir_value_t *source = narrow->operands[0].value;
        th_type *concrete = source ? source->type : nullptr;
        if (concrete && th_is_closed(types, concrete)
            && !th_env_type_pattern_accepts(types,
                narrow->operands[1].type, concrete)) break;
        value = source;
    }
    return value ? value->type : nullptr;
}

static int
static_type_test_result(lower_state_t *state, const ir_instr_t *test)
{
    if (!state || !test || test->op != IR_OP_TYPE_TEST
        || test->n_operands != 2
        || test->operands[0].kind != IR_OPD_VALUE
        || test->operands[1].kind != IR_OPD_TYPE
        || !test->operands[0].value) return -1;
    th_type *source = type_test_source_type(state,
                                           test->operands[0].value);
    th_type *pattern = test->operands[1].type;
    th_ctx *types = ir_module_types(state->function->owner);
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (!source || !pattern || !th_is_closed(types, source)
        || value_type(state->function->owner, source, context)
            == box_llvm_type(context)) return -1;
    if (!th_is_closed(types, pattern))
        return th_env_type_pattern_accepts(types, pattern, source);
    uint32_t source_index = runtime_type_index(state, source);
    uint32_t pattern_index = runtime_type_index(state, pattern);
    if (!source_index || !pattern_index) return -1;
    return source_index == pattern_index;
}

static int
static_type_test_branch(lower_state_t *state, const ir_instr_t *branch)
{
    if (!state || !branch || branch->op != IR_OP_BRANCH
        || branch->n_operands != 3
        || branch->operands[0].kind != IR_OPD_VALUE)
        return -1;
    ir_value_t *condition = branch->operands[0].value;
    ir_instr_t *test = condition && condition->kind == IR_VAL_INSTR
        ? condition->def : nullptr;
    return static_type_test_result(state, test);
}

static bool
feasible_block_order(lower_state_t *state, uint32_t *order, bool *feasible,
                     uint32_t *reachable_count)
{
    ir_func_t *function = state->function;
    uint32_t count = function->n_blocks;
    uint32_t *stack = calloc(count, sizeof(*stack));
    uint32_t *next = calloc(count, sizeof(*next));
    ir_block_t *entry = ir_block_entry(function);
    if (!stack || !next || !entry) {
        free(stack);
        free(next);
        return false;
    }
    uint32_t depth = 0, post = 0;
    stack[0] = entry->index;
    feasible[entry->index] = true;
    for (;;) {
        uint32_t index = stack[depth];
        ir_instr_t *term = ir_block_terminator(function->blocks[index]);
        int fixed_arm = static_type_test_branch(state, term);
        uint32_t successors = fixed_arm >= 0 ? 1 : ir_succ_count(term);
        if (next[index] < successors) {
            uint32_t arm = fixed_arm >= 0
                ? (fixed_arm ? 0 : 1) : next[index];
            next[index]++;
            ir_block_t *to = ir_succ_at(term, arm);
            if (!to || to->index >= count) {
                free(stack);
                free(next);
                return false;
            }
            if (!feasible[to->index]) {
                feasible[to->index] = true;
                stack[++depth] = to->index;
            }
            continue;
        }
        order[post++] = index;
        if (!depth) break;
        depth--;
    }
    for (uint32_t i = 0; i < post / 2; i++) {
        uint32_t other = order[i];
        order[i] = order[post - 1 - i];
        order[post - 1 - i] = other;
    }
    *reachable_count = post;
    for (uint32_t i = 0; i < count; i++)
        if (!feasible[i]) order[post++] = i;
    free(stack);
    free(next);
    return post == count;
}

static bool
feasible_edge(lower_state_t *state, ir_block_t *from, ir_block_t *to,
              const bool *feasible)
{
    if (!from || !to || !feasible[from->index]) return false;
    ir_instr_t *term = ir_block_terminator(from);
    int fixed_arm = static_type_test_branch(state, term);
    if (fixed_arm >= 0)
        return ir_succ_at(term, fixed_arm ? 0 : 1) == to;
    for (uint32_t i = 0; i < ir_succ_count(term); i++)
        if (ir_succ_at(term, i) == to) return true;
    return false;
}

static bool
lower_instruction(lower_state_t *state, ir_instr_t *instruction)
{
    LLVMValueRef result = nullptr;
    state->failure_detail = nullptr;
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
        if (!found || !record_type
            || native_count <= (unsigned)state->with_mutator
            || optional_index >= 64)
            break;
        LLVMValueRef record = LLVMGetParam(state->target_function,
            native_count - 1 - (unsigned)state->with_mutator);
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
            LLVMTypeRef float_type = machine.class == IR_CLASS_FLOAT
                ? scalar_type(state->function->owner, instruction->dest->type,
                              LLVMGetModuleContext(state->module))
                : nullptr;
            if (!float_type) break;
            result = LLVMConstReal(float_type, operands[0].lit.f);
            return save_result(state, instruction, result);
        }
        LLVMTypeRef type = scalar_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        result = integer_constant(machine, type, operands[0].lit.i);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    }
    case IR_OP_ENUM_CONST:
    case IR_OP_BITSET_CONST: {
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
        ir_annot_t *promoted_write = ir_annot_get(&instruction->annots,
            "ir.promoted_local_write");
        th_type *copy_type = instruction->dest_kind == IR_DEST_LOCAL
            && instruction->dest_local ? instruction->dest_local->type
            : promoted_write && promoted_write->kind == IR_ANNOT_BOOL
                && promoted_write->b && instruction->dest
                ? instruction->dest->type : nullptr;
        if (result && copy_type
            && th_kind_of(th_resolve(copy_type)) == TH_SEQUENCE
            && !sequence_value_transfers_storage(operands[0].value))
            result = copy_sequence_value(state, result, copy_type);
        return result && save_result(state, instruction, result);
    case IR_OP_CONSUME: {
        if ((count != 1 && count != 2)
            || operands[0].kind != IR_OPD_VALUE
            || (count == 2 && operands[1].kind != IR_OPD_VALUE)) break;
        result = lowered_value(state, operands[0].value);
        if (!result) break;
        if (count == 2 && operands[1].value
            && operands[1].value->kind == IR_VAL_LOCAL) {
            ir_local_t *local = operands[1].value->local;
            bool cleared = false;
            for (uint32_t i = 0; i < state->function->n_locals; i++) {
                if (state->function->locals[i] != local) continue;
                LLVMTypeRef type = value_type(state->function->owner,
                    local->type, LLVMGetModuleContext(state->module));
                if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind)
                    break;
                LLVMBuildStore(state->builder, LLVMConstNull(type),
                               state->locals[i]);
                cleared = true;
                break;
            }
            if (!cleared) break;
        }
        return save_result(state, instruction, result);
    }
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
                && LLVMGetTypeKind(type) != LLVMStructTypeKind
                && LLVMGetTypeKind(type) != LLVMArrayTypeKind
                && LLVMGetTypeKind(type) != LLVMIntegerTypeKind)) break;
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
            if (packed_option_type(state->function->owner,
                    instruction->dest->type,
                    LLVMGetModuleContext(state->module), nullptr)) {
                result = pack_option_integer(state, result,
                                             instruction->dest->type);
                return result && save_result(state, instruction, result);
            }
            uint8_t shared_mask = 0;
            LLVMTypeRef option = value_type(state->function->owner,
                instruction->dest->type,
                LLVMGetModuleContext(state->module));
            LLVMTypeRef shared = shared_option_value_type(
                state->function->owner, instruction->dest->type,
                LLVMGetModuleContext(state->module), &shared_mask);
            if (shared && option == LLVMTypeOf(result)) {
                LLVMValueRef tag = LLVMBuildExtractValue(state->builder,
                    result, 1, "option.inner.tag");
                tag = LLVMBuildOr(state->builder, tag,
                    LLVMConstInt(LLVMTypeOf(tag), shared_mask, false),
                    "option.outer.tag");
                result = LLVMBuildInsertValue(state->builder, result, tag,
                                              1, "option.some");
            }
            else if (option && LLVMGetTypeKind(option) == LLVMStructTypeKind
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
        else if (packed_option_type(state->function->owner,
                     operands[0].value->type,
                     LLVMGetModuleContext(state->module), nullptr)) {
            result = unpack_option_integer(state, result,
                                           operands[0].value->type);
        }
        else if (LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMStructTypeKind) {
            uint8_t shared_mask = 0;
            LLVMTypeRef shared = shared_option_value_type(
                state->function->owner, operands[0].value->type,
                LLVMGetModuleContext(state->module), &shared_mask);
            if (shared) {
                LLVMValueRef tag = LLVMBuildExtractValue(state->builder,
                    result, 1, "option.outer.tag");
                tag = LLVMBuildAnd(state->builder, tag,
                    LLVMConstInt(LLVMTypeOf(tag), ~shared_mask & 0xffu,
                                 false), "option.inner.tag");
                result = LLVMBuildInsertValue(state->builder, result, tag,
                                              1, "option.get");
            } else result = LLVMBuildExtractValue(state->builder, result, 0,
                                                  "option.get");
        }
        return save_result(state, instruction, result);
    case IR_OP_OPTION_IS_NIL: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value) break;
        LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(value));
        uint64_t packed_bit = 0;
        LLVMTypeRef packed = packed_option_type(state->function->owner,
            operands[0].value->type, LLVMGetModuleContext(state->module),
            &packed_bit);
        uint8_t shared_mask = 0;
        LLVMTypeRef shared = shared_option_value_type(
            state->function->owner, operands[0].value->type,
            LLVMGetModuleContext(state->module), &shared_mask);
        if (packed && LLVMTypeOf(value) == packed) {
            value = packed_option_presence(state, value, packed_bit);
            if (!value) break;
        } else if (kind == LLVMStructTypeKind) {
            value = LLVMBuildExtractValue(state->builder, value, 1,
                                          "option.tag");
            if (shared)
                value = LLVMBuildAnd(state->builder, value,
                    LLVMConstInt(LLVMTypeOf(value), shared_mask, false),
                    "option.outer.tag");
        } else if (kind != LLVMPointerTypeKind) break;
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
        uint64_t packed_bit = 0;
        LLVMTypeRef packed = packed_option_type(state->function->owner,
            operands[0].value->type, LLVMGetModuleContext(state->module),
            &packed_bit);
        if (packed && LLVMTypeOf(value) == packed) {
            tag = packed_option_presence(state, value, packed_bit);
            if (!tag) break;
            value = unpack_option_integer(state, value,
                                          operands[0].value->type);
            if (!value || LLVMTypeOf(value) != LLVMTypeOf(fallback)) break;
        } else if (kind == LLVMStructTypeKind) {
            uint8_t shared_mask = 0;
            LLVMTypeRef shared = shared_option_value_type(
                state->function->owner, operands[0].value->type,
                LLVMGetModuleContext(state->module), &shared_mask);
            if (!shared && LLVMStructGetTypeAtIndex(LLVMTypeOf(value), 0)
                    != LLVMTypeOf(fallback)) break;
            if (shared && LLVMTypeOf(value) != LLVMTypeOf(fallback)) break;
            tag = LLVMBuildExtractValue(state->builder, value, 1,
                                        "option.tag");
            if (shared) {
                LLVMValueRef inner_tag = LLVMBuildAnd(state->builder, tag,
                    LLVMConstInt(LLVMTypeOf(tag), ~shared_mask & 0xffu,
                                 false), "option.inner.tag");
                value = LLVMBuildInsertValue(state->builder, value,
                    inner_tag, 1, "option.payload");
                tag = LLVMBuildAnd(state->builder, tag,
                    LLVMConstInt(LLVMTypeOf(tag), shared_mask, false),
                    "option.outer.tag");
            } else value = LLVMBuildExtractValue(state->builder, value, 0,
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
        } else if (binding->kind == IR_VAL_PARAM
            && binding->owner == state->function
            && binding->index < state->function->n_params) {
            result = state->param_addresses[binding->index];
        } else if (binding->kind == IR_VAL_GLOBAL && binding->global) {
            result = global_ref(state->module, binding->global,
                state->global_bindings, state->global_binding_count);
        }
        if (result && instruction->dest
            && uses_bitptr(state->function->owner, instruction->dest->type)) {
            LLVMTypeRef i64 = LLVMInt64TypeInContext(
                LLVMGetModuleContext(state->module));
            result = make_packed_bitptr(state, result,
                LLVMConstNull(i64),
                th_ind_referent(instruction->dest->type));
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
        if (value_type(state->function->owner, record_type,
                LLVMGetModuleContext(state->module))
            == box_llvm_type(LLVMGetModuleContext(state->module))) {
            ir_module_t *owner = state->function->owner;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef target = value_type(owner,
                instruction->dest->type, context);
            LLVMTypeRef box = box_llvm_type(context);
            if ((LLVMTypeOf(address) == box
                    || LLVMGetTypeKind(LLVMTypeOf(address))
                        == LLVMPointerTypeKind)
                && th_kind_of(th_resolve(operands[0].value->type))
                    == TH_INDIRECTION
                && !uses_bitptr(owner, operands[0].value->type)
                && target) {
                const ir_final_member_layout_t *member =
                    known_byte_record_member(owner, record_type,
                        operands[1].name);
                bool native_reference = LLVMGetTypeKind(target)
                    == LLVMPointerTypeKind;
                bool boxed_reference = target == box
                    && th_kind_of(th_resolve(instruction->dest->type))
                        == TH_INDIRECTION
                    && !uses_bitptr(owner, instruction->dest->type);
                LLVMValueRef type_index = member && boxed_reference
                    ? runtime_type_value(state, instruction->dest->type)
                    : nullptr;
                if (member && (native_reference
                    || (boxed_reference && type_index))) {
                    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
                    LLVMValueRef boxed = LLVMTypeOf(address) == box
                        ? address : LLVMBuildLoad2(state->builder, box,
                            address, "boxed.record.ref");
                    LLVMValueRef bits = LLVMBuildExtractValue(state->builder,
                        boxed, 3, "boxed.record.address");
                    LLVMValueRef base = LLVMBuildIntToPtr(state->builder,
                        bits, LLVMPointerTypeInContext(context, 0),
                        "boxed.record.base");
                    LLVMValueRef offset = LLVMConstInt(
                        LLVMInt64TypeInContext(context),
                        member->bit_offset / 8, false);
                    result = LLVMBuildInBoundsGEP2(state->builder, i8,
                        base, &offset, 1, "boxed.field.address");
                    if (boxed_reference) {
                        LLVMValueRef pointer_bits = LLVMBuildPtrToInt(
                            state->builder, result,
                            LLVMInt64TypeInContext(context),
                            "boxed.field.pointer");
                        LLVMValueRef reference = LLVMConstNull(box);
                        reference = LLVMBuildInsertValue(state->builder,
                            reference, type_index, 2, "boxed.field.type");
                        result = LLVMBuildInsertValue(state->builder,
                            reference, pointer_bits, 3, "boxed.field.ref");
                    }
                    return save_result(state, instruction, result);
                }
            }
            LLVMValueRef boxed = dynamic_field_box(state, address,
                operands[1].name,
                record_member_ordinal(state->function->owner, record_type,
                                      operands[1].name), false);
            result = boxed && target == box_llvm_type(
                LLVMGetModuleContext(state->module)) ? boxed
                : boxed ? unbox_value(state, boxed,
                    instruction->dest->type) : nullptr;
            return result && save_result(state, instruction, result);
        }
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
        if (uses_bitptr(state->function->owner,
                        instruction->dest->type)) {
            result = make_packed_bitptr(state, result,
                LLVMConstNull(LLVMInt64TypeInContext(
                    LLVMGetModuleContext(state->module))),
                th_ind_referent(instruction->dest->type));
            if (!result) break;
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_SEQUENCE_SET: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || operands[2].kind != IR_OPD_VALUE) break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef box = box_llvm_type(context);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMValueRef base = lowered_value(state, operands[0].value);
        LLVMValueRef index = lowered_value(state, operands[1].value);
        LLVMValueRef value = lowered_value(state, operands[2].value);
        bool boxed_reference = base && LLVMTypeOf(base) == box
            && th_kind_of(th_resolve(operands[0].value->type))
                == TH_INDIRECTION
            && th_kind_of(th_resolve(th_ind_referent(
                operands[0].value->type))) == TH_SEQUENCE;
        th_type *referent = th_ind_referent(operands[0].value->type);
        LLVMTypeRef native_array = referent && base
            ? value_type(state->function->owner, referent, context) : nullptr;
        th_type *element = referent
            && th_kind_of(th_resolve(referent)) == TH_SEQUENCE
            ? th_seq_elem(th_resolve(referent)) : nullptr;
        LLVMTypeRef native_element = element
            ? value_type(state->function->owner, element, context) : nullptr;
        if (!boxed_reference && base && index && value && native_array
            && native_array != box && native_element
            && native_element != box && LLVMTypeOf(base) == pointer
            && LLVMTypeOf(value) == native_element
            && th_is_closed(ir_module_types(state->function->owner),
                referent)
            && !packed_array_width(state->function->owner, element)
            && n00bb_frame_pointer_free_value(state->function->owner, element)
            && literal_element_layout_matches(state->function->owner,
                element, native_element,
                LLVMGetModuleDataLayout(state->module))) {
            LLVMValueRef slot = native_array_slot(state, base, index,
                native_array, native_element,
                ir_machine_of(state->function->owner,
                    operands[1].value->type).class == IR_CLASS_SINT);
            return slot && LLVMBuildStore(state->builder, value, slot);
        }
        if (!boxed_reference && base && native_array
            && native_array != box
            && th_kind_of(th_resolve(referent)) == TH_SEQUENCE) {
            LLVMValueRef native_box = box_fresh_storage(state, base, referent);
            LLVMValueRef reference_slot = native_box
                ? entry_alloca(state, box) : nullptr;
            if (!reference_slot) break;
            LLVMBuildStore(state->builder, native_box, reference_slot);
            base = reference_slot;
        }
        if (boxed_reference) {
            LLVMValueRef reference_slot = entry_alloca(state, box);
            if (!reference_slot) break;
            LLVMBuildStore(state->builder, base, reference_slot);
            base = reference_slot;
        }
        if (!base || !index || !value
            || LLVMTypeOf(base) != pointer
            || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind)
            break;
        if (LLVMTypeOf(value) != box)
            value = box_value(state, value, operands[2].value->type);
        if (!value || LLVMTypeOf(value) != box) break;
        LLVMValueRef value_slot = entry_alloca(state, box);
        if (!value_slot) break;
        LLVMBuildStore(state->builder, value, value_slot);
        LLVMTypeRef parameters[] = {pointer, i64, pointer};
        LLVMTypeRef signature = LLVMFunctionType(
            LLVMVoidTypeInContext(context), parameters, 3, false);
        const char *helper_name = boxed_reference
            ? "n00bb_bootstrap_array_write_ref_box"
            : "n00bb_bootstrap_array_write_box";
        LLVMValueRef helper = LLVMGetNamedFunction(state->module,
            helper_name);
        if (!helper) helper = LLVMAddFunction(state->module,
            helper_name, signature);
        if (!helper || LLVMGlobalGetValueType(helper) != signature) break;
        LLVMValueRef helper_index = array_helper_index(state, index,
            operands[1].value->type);
        if (!helper_index) break;
        LLVMValueRef arguments[] = {base, helper_index, value_slot};
        LLVMBuildCall2(state->builder, signature, helper, arguments, 3, "");
        return true;
    }
    case IR_OP_SEQUENCE_LENGTH: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest) break;
        LLVMValueRef array = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef fields[] = {pointer, pointer};
        LLVMTypeRef array_type = LLVMStructTypeInContext(context,
            fields, 2, false);
        if (array && LLVMTypeOf(array) == box_llvm_type(context)) {
            LLVMValueRef bits = LLVMBuildExtractValue(state->builder,
                array, 3, "array.length.box.payload");
            LLVMValueRef address = LLVMBuildIntToPtr(state->builder,
                bits, pointer, "array.length.box.address");
            array = LLVMBuildLoad2(state->builder, array_type, address,
                "array.length.box.value");
        }
        if (!array || LLVMTypeOf(array) != array_type) break;
        LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder,
            array, 1, "array.length.descriptor");
        LLVMTypeRef descriptor_type = array_descriptor_type(context);
        LLVMValueRef slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 5, "array.length.slot");
        LLVMValueRef length = LLVMBuildLoad2(state->builder,
            LLVMInt64TypeInContext(context), slot, "array.length");
        return save_result(state, instruction, length);
    }
    case IR_OP_INDEX:
    case IR_OP_SEQUENCE_GET: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE || !instruction->dest)
            break;
        bool value_read = instruction->op == IR_OP_SEQUENCE_GET;
        uint64_t extent = 0;
        LLVMTypeRef array = value_read ? nullptr
            : inline_index_base_type(state, operands[0].value, &extent);
        LLVMValueRef base = lowered_value(state, operands[0].value);
        LLVMValueRef index = lowered_value(state, operands[1].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        bool boxed_reference = !value_read && base
            && LLVMTypeOf(base) == box_llvm_type(context)
            && th_kind_of(th_resolve(operands[0].value->type))
                == TH_INDIRECTION
            && th_kind_of(th_resolve(th_ind_referent(
                operands[0].value->type))) == TH_SEQUENCE;
        if (boxed_reference) {
            LLVMValueRef reference_slot = entry_alloca(state,
                box_llvm_type(context));
            if (!reference_slot) break;
            LLVMBuildStore(state->builder, base, reference_slot);
            base = reference_slot;
        }
        if (value_read && base) {
            LLVMTypeRef native_sequence = value_type(state->function->owner,
                operands[0].value->type, LLVMGetModuleContext(state->module));
            if (!native_sequence || LLVMTypeOf(base) != native_sequence)
                break;
            LLVMValueRef storage = entry_alloca(state, native_sequence);
            if (!storage) break;
            LLVMBuildStore(state->builder, base, storage);
            base = storage;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            if (native_sequence == box_llvm_type(context)) {
                LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef parameters[] = {pointer, i64, pointer};
                LLVMTypeRef signature = LLVMFunctionType(
                    LLVMVoidTypeInContext(context), parameters, 3, false);
                const char *helper_name =
                    value_has_one_immediate_use(state->function,
                        instruction, IR_OP_EXTRACT)
                    || value_has_one_immediate_use(state->function,
                        instruction, IR_OP_EXTRACT_INDEX)
                    ? "n00bb_bootstrap_array_borrow_box"
                    : "n00bb_bootstrap_array_read_box";
                LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                    helper_name);
                if (!helper) helper = LLVMAddFunction(state->module,
                    helper_name, signature);
                LLVMValueRef result_slot = entry_alloca(state,
                    box_llvm_type(context));
                if (!helper || !result_slot
                    || LLVMGlobalGetValueType(helper) != signature
                    || LLVMGetTypeKind(LLVMTypeOf(index))
                        != LLVMIntegerTypeKind) break;
                LLVMValueRef helper_index = array_helper_index(state, index,
                    operands[1].value->type);
                if (!helper_index) break;
                LLVMValueRef arguments[] = {base, helper_index, result_slot};
                LLVMBuildCall2(state->builder, signature, helper,
                    arguments, 3, "");
                result = LLVMBuildLoad2(state->builder,
                    box_llvm_type(context), result_slot,
                    "generic.array.element");
                return save_result(state, instruction, result);
            }
        }
        if (!base || !index
            || LLVMGetTypeKind(LLVMTypeOf(base)) != LLVMPointerTypeKind
            || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind) {
            backend_error(state->diagnostics,
                "Array index needs a native base pointer and integer index");
            break;
        }
        LLVMValueRef limit = nullptr;
        LLVMValueRef descriptor = nullptr;
        LLVMValueRef flat_rank = nullptr;
        th_type *sequence = nullptr;
        uint64_t packed_width = 0;
        bool inline_array = array != nullptr;
        if (inline_array) {
            if (!extent) break;
        } else {
            ir_module_t *source = state->function->owner;
            sequence = value_read ? operands[0].value->type
                : th_ind_referent(operands[0].value->type);
            if (!sequence || th_kind_of(th_resolve(sequence)) != TH_SEQUENCE) {
                backend_error(state->diagnostics,
                    "Array index base has no sequence type");
                break;
            }
            th_type *element = th_seq_elem(th_resolve(sequence));
            LLVMTypeRef native_element = element
                ? value_type(source, element, context) : nullptr;
            LLVMTypeRef native_array = value_type(source, sequence, context);
            LLVMValueRef dynamic_type = value_read ? nullptr
                : lowered_dynamic_type_index(state, operands[0].value);
            if (!value_read && native_array == box_llvm_type(context)
                && !dynamic_type)
                dynamic_type = runtime_type_value(state, sequence);
            if (native_array == box_llvm_type(context) && !value_read
                && value_type(source, instruction->dest->type, context)
                    == box_llvm_type(context)) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
                LLVMTypeRef allocator_params[] = {i32, i32, i64, i32,
                    pointer};
                LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
                    allocator_params, 5, false);
                const char *allocator_name = ir_func_execution_phase(
                    state->function) == IR_PHASE_COMPTIME
                    ? "n00b_comptime_allocate" : "n00b_allocate";
                LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                    allocator_name);
                if (!allocator) allocator = LLVMAddFunction(state->module,
                    allocator_name, allocator_type);
                LLVMTypeRef parameters[] = {pointer, i64, pointer, pointer};
                LLVMTypeRef signature = LLVMFunctionType(
                    LLVMVoidTypeInContext(context), parameters, 4, false);
                const char *helper_name = boxed_reference
                    ? "n00bb_bootstrap_array_ref_from_ref_box"
                    : "n00bb_bootstrap_array_ref_box";
                LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                    helper_name);
                if (!helper) helper = LLVMAddFunction(state->module,
                    helper_name, signature);
                LLVMValueRef result_slot = entry_alloca(state,
                    box_llvm_type(context));
                if (!allocator || !helper || !result_slot
                    || LLVMGlobalGetValueType(allocator) != allocator_type
                    || LLVMGlobalGetValueType(helper) != signature) break;
                LLVMValueRef helper_index = array_helper_index(state, index,
                    operands[1].value->type);
                if (!helper_index) break;
                LLVMValueRef arguments[] = {base, helper_index, allocator,
                    result_slot};
                LLVMBuildCall2(state->builder, signature, helper,
                    arguments, 4, "");
                result = LLVMBuildLoad2(state->builder,
                    box_llvm_type(context), result_slot,
                    "generic.array.reference");
                return save_result(state, instruction, result);
            }
            if (native_array == box_llvm_type(context) && dynamic_type) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
                LLVMTypeRef parameters[] = {pointer, i32, i64, pointer};
                LLVMTypeRef signature = LLVMFunctionType(pointer, parameters,
                                                           4, false);
                LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                    "n00bb_bootstrap_array_slot");
                if (!helper) helper = LLVMAddFunction(state->module,
                    "n00bb_bootstrap_array_slot", signature);
                LLVMValueRef child_index = entry_alloca(state, i32);
                if (!helper || !child_index
                    || LLVMGlobalGetValueType(helper) != signature) break;
                LLVMValueRef helper_index = array_helper_index(state, index,
                    operands[1].value->type);
                if (!helper_index) break;
                LLVMValueRef arguments[] = {base, dynamic_type,
                    helper_index, child_index};
                result = LLVMBuildCall2(state->builder, signature, helper,
                    arguments, 4, "generic.array.slot");
                LLVMValueRef element_index = LLVMBuildLoad2(state->builder,
                    i32, child_index, "generic.element.type");
                return result && save_result_with_dynamic_type(state,
                    instruction, result, element_index);
            }
            if (native_array == box_llvm_type(context)) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
                LLVMTypeRef parameters[] = {i32, i64, i64, pointer};
                LLVMTypeRef signature = LLVMFunctionType(pointer, parameters,
                                                           4, false);
                LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                    "n00bb_bootstrap_array_element");
                if (!helper) helper = LLVMAddFunction(state->module,
                    "n00bb_bootstrap_array_element", signature);
                if (!helper || LLVMGlobalGetValueType(helper) != signature)
                    break;
                LLVMValueRef boxed = LLVMBuildLoad2(state->builder,
                    native_array, base, "generic.array");
                LLVMValueRef child_index = entry_alloca(state, i32);
                LLVMValueRef helper_index = array_helper_index(state, index,
                    operands[1].value->type);
                if (!helper_index) break;
                LLVMValueRef arguments[] = {
                    LLVMBuildExtractValue(state->builder, boxed, 2,
                        "generic.array.type"),
                    LLVMBuildExtractValue(state->builder, boxed, 3,
                        "generic.array.payload"),
                    helper_index,
                    child_index,
                };
                if (!child_index) break;
                result = LLVMBuildCall2(state->builder, signature, helper,
                    arguments, 4, "generic.array.element");
                LLVMValueRef element_index = LLVMBuildLoad2(state->builder,
                    i32, child_index, "generic.element.type");
                return result && save_result_with_dynamic_type(state,
                    instruction, result, element_index);
            }
            packed_width = packed_array_width(source, element);
            if (!native_element || !native_array
                || (!packed_width && !literal_element_layout_matches(source,
                    element, native_element,
                    LLVMGetModuleDataLayout(state->module)))) {
                backend_error(state->diagnostics,
                    "Array index element has no compatible native layout");
                break;
            }
            LLVMValueRef value = LLVMBuildLoad2(state->builder, native_array,
                base, "array.value");
            descriptor = LLVMBuildExtractValue(state->builder,
                value, 1, "array.descriptor");
            base = LLVMBuildExtractValue(state->builder, value, 0,
                "array.data");
            LLVMTypeRef descriptor_type = array_descriptor_type(context);
            LLVMValueRef length_address = LLVMBuildStructGEP2(state->builder,
                descriptor_type, descriptor, 5, "array.length.address");
            limit = LLVMBuildLoad2(state->builder,
                LLVMInt64TypeInContext(context), length_address,
                "array.length");
            LLVMValueRef flat_address = LLVMBuildStructGEP2(state->builder,
                descriptor_type, descriptor, 2, "array.flat.rank.address");
            flat_rank = LLVMBuildLoad2(state->builder,
                LLVMInt32TypeInContext(context), flat_address,
                "array.flat.rank");
            array = native_element;
        }
        bool signed_index = ir_machine_of(state->function->owner,
            operands[1].value->type).class == IR_CLASS_SINT;
        limit = inline_array ? LLVMConstInt(
            LLVMInt64TypeInContext(context), extent, false) : limit;
        LLVMValueRef position = checked_array_index(state, index, limit,
            signed_index, !ir_index_proven(state->index_proofs, instruction));
        if (!position) break;
        LLVMTypeRef arithmetic = LLVMTypeOf(position);
        if (inline_array) {
            LLVMValueRef indices[] = {
                LLVMConstInt(arithmetic, 0, false), position,
            };
            result = LLVMBuildInBoundsGEP2(state->builder, array, base,
                indices, 2, "index.address");
        } else if (th_kind_of(th_resolve(th_seq_elem(th_resolve(sequence))))
                   == TH_SEQUENCE) {
            LLVMValueRef rectangular = LLVMBuildICmp(state->builder,
                LLVMIntUGT, flat_rank,
                LLVMConstInt(LLVMTypeOf(flat_rank), 1, false),
                "array.rectangular.suffix");
            ir_annot_t *direct = ir_annot_get(&instruction->annots,
                "n00bb.array.direct_ref");
            if (direct && direct->kind == IR_ANNOT_BOOL && direct->b) {
                emit_trap_if(state, rectangular);
                result = LLVMBuildInBoundsGEP2(state->builder,
                    array, base, &position, 1, "index.address");
                return save_result(state, instruction, result);
            }
            LLVMValueRef function = LLVMGetBasicBlockParent(
                LLVMGetInsertBlock(state->builder));
            LLVMBasicBlockRef flat = LLVMAppendBasicBlockInContext(context,
                function, "array.index.flat");
            LLVMBasicBlockRef ragged = LLVMAppendBasicBlockInContext(context,
                function, "array.index.ragged");
            LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
                function, "array.index.done");
            LLVMBuildCondBr(state->builder, rectangular, flat, ragged);
            LLVMPositionBuilderAtEnd(state->builder, flat);
            LLVMValueRef position64 = LLVMTypeOf(position)
                == LLVMInt64TypeInContext(context) ? position
                : LLVMBuildTrunc(state->builder, position,
                    LLVMInt64TypeInContext(context), "array.index64");
            LLVMValueRef suffix = rectangular_suffix_address(state, base,
                descriptor, position64, flat_rank, sequence);
            if (!suffix) return false;
            LLVMBasicBlockRef flat_exit = LLVMGetInsertBlock(state->builder);
            LLVMBuildBr(state->builder, done);
            LLVMPositionBuilderAtEnd(state->builder, ragged);
            LLVMValueRef child = LLVMBuildInBoundsGEP2(state->builder,
                array, base, &position, 1, "index.address");
            LLVMBuildBr(state->builder, done);
            LLVMPositionBuilderAtEnd(state->builder, done);
            result = LLVMBuildPhi(state->builder, LLVMTypeOf(child),
                "array.index.address");
            LLVMValueRef addresses[] = {suffix, child};
            LLVMBasicBlockRef blocks[] = {flat_exit, ragged};
            LLVMAddIncoming(result, addresses, blocks, 2);
        } else if (packed_width) {
            LLVMTypeRef descriptor_type = array_descriptor_type(context);
            LLVMValueRef flags_address = LLVMBuildStructGEP2(state->builder,
                descriptor_type, descriptor, 3, "array.flags.address");
            LLVMValueRef flags = LLVMBuildLoad2(state->builder,
                LLVMInt32TypeInContext(context), flags_address,
                "array.flags");
            LLVMValueRef invalid_flags = LLVMBuildAnd(state->builder, flags,
                LLVMConstInt(LLVMTypeOf(flags),
                    ~N00B_ABI_ARRAY_ORIGIN_BIT_MASK, false),
                "array.invalid.flags");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                invalid_flags, LLVMConstNull(LLVMTypeOf(flags)),
                "array.flags.invalid"));
            LLVMTypeRef i128 = LLVMInt128TypeInContext(context);
            LLVMValueRef bit_position = LLVMBuildAdd(state->builder,
                LLVMBuildMul(state->builder,
                    LLVMBuildZExtOrBitCast(state->builder, position, i128,
                        "array.index.wide"),
                    LLVMConstInt(i128, packed_width, false),
                    "array.index.element.bits"),
                LLVMBuildZExt(state->builder, flags, i128,
                    "array.bit.origin"), "array.bit.position");
            result = make_packed_bitptr(state, base, bit_position,
                th_seq_elem(th_resolve(sequence)));
            if (!result) break;
        } else {
            result = LLVMBuildInBoundsGEP2(state->builder, array, base,
                &position, 1, "index.address");
        }
        if (value_read) {
            LLVMTypeRef native_result = value_type(state->function->owner,
                instruction->dest->type, context);
            if (!native_result) break;
            result = packed_width ? load_packed_bitptr(state, result,
                    th_seq_elem(th_resolve(sequence)))
                : LLVMBuildLoad2(state->builder, native_result, result,
                    "sequence.element");
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
                || !floating_type(target)) break;
            result = from.class == IR_CLASS_SINT
                ? LLVMBuildSIToFP(state->builder, source, target, "cast.sitofp")
                : LLVMBuildUIToFP(state->builder, source, target, "cast.uitofp");
            return result && save_result(state, instruction, result);
        }
        if (from.class == IR_CLASS_FLOAT && to.class == IR_CLASS_FLOAT) {
            if (!floating_type(LLVMTypeOf(source)) || !floating_type(target))
                break;
            if (to.width > from.width && instruction->n_mods == 0)
                result = LLVMBuildFPExt(state->builder, source, target,
                                        "cast.fpext");
            else if (to.width < from.width && instruction->n_mods == 1
                     && ir_instr_has_mod(instruction, "nearest"))
                result = LLVMBuildFPTrunc(state->builder, source, target,
                                          "cast.fptrunc");
            else if (to.width == from.width) result = source;
            else break;
            return result && save_result(state, instruction, result);
        }
        if (from.class == IR_CLASS_FLOAT && to_integer) {
            if (instruction->n_mods != 2
                || !ir_instr_has_mod(instruction, "trap")
                || !ir_instr_has_mod(instruction, "to_zero")
                || !floating_type(LLVMTypeOf(source))
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
        th_type *source_type = operands[0].value->type;
        th_type *source_shape = ir_type_definition(state->function->owner,
            source_type);
        if (!source_shape && th_kind_of(th_resolve(source_type)) == TH_RECORD)
            source_shape = th_resolve(source_type);
        th_type *target_shape = ir_type_definition(state->function->owner,
            instruction->dest->type);
        if (source_shape && target_shape
            && th_kind_of(source_shape) == TH_RECORD
            && th_kind_of(target_shape) == TH_RECORD) {
            result = nominal_record_value(state, source, source_type,
                instruction->dest->type, source_shape, target_shape, target);
            return result && save_result(state, instruction, result);
        }
        LLVMTypeRef source_native_type = LLVMTypeOf(source);
        if (source_native_type == target)
            return save_result(state, instruction, source);
        LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
        if (!layout || LLVMABISizeOfType(layout, source_native_type)
                != LLVMABISizeOfType(layout, target)
            || LLVMABIAlignmentOfType(layout, source_native_type)
                != LLVMABIAlignmentOfType(layout, target)) break;
        LLVMValueRef slot = LLVMBuildAlloca(state->builder, source_native_type,
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
        const char *source_name = nullptr, *target_name = nullptr;
        size_t source_arity = 0, target_arity = 0;
        th_type *source_type = th_resolve(operands[0].value->type);
        th_type *target_type = th_resolve(instruction->dest->type);
        LLVMTypeRef target_native = value_type(state->function->owner,
            target_type, LLVMGetModuleContext(state->module));
        if (target_native && LLVMTypeOf(source) == target_native
            && ir_type_eq(state->function->owner, source_type, target_type))
            return save_result(state, instruction, source);
        bool source_option = th_named_info(source_type, &source_name,
            nullptr, nullptr, &source_arity) && source_name
            && !strcmp(source_name, "option") && source_arity == 1;
        bool target_option = th_named_info(target_type, &target_name,
            nullptr, nullptr, &target_arity) && target_name
            && !strcmp(target_name, "option") && target_arity == 1;
        if (source_option && target_option) {
            const th_arg *element = th_named_arg(source_type, 0);
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef target = value_type(state->function->owner,
                target_type, context);
            if (!element || element->kind != TH_ARG_TYPE || !target
                || LLVMGetTypeKind(target) != LLVMStructTypeKind
                || LLVMCountStructElementTypes(target) != 2) break;
            LLVMValueRef tag = LLVMBuildExtractValue(state->builder, source,
                1, "option.tag");
            LLVMValueRef present = LLVMBuildICmp(state->builder, LLVMIntNE,
                tag, LLVMConstNull(LLVMTypeOf(tag)), "option.present");
            LLVMBasicBlockRef yes = LLVMAppendBasicBlockInContext(context,
                state->target_function, "option.box.present");
            LLVMBasicBlockRef no = LLVMAppendBasicBlockInContext(context,
                state->target_function, "option.box.absent");
            LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
                state->target_function, "option.box.done");
            LLVMBuildCondBr(state->builder, present, yes, no);
            LLVMPositionBuilderAtEnd(state->builder, yes);
            LLVMValueRef payload = LLVMBuildExtractValue(state->builder,
                source, 0, "option.payload");
            LLVMValueRef boxed = box_value(state, payload, element->type);
            if (!boxed) break;
            LLVMValueRef some = LLVMBuildInsertValue(state->builder,
                LLVMGetUndef(target), boxed, 0, "option.box.payload");
            some = LLVMBuildInsertValue(state->builder, some, tag, 1,
                "option.box.tag");
            LLVMBuildBr(state->builder, done);
            LLVMBasicBlockRef yes_tail = LLVMGetInsertBlock(state->builder);
            LLVMPositionBuilderAtEnd(state->builder, no);
            LLVMValueRef none = LLVMConstNull(target);
            LLVMBuildBr(state->builder, done);
            LLVMBasicBlockRef no_tail = LLVMGetInsertBlock(state->builder);
            LLVMPositionBuilderAtEnd(state->builder, done);
            result = LLVMBuildPhi(state->builder, target, "option.box");
            LLVMValueRef incoming[] = {some, none};
            LLVMBasicBlockRef predecessors[] = {yes_tail, no_tail};
            LLVMAddIncoming(result, incoming, predecessors, 2);
            return save_result(state, instruction, result);
        }
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
    case IR_OP_EXTRACT_INDEX: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_CONST
            || operands[1].lit.is_float || operands[1].lit.i.neg
            || operands[1].lit.i.mag > UINT_MAX || !instruction->dest)
            break;
        LLVMValueRef tuple = lowered_value(state, operands[0].value);
        unsigned index = (unsigned)operands[1].lit.i.mag;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef expected = value_type(state->function->owner,
            instruction->dest->type, context);
        if (tuple && expected && LLVMTypeOf(tuple) == box_llvm_type(context)) {
            LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(tuple));
            char field_name[3 * sizeof(size_t) + 2];
            int length = snprintf(field_name, sizeof(field_name), "%u", index);
            if (!slot || length < 0
                || (size_t)length >= sizeof(field_name)) break;
            LLVMBuildStore(state->builder, tuple, slot);
            LLVMValueRef boxed = dynamic_field_box(state, slot, field_name,
                index, true);
            result = boxed && expected == box_llvm_type(context) ? boxed
                : boxed ? unbox_value(state, boxed,
                    instruction->dest->type) : nullptr;
            return result && save_result(state, instruction, result);
        }
        if (!tuple || !expected
            || LLVMGetTypeKind(LLVMTypeOf(tuple)) != LLVMStructTypeKind
            || index >= LLVMCountStructElementTypes(LLVMTypeOf(tuple)))
            break;
        result = LLVMBuildExtractValue(state->builder, tuple, index,
            "tuple.element");
        return result && LLVMTypeOf(result) == expected
            && save_result(state, instruction, result);
    }
    case IR_OP_EXTRACT: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME || !operands[1].name
            || !instruction->dest || !operands[0].value) break;
        th_type *record_type = th_resolve(operands[0].value->type);
        LLVMTypeRef native = nullptr;
        unsigned member_index = 0;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (value && LLVMTypeOf(value) == box_llvm_type(context)) {
            ir_module_t *owner = state->function->owner;
            LLVMTypeRef native_result = value_type(owner,
                instruction->dest->type, context);
            const ir_final_layout_t *record_layout = ir_layout_final(owner,
                record_type, ir_module_target(owner));
            const ir_final_member_layout_t *member =
                known_byte_record_member(owner, record_type,
                    operands[1].name);
            LLVMTargetDataRef data_layout = LLVMGetModuleDataLayout(
                state->module);
            LLVMTypeKind native_kind = native_result
                ? LLVMGetTypeKind(native_result) : LLVMVoidTypeKind;
            if (record_layout && record_layout->intrinsic_bits > 64
                && member && native_result && data_layout
                && (native_kind == LLVMIntegerTypeKind
                    || native_kind == LLVMFloatTypeKind
                    || native_kind == LLVMDoubleTypeKind
                    || native_kind == LLVMPointerTypeKind)
                && LLVMABISizeOfType(data_layout, native_result) * 8
                    == member->occupied[0].width
                && (member->bit_offset / 8)
                    % LLVMABIAlignmentOfType(data_layout, native_result)
                    == 0) {
                LLVMValueRef bits = LLVMBuildExtractValue(state->builder,
                    value, 3, "boxed.record.address");
                LLVMValueRef base = LLVMBuildIntToPtr(state->builder,
                    bits, LLVMPointerTypeInContext(context, 0),
                    "boxed.record.base");
                LLVMValueRef offset = LLVMConstInt(
                    LLVMInt64TypeInContext(context),
                    member->bit_offset / 8, false);
                LLVMValueRef address = LLVMBuildInBoundsGEP2(state->builder,
                    LLVMInt8TypeInContext(context), base, &offset, 1,
                    "boxed.field.address");
                result = LLVMBuildLoad2(state->builder, native_result,
                    address, "boxed.field.value");
                return save_result(state, instruction, result);
            }
            LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(value));
            if (!slot) break;
            LLVMBuildStore(state->builder, value, slot);
            LLVMValueRef boxed = dynamic_field_box(state, slot,
                operands[1].name,
                record_member_ordinal(state->function->owner, record_type,
                                      operands[1].name), true);
            result = boxed && native_result == box_llvm_type(context)
                ? boxed : boxed ? unbox_value(state, boxed,
                    instruction->dest->type) : nullptr;
            return result && save_result(state, instruction, result);
        }
        if (!value) {
            state->failure_detail = "record source has no lowered value";
            break;
        }
        if (!record_member_index(state, record_type, operands[1].name,
                                 &native, &member_index)) {
            th_type *definition = ir_type_definition(
                state->function->owner, record_type);
            const ir_final_layout_t *layout = ir_layout_final(
                state->function->owner, record_type,
                ir_module_target(state->function->owner));
            if (!state->failure_detail) state->failure_detail = !definition
                ? "record source has no type definition"
                : !layout ? "record source has no finalized layout"
                : "record field has no matching layout";
            break;
        }
        if (LLVMTypeOf(value) != native) {
            state->failure_detail = "record source differs from its layout type";
            break;
        }
        result = LLVMBuildExtractValue(state->builder, value, member_index,
                                       "field.value");
        LLVMTypeRef semantic = value_type(state->function->owner,
            instruction->dest->type, context);
        if (result && semantic
            && ir_machine_of(state->function->owner,
                instruction->dest->type).class == IR_CLASS_BOOL
            && LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMTypeOf(result)) == 8
            && LLVMGetIntTypeWidth(semantic) == 1)
            result = LLVMBuildTrunc(state->builder, result, semantic,
                "field.bool.value");
        if (!result || LLVMTypeOf(result) != value_type(
                state->function->owner, instruction->dest->type,
                LLVMGetModuleContext(state->module))) {
            state->failure_detail = "record field differs from its result type";
            break;
        }
        return save_result(state, instruction, result);
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
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef type = value_type(state->function->owner,
            operands[0].type, context);
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) break;
        if (instruction->op == IR_OP_ALLOC
            && type == box_llvm_type(context)) {
            bool local = ir_allocation_has_only_local_uses(state->function,
                                                           instruction);
            LLVMValueRef type_index = runtime_type_value(state,
                operands[0].type);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
            LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
                allocator_params, 5, false);
            const char *allocator_name = ir_func_execution_phase(
                state->function) == IR_PHASE_COMPTIME
                ? "n00b_comptime_allocate" : "n00b_allocate";
            LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                allocator_name);
            if (!allocator) allocator = LLVMAddFunction(state->module,
                allocator_name, allocator_type);
            LLVMTypeRef helper_params[] = {i32, pointer, pointer};
            LLVMTypeRef helper_type = LLVMFunctionType(
                LLVMVoidTypeInContext(context), helper_params, 3, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                local ? "n00bb_bootstrap_new_box"
                      : "n00bb_bootstrap_new_ref_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                local ? "n00bb_bootstrap_new_box"
                      : "n00bb_bootstrap_new_ref_box", helper_type);
            result = entry_alloca(state, type);
            if (!type_index || !allocator || !helper || !result
                || LLVMGlobalGetValueType(allocator) != allocator_type
                || LLVMGlobalGetValueType(helper) != helper_type) break;
            LLVMValueRef args[] = {type_index, allocator, result};
            LLVMBuildCall2(state->builder, helper_type, helper, args, 3, "");
            return save_result(state, instruction, local ? result
                : LLVMBuildLoad2(state->builder, type, result,
                    "generic.storage.reference"));
        }
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
                    "allocation layout mismatch for %s (type index %u, IR size %llu, LLVM size %llu, IR alignment %u, LLVM alignment %u)",
                    th_print(ir_module_types(state->function->owner),
                        operands[0].type),
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
                "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator");
            if (!allocator)
                allocator = LLVMAddFunction(state->module,
                    "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator",
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
        if (result && uses_bitptr(state->function->owner,
                instruction->dest->type)) {
            LLVMValueRef offset = LLVMConstNull(
                LLVMInt64TypeInContext(context));
            result = make_packed_bitptr(state, result, offset,
                operands[0].type);
        }
        return result && save_result(state, instruction, result);
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
        if (th_kind_of(th_resolve(operands[0].value->global->type))
                == TH_SEQUENCE
            && !sequence_value_transfers_storage(operands[1].value)) {
            value = copy_sequence_value(state, value,
                                        operands[0].value->global->type);
            if (!value) break;
        }
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
        if (!guard.valid) {
            state->failure_detail = "load is blocked by a global read guard";
            return false;
        }
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!address || !type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) {
            state->failure_detail = "load has no lowerable address or value type";
            break;
        }
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (LLVMTypeOf(address) == box_llvm_type(context)) {
            if (!instruction->n_mods
                && type == box_llvm_type(context)
                && th_kind_of(th_resolve(instruction->dest->type))
                    == TH_SEQUENCE
                && !uses_bitptr(state->function->owner,
                    operands[0].value->type)
                && value_has_one_immediate_use(state->function,
                    instruction, IR_OP_SEQUENCE_GET)) {
                LLVMValueRef reference_type = LLVMBuildExtractValue(
                    state->builder, address, 2, "borrow.reference.type");
                LLVMValueRef array_type = runtime_type_component(state,
                    reference_type, 0);
                if (array_type) {
                    LLVMValueRef payload = LLVMBuildExtractValue(
                        state->builder, address, 3, "borrow.array.address");
                    LLVMValueRef borrowed = LLVMConstNull(type);
                    borrowed = LLVMBuildInsertValue(state->builder,
                        borrowed, array_type, 2, "borrow.array.type");
                    result = LLVMBuildInsertValue(state->builder,
                        borrowed, payload, 3, "borrow.array.value");
                    return save_result(state, instruction, result);
                }
            }
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef parameters[] = {pointer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 2, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_ref_read_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_ref_read_box", signature);
            LLVMValueRef source_slot = entry_alloca(state,
                box_llvm_type(context));
            LLVMValueRef result_slot = entry_alloca(state,
                box_llvm_type(context));
            if (instruction->n_mods || !helper || !source_slot || !result_slot
                || LLVMGlobalGetValueType(helper) != signature) {
                state->failure_detail = "boxed reference read has no helper";
                break;
            }
            LLVMBuildStore(state->builder, address, source_slot);
            LLVMValueRef arguments[] = {source_slot, result_slot};
            LLVMBuildCall2(state->builder, signature, helper,
                arguments, 2, "");
            LLVMValueRef boxed = LLVMBuildLoad2(state->builder,
                box_llvm_type(context), result_slot,
                "generic.reference.read");
            result = type == box_llvm_type(context) ? boxed
                : unbox_value(state, boxed, instruction->dest->type);
            if (!result)
                state->failure_detail =
                    "boxed reference read cannot unbox its value";
            bool saved = result && save_result(state, instruction, result);
            if (result && !saved)
                state->failure_detail =
                    "boxed reference read cannot root its result";
            return saved;
        }
        th_type *storage = th_ind_referent(operands[0].value->type);
        LLVMValueRef dynamic_type = lowered_dynamic_type_index(state,
            operands[0].value);
        if (dynamic_type) {
            if (instruction->n_mods
                || type != box_llvm_type(LLVMGetModuleContext(state->module)))
                break;
            result = dynamic_box_load(state, address, dynamic_type);
            bool saved = result && save_result(state, instruction, result);
            if (!saved)
                state->failure_detail =
                    "dynamic reference load cannot root its result";
            return saved;
        }
        if (uses_bitptr(state->function->owner,
                        operands[0].value->type)) {
            if (instruction->n_mods || !storage
                || !packed_array_width(state->function->owner, storage)) break;
            if (LLVMTypeOf(address) != bitptr_llvm_type(
                    LLVMGetModuleContext(state->module))) {
                const ir_instr_t *source = operands[0].value->def;
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "packed value load has a non-bitptr address from %s",
                    source ? ir_op_spelling(source->op) : "an input value");
                bool reported = backend_error(state->diagnostics,
                                              message.data);
                core_buffer_release(&message);
                return reported;
            }
            result = load_packed_bitptr(state, address, storage);
            bool saved = result && save_result(state, instruction, result);
            if (!saved)
                state->failure_detail =
                    "packed reference load cannot root its result";
            return saved;
        }
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
        bool saved = save_result(state, instruction, result);
        if (!saved)
            state->failure_detail = "native reference load cannot root its result";
        return saved;
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
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        th_type *storage = th_ind_referent(operands[0].value->type);
        if (LLVMTypeOf(address) == box_llvm_type(context)) {
            LLVMTypeRef box = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef parameters[] = {pointer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 2, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_ref_write_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_ref_write_box", signature);
            if (instruction->n_mods || !helper
                || LLVMGlobalGetValueType(helper) != signature) break;
            if (LLVMTypeOf(value) != box)
                value = box_value(state, value, operands[1].value->type);
            if (value && storage
                && th_kind_of(th_resolve(storage)) == TH_SEQUENCE
                && !sequence_value_transfers_storage(operands[1].value))
                value = copy_boxed_array(state, value);
            LLVMValueRef address_slot = entry_alloca(state, box);
            LLVMValueRef value_slot = entry_alloca(state, box);
            if (!value || !address_slot || !value_slot) break;
            LLVMBuildStore(state->builder, address, address_slot);
            LLVMBuildStore(state->builder, value, value_slot);
            LLVMValueRef args[] = {address_slot, value_slot};
            return LLVMBuildCall2(state->builder, signature, helper,
                args, 2, "") != nullptr;
        }
        LLVMValueRef dynamic_type = lowered_dynamic_type_index(state,
            operands[0].value);
        if (dynamic_type) {
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            if (instruction->n_mods) break;
            if (LLVMTypeOf(value) != box)
                value = box_value(state, value, operands[1].value->type);
            if (value && th_kind_of(th_resolve(storage)) == TH_SEQUENCE
                && !sequence_value_transfers_storage(operands[1].value))
                value = copy_boxed_array(state, value);
            LLVMValueRef value_slot = value ? entry_alloca(state, box)
                : nullptr;
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef parameters[] = {pointer,
                LLVMInt32TypeInContext(context), pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 3, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_store_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_store_box", signature);
            if (!value_slot || !helper
                || LLVMGlobalGetValueType(helper) != signature) break;
            LLVMBuildStore(state->builder, value, value_slot);
            LLVMValueRef args[] = {address, dynamic_type, value_slot};
            return LLVMBuildCall2(state->builder, signature, helper,
                                  args, 3, "") != nullptr;
        }
        if (uses_bitptr(state->function->owner,
                        operands[0].value->type)) {
            if (instruction->n_mods || !storage
                || !packed_array_width(state->function->owner, storage)
                || LLVMTypeOf(address) != bitptr_llvm_type(
                    LLVMGetModuleContext(state->module)))
            {
                core_buffer_t message = {0};
                core_buffer_printf(&message,
                    "packed value store unsupported in %s.%s for %s (width %llu, address kind %d, producer %s)",
                    ir_module_name(state->function->owner),
                    state->function->name,
                    th_print(ir_module_types(state->function->owner),
                        operands[0].value->type),
                    (unsigned long long)packed_array_width(
                        state->function->owner, storage),
                    (int)LLVMGetTypeKind(LLVMTypeOf(address)),
                    operands[0].value->def
                        ? ir_op_spelling(operands[0].value->def->op)
                        : "value");
                bool reported = backend_error(state->diagnostics,
                    message.data);
                core_buffer_release(&message);
                return reported;
            }
            if (!store_packed_bitptr(state, address, value, storage)) break;
            const ir_global_t *global = direct_global_address(
                operands[0].value);
            return !global || mark_global_initialized(state, global);
        }
        LLVMBasicBlockRef row_done = nullptr;
        ir_instr_t *indexed = operands[0].value->def;
        if (indexed && indexed->op == IR_OP_INDEX
            && indexed->n_operands && indexed->operands[0].kind == IR_OPD_VALUE
            && th_kind_of(th_resolve(storage)) == TH_SEQUENCE) {
            LLVMValueRef parent_address = lowered_value(state,
                indexed->operands[0].value);
            th_type *parent_type = th_ind_referent(
                indexed->operands[0].value->type);
            LLVMTypeRef native_parent = value_type(state->function->owner,
                parent_type, LLVMGetModuleContext(state->module));
            if (!parent_address || !native_parent) break;
            LLVMValueRef parent = LLVMBuildLoad2(state->builder,
                native_parent, parent_address, "array.row.parent");
            LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder,
                parent, 1, "array.row.parent.descriptor");
            LLVMTypeRef descriptor_type = array_descriptor_type(
                LLVMGetModuleContext(state->module));
            LLVMValueRef flat_slot = LLVMBuildStructGEP2(state->builder,
                descriptor_type, descriptor, 2, "array.row.parent.flat.slot");
            LLVMValueRef flat_rank = LLVMBuildLoad2(state->builder,
                LLVMInt32TypeInContext(LLVMGetModuleContext(state->module)),
                flat_slot, "array.row.parent.flat");
            LLVMValueRef rectangular = LLVMBuildICmp(state->builder,
                LLVMIntUGT, flat_rank,
                LLVMConstInt(LLVMTypeOf(flat_rank), 1, false),
                "array.row.is.rectangular");
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMValueRef function = LLVMGetBasicBlockParent(
                LLVMGetInsertBlock(state->builder));
            LLVMBasicBlockRef flat = LLVMAppendBasicBlockInContext(context,
                function, "array.row.copy");
            LLVMBasicBlockRef ragged = LLVMAppendBasicBlockInContext(context,
                function, "array.row.store");
            row_done = LLVMAppendBasicBlockInContext(context,
                function, "array.row.done");
            LLVMBuildCondBr(state->builder, rectangular, flat, ragged);
            LLVMPositionBuilderAtEnd(state->builder, flat);
            if (!copy_rectangular_row(state, address, value, storage))
                return false;
            LLVMBuildBr(state->builder, row_done);
            LLVMPositionBuilderAtEnd(state->builder, ragged);
        }
        if (th_kind_of(th_resolve(storage)) == TH_SEQUENCE
            && !sequence_value_transfers_storage(operands[1].value)) {
            value = copy_sequence_value(state, value, storage);
            if (!value) break;
        }
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
        ir_local_t *local = state->frame_roots
            ? direct_local_address(operands[0].value) : nullptr;
        const core_indexed_entry_t *local_root = local
            ? core_indexed_table_find(state->frame_roots, local) : nullptr;
        if (local_root) {
            LLVMValueRef pointer = frame_root_pointer(state, value,
                local->type);
            if (!pointer) return false;
            LLVMValueRef write = LLVMBuildStore(state->builder, pointer,
                state->frame_slots[local_root->index]);
            LLVMSetVolatile(write, true);
        }
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
        if (row_done) {
            LLVMBuildBr(state->builder, row_done);
            LLVMPositionBuilderAtEnd(state->builder, row_done);
        }
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
        int fixed = static_type_test_result(state, instruction);
        if (fixed >= 0)
            return save_result(state, instruction, LLVMConstInt(
                LLVMInt1TypeInContext(LLVMGetModuleContext(state->module)),
                (unsigned)fixed, false));
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMValueRef type_field = nullptr;
        if (value && LLVMTypeOf(value) == box_llvm_type(context))
            type_field = LLVMBuildExtractValue(state->builder, value, 2,
                "type.field");
        else if (operands[0].value && th_is_closed(
                ir_module_types(state->function->owner),
                operands[0].value->type)) {
            uint32_t source_index = runtime_type_index(state,
                operands[0].value->type);
            if (source_index)
                type_field = LLVMConstInt(i32, source_index, false);
        }
        if (!type_field) {
            state->failure_detail = "typecase subject has no runtime type";
            break;
        }
        if (!th_is_closed(ir_module_types(state->function->owner),
                          operands[1].type)) {
            th_type *pattern = th_resolve(operands[1].type);
            th_type *element = th_kind_of(pattern) == TH_SEQUENCE
                ? th_resolve(th_seq_elem(pattern)) : nullptr;
            LLVMValueRef match = nullptr;
            if (element && th_kind_of(element) == TH_VAR
                && !th_var_constraint_count(element)) {
                LLVMTypeRef signature = LLVMFunctionType(i32, &i32, 1,
                                                         false);
                LLVMValueRef predicate = LLVMGetNamedFunction(state->module,
                    "n00bb_bootstrap_type_is_array");
                if (!predicate) predicate = LLVMAddFunction(state->module,
                    "n00bb_bootstrap_type_is_array", signature);
                if (predicate) match = LLVMBuildCall2(state->builder,
                    signature, predicate, &type_field, 1, "type.is_array");
            }
            else if (th_kind_of(pattern) == TH_VAR) {
                LLVMTypeRef parameters[] = {i32, i32};
                LLVMTypeRef signature = LLVMFunctionType(i32, parameters, 2,
                                                         false);
                LLVMValueRef predicate = LLVMGetNamedFunction(state->module,
                    "n00bb_bootstrap_type_specializes");
                if (!predicate) predicate = LLVMAddFunction(state->module,
                    "n00bb_bootstrap_type_specializes", signature);
                bool valid = predicate != nullptr;
                bool has_trait = false;
                for (size_t i = 0; valid
                     && i < th_var_constraint_count(pattern); i++) {
                    th_constraint constraint;
                    valid = th_var_constraint_at(pattern, i, &constraint);
                    if (!valid) break;
                    if (constraint.kind == TH_CON_METHOD) continue;
                    if (constraint.kind != TH_CON_IMPLEMENTS
                        || !constraint.iface) { valid = false; break; }
                    uint32_t trait_index = runtime_trait_index(state,
                        constraint.iface);
                    if (!trait_index) {
                        state->failure_detail =
                            "typecase trait has no runtime type index";
                        valid = false;
                        break;
                    }
                    LLVMValueRef args[] = {type_field,
                        LLVMConstInt(i32, trait_index, false)};
                    LLVMValueRef check = LLVMBuildCall2(state->builder,
                        signature, predicate, args, 2, "type.specializes");
                    match = match ? LLVMBuildAnd(state->builder, match,
                        check, "type.traits") : check;
                    has_trait = true;
                }
                if (!valid || !has_trait) match = nullptr;
            }
            if (!match) break;
            result = LLVMBuildICmp(state->builder, LLVMIntEQ, match,
                LLVMConstInt(i32, 1, false), "type.matches");
            return save_result(state, instruction, result);
        }
        uint32_t index = runtime_type_index(state, operands[1].type);
        if (!index) break;
        LLVMValueRef actual = type_field;
        actual = LLVMBuildAnd(state->builder, actual,
            LLVMConstInt(LLVMTypeOf(actual), UINT32_C(0x7fffffff), false),
            "type.index.masked");
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, actual,
            LLVMConstInt(LLVMTypeOf(actual), index, false), "type.matches");
        return save_result(state, instruction, result);
    }
    case IR_OP_TYPE_NARROW: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_TYPE || !instruction->dest) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (!value) break;
        if (LLVMTypeOf(value) == box_llvm_type(context))
            result = unbox_value(state, value, operands[1].type);
        else {
            LLVMTypeRef target = value_type(state->function->owner,
                instruction->dest->type, context);
            if (th_is_closed(ir_module_types(state->function->owner),
                    operands[0].value->type)
                && !th_is_closed(ir_module_types(state->function->owner),
                    operands[1].type))
                result = value;
            else if (!target) break;
            else if (LLVMTypeOf(value) == target)
                result = value;
            else if (th_is_closed(ir_module_types(state->function->owner),
                    operands[0].value->type)
                && th_is_closed(ir_module_types(state->function->owner),
                    operands[1].type)
                && runtime_type_index(state, operands[0].value->type)
                    != runtime_type_index(state, operands[1].type))
                result = LLVMGetPoison(target);
        }
        return result && save_result(state, instruction, result);
    }
    case IR_OP_AGGREGATE:
        {
        th_type *aggregate = instruction->dest
            ? th_resolve(instruction->dest->type) : nullptr;
        const char *name = nullptr;
        bool tuple = aggregate && th_named_info(aggregate, &name, nullptr,
            nullptr, nullptr) && name && !strcmp(name, TH_TUPLE);
        result = tuple ? tuple_aggregate(state, instruction)
                       : record_aggregate(state, instruction);
        if (!result) result = dynamic_array(state, instruction);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
        }
    case IR_OP_ARRAY_NEW:
        result = new_array(state, instruction);
        return result ? save_result(state, instruction, result)
                      : unsupported(state, instruction);
    case IR_OP_SEQUENCE_SHAPE_CHECK: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->shape_constraint) break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
        LLVMTypeRef fields[] = {pointer, pointer};
        LLVMTypeRef array_type = LLVMStructTypeInContext(context,
            fields, 2, false);
        LLVMValueRef array = lowered_value(state, operands[0].value);
        if (array && LLVMTypeOf(array) == box_llvm_type(context)) {
            LLVMValueRef bits = LLVMBuildExtractValue(state->builder,
                array, 3, "array.shape.box.payload");
            LLVMValueRef address = LLVMBuildIntToPtr(state->builder,
                bits, pointer, "array.shape.box.address");
            array = LLVMBuildLoad2(state->builder, array_type, address,
                "array.shape.box.value");
        }
        if (!array || LLVMTypeOf(array) != array_type) break;
        LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder,
            array, 1, "array.shape.descriptor");
        LLVMTypeRef descriptor_type = array_descriptor_type(context);
        LLVMValueRef rank_slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 1, "array.shape.rank.slot");
        LLVMValueRef rank = LLVMBuildLoad2(state->builder, i32,
            rank_slot, "array.shape.rank");
        uint32_t expected_rank = ir_shape_rank(instruction->shape_constraint);
        if (!expected_rank) break;
        emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
            rank, LLVMConstInt(i32, expected_rank, false),
            "array.shape.rank.mismatch"));
        LLVMValueRef first_extent = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 5, "array.shape.extents");
        for (uint32_t i = 0; i < expected_rank; i++) {
            ir_shape_extent_t expected = {0};
            if (!ir_shape_extent(instruction->shape_constraint,
                    i, &expected)) return unsupported(state, instruction);
            if (expected.kind == IR_SHAPE_EXTENT_UNRESOLVED) continue;
            LLVMValueRef wanted = expected.kind == IR_SHAPE_EXTENT_STATIC
                ? LLVMConstInt(i64, expected.static_value, false)
                : expected.value ? lowered_value(state, expected.value)
                                 : nullptr;
            if (!wanted || LLVMTypeOf(wanted) != i64)
                return unsupported(state, instruction);
            LLVMValueRef offset = LLVMConstInt(i64, i, false);
            LLVMValueRef slot = LLVMBuildGEP2(state->builder, i64,
                first_extent, &offset, 1, "array.shape.extent.slot");
            LLVMValueRef actual = LLVMBuildLoad2(state->builder, i64,
                slot, "array.shape.extent");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                actual, wanted, "array.shape.extent.mismatch"));
        }
        return true;
    }
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
            result = materialized_graph_value(state, instruction);
            bool cloned_graph = result != nullptr;
            if (!result) result = materialized_array(state, instruction);
            if (!result) result = materialized_string(state, instruction);
            if (!result) result = materialized_reloc_array(state, instruction);
            if (!result) result = materialized_record_array(state, instruction);
            if (!result) result = materialized_reloc_value(state, instruction);
            if (!result) result = materialized_nested_array(state, instruction);
            if (!result) result = materialized_raw_value(state, instruction);
            if (result && instruction->dest && !cloned_graph)
                result = copy_literal_value(state, result,
                                            instruction->dest->type);
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
        if (floating_type(type)) {
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
        if (floating_type(LLVMTypeOf(left))) {
            if (instruction->n_mods) break;
            result = instruction->op == IR_OP_ADD
                ? LLVMBuildFAdd(state->builder, left, right, "arithmetic")
                : instruction->op == IR_OP_SUB
                    ? LLVMBuildFSub(state->builder, left, right, "arithmetic")
                    : LLVMBuildFMul(state->builder, left, right, "arithmetic");
            return save_result(state, instruction, result);
        }
        if (LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMIntegerTypeKind) break;
        bool safe_sub = instruction->op == IR_OP_SUB
            && ir_unsigned_sub_proven_safe(instruction);
        if (ir_instr_has_mod(instruction, "trap") && !safe_sub) {
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
            ir_instr_has_mod(instruction, "trap") && !safe_sub);
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
        if (floating_type(type)) {
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
        const char *left_name = nullptr;
        bool string_compare = th_named_info(
                             th_resolve(operands[0].value->type),
                             &left_name, nullptr, nullptr, nullptr)
            && left_name && !strcmp(left_name, "string");
        bool boxed_reference = kind == LLVMStructTypeKind
            && LLVMTypeOf(left) == box_llvm_type(
                LLVMGetModuleContext(state->module))
            && th_kind_of(th_resolve(operands[0].value->type))
                == TH_INDIRECTION;
        bool bit_reference = kind == LLVMStructTypeKind
            && LLVMTypeOf(left) == bitptr_llvm_type(
                LLVMGetModuleContext(state->module))
            && th_kind_of(th_resolve(operands[0].value->type))
                == TH_INDIRECTION;
        if (bit_reference && (instruction->op == IR_OP_EQ
                              || instruction->op == IR_OP_NE)) {
            LLVMValueRef equal = LLVMConstInt(
                LLVMInt1TypeInContext(LLVMGetModuleContext(state->module)),
                1, false);
            for (unsigned field = 0; field < 4; field++) {
                LLVMValueRef a = LLVMBuildExtractValue(state->builder,
                    left, field, "bitref.left");
                LLVMValueRef b = LLVMBuildExtractValue(state->builder,
                    right, field, "bitref.right");
                LLVMValueRef same = LLVMBuildICmp(state->builder, LLVMIntEQ,
                    a, b, "bitref.field.equal");
                equal = LLVMBuildAnd(state->builder, equal, same,
                    "bitref.equal");
            }
            result = instruction->op == IR_OP_EQ ? equal
                : LLVMBuildNot(state->builder, equal, "bitref.not.equal");
        } else if (boxed_reference && (instruction->op == IR_OP_EQ
                                || instruction->op == IR_OP_NE)) {
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef parameters[] = {pointer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMInt32TypeInContext(context), parameters, 2, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_ref_equal_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_ref_equal_box", signature);
            LLVMValueRef left_slot = entry_alloca(state,
                box_llvm_type(context));
            LLVMValueRef right_slot = entry_alloca(state,
                box_llvm_type(context));
            if (!helper || !left_slot || !right_slot
                || LLVMGlobalGetValueType(helper) != signature) break;
            LLVMBuildStore(state->builder, left, left_slot);
            LLVMBuildStore(state->builder, right, right_slot);
            LLVMValueRef arguments[] = {left_slot, right_slot};
            LLVMValueRef equal = LLVMBuildCall2(state->builder, signature,
                helper, arguments, 2, "reference.equal");
            if (!equal) break;
            result = LLVMBuildICmp(state->builder,
                instruction->op == IR_OP_EQ ? LLVMIntNE : LLVMIntEQ,
                equal, LLVMConstNull(LLVMTypeOf(equal)), "reference.compare");
        } else if (string_compare && kind == LLVMStructTypeKind
            && LLVMCountStructElementTypes(LLVMTypeOf(left)) == 2) {
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef integer = LLVMInt64TypeInContext(context);
            LLVMTypeRef parameters[] = {integer, integer, integer, integer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMInt32TypeInContext(context), parameters, 4, false);
            bool equality = instruction->op == IR_OP_EQ
                || instruction->op == IR_OP_NE;
            const char *function_name = equality
                ? "n00bb_bootstrap_string_equal"
                : "n00bb_bootstrap_string_compare";
            LLVMValueRef callee = LLVMGetNamedFunction(state->module,
                function_name);
            if (!callee) callee = LLVMAddFunction(state->module,
                function_name, signature);
            if (!callee || LLVMGlobalGetValueType(callee) != signature) break;
            LLVMValueRef arguments[] = {
                LLVMBuildExtractValue(state->builder, left, 0, "string.left.low"),
                LLVMBuildExtractValue(state->builder, left, 1, "string.left.high"),
                LLVMBuildExtractValue(state->builder, right, 0, "string.right.low"),
                LLVMBuildExtractValue(state->builder, right, 1, "string.right.high"),
            };
            LLVMValueRef comparison = LLVMBuildCall2(state->builder,
                signature, callee, arguments, 4, "string.compare");
            LLVMIntPredicate predicate = instruction->op == IR_OP_EQ
                ? LLVMIntNE : instruction->op == IR_OP_NE
                    ? LLVMIntEQ : instruction->op == IR_OP_LT
                        ? LLVMIntSLT : instruction->op == IR_OP_LE
                            ? LLVMIntSLE : instruction->op == IR_OP_GT
                                ? LLVMIntSGT : LLVMIntSGE;
            LLVMValueRef zero = equality
                ? LLVMConstNull(LLVMTypeOf(comparison))
                : LLVMConstInt(LLVMTypeOf(comparison), 0, false);
            result = LLVMBuildICmp(state->builder, predicate, comparison,
                                   zero, "string.compare.bool");
        } else if (floating_kind(kind)) {
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
        if (LLVMGetTypeKind(payload_type) == LLVMVoidTypeKind) return true;
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
            const th_param *formal = th_func_param(setup_target->type, i);
            if (value && wanted && LLVMTypeOf(value) != wanted && formal)
                value = adapt_call_value(state, value, provided[i]->type,
                                         formal->type);
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
        LLVMTypeRef outcome = native_outcome_type(state->module,
            state->function->owner, callback_type, failure, context);
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
            || LLVMCountStructElementTypes(LLVMTypeOf(callback)) != 3) {
            state->failure_detail = "callback value is not a native callback record";
            break;
        }
        th_type *handler_type = ir_instr_callback_invoke_handler(instruction);
        if (!handler_type)
            handler_type = ir_instr_call_signature(instruction);
        LLVMTypeRef signature = native_function_type(state->module,
            state->function->owner, handler_type, false);
        unsigned parameter_count = signature
            ? LLVMCountParamTypes(signature) : 0;
        bool stateful = parameter_count == count;
        if (!signature || (parameter_count != count - 1 && !stateful)) {
            state->failure_detail = "callback handler signature is not native";
            break;
        }
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMValueRef index = LLVMBuildExtractValue(state->builder, callback,
                                                    0, "callback.index");
        LLVMValueRef callback_type_index = LLVMBuildExtractValue(
            state->builder, callback, 1, "callback.type.index");
        LLVMBasicBlockRef direct_done = nullptr;
        LLVMValueRef direct_result = nullptr;
        for (size_t i = 0; !stateful && state->callback_bindings
             && i < state->callback_bindings->len; i++) {
            const callback_binding_t *binding =
                &state->callback_bindings->data[i];
            uint32_t function_index = runtime_function_index(state,
                binding->handler);
            uint32_t type_index = runtime_type_index(state,
                binding->callback_type);
            LLVMTypeRef concrete = native_function_type(state->module,
                binding->handler->owner, binding->handler_signature, false);
            if (!function_index || !type_index || !concrete
                || concrete == signature
                || LLVMCountParamTypes(concrete) != count - 1)
                continue;
            core_buffer_t name = {0};
            core_buffer_printf(&name, "%s.%s",
                ir_module_name(binding->handler->owner),
                binding->handler->name);
            LLVMValueRef target = name.data
                ? LLVMGetNamedFunction(state->module, name.data) : nullptr;
            if (!target && name.data)
                target = LLVMAddFunction(state->module, name.data, concrete);
            core_buffer_release(&name);
            if (!target || LLVMGlobalGetValueType(target) != concrete) {
                state->failure_detail =
                    "callback direct target has a conflicting native signature";
                return unsupported(state, instruction);
            }
            if (!direct_done) {
                direct_done = LLVMAppendBasicBlockInContext(context,
                    state->target_function, "callback.done");
                if (instruction->dest) {
                    LLVMBasicBlockRef current = LLVMGetInsertBlock(
                        state->builder);
                    LLVMPositionBuilderAtEnd(state->builder, direct_done);
                    direct_result = LLVMBuildPhi(state->builder,
                        LLVMGetReturnType(signature), "callback.result");
                    LLVMPositionBuilderAtEnd(state->builder, current);
                }
            }
            LLVMValueRef selected = LLVMBuildAnd(state->builder,
                LLVMBuildICmp(state->builder, LLVMIntEQ, index,
                    LLVMConstInt(i32, function_index, false),
                    "callback.handler.match"),
                LLVMBuildICmp(state->builder, LLVMIntEQ,
                    callback_type_index,
                    LLVMConstInt(i32, type_index, false),
                    "callback.type.match"), "callback.match");
            LLVMBasicBlockRef yes = LLVMAppendBasicBlockInContext(context,
                state->target_function, "callback.concrete");
            LLVMBasicBlockRef no = LLVMAppendBasicBlockInContext(context,
                state->target_function, "callback.next");
            LLVMBuildCondBr(state->builder, selected, yes, no);
            LLVMPositionBuilderAtEnd(state->builder, yes);
            LLVMValueRef *direct_args = calloc(count - 1,
                sizeof(*direct_args));
            if (!direct_args) {
                state->failure_detail =
                    "could not allocate callback arguments";
                return unsupported(state, instruction);
            }
            bool adapted = true;
            for (uint32_t j = 1; adapted && j < count; j++) {
                const th_param *formal = th_func_param(
                    binding->handler_signature, j - 1);
                LLVMValueRef value = operands[j].kind == IR_OPD_VALUE
                    ? lowered_value(state, operands[j].value) : nullptr;
                if (value && formal)
                    value = adapt_call_value(state, value,
                        operands[j].value->type, formal->type);
                direct_args[j - 1] = value;
                adapted = value != nullptr;
            }
            LLVMValueRef called = adapted ? LLVMBuildCall2(state->builder,
                concrete, target, direct_args, count - 1,
                instruction->dest ? "callback.concrete.result" : "")
                : nullptr;
            free(direct_args);
            if (!called) {
                state->failure_detail =
                    "callback direct call arguments cannot be adapted";
                return unsupported(state, instruction);
            }
            if (instruction->dest) {
                called = adapt_call_result(state, called,
                    binding->handler_signature, handler_type);
                if (!called || LLVMTypeOf(called)
                    != LLVMGetReturnType(signature)) {
                    state->failure_detail =
                        "callback direct result cannot be adapted";
                    return unsupported(state, instruction);
                }
                LLVMBasicBlockRef end = LLVMGetInsertBlock(state->builder);
                LLVMBuildBr(state->builder, direct_done);
                LLVMAddIncoming(direct_result, &called, &end, 1);
            } else LLVMBuildBr(state->builder, direct_done);
            LLVMPositionBuilderAtEnd(state->builder, no);
        }
        LLVMTypeRef resolver_type = LLVMFunctionType(pointer, &i32, 1, false);
        LLVMValueRef resolver = LLVMGetNamedFunction(state->module,
            "n00b_abi_v1_runtime_function_address");
        if (!resolver) resolver = LLVMAddFunction(state->module,
            "n00b_abi_v1_runtime_function_address", resolver_type);
        if (!resolver || LLVMGlobalGetValueType(resolver) != resolver_type)
            break;
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
            const th_param *formal = th_func_param(handler_type, ordinal);
            if (arguments[ordinal] && formal
                && LLVMTypeOf(arguments[ordinal]) != expected[ordinal])
                arguments[ordinal] = adapt_call_value(state,
                    arguments[ordinal], operands[i].value->type,
                    formal->type);
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
        if (!valid || !result) {
            state->failure_detail = "callback arguments cannot match native handler";
            break;
        }
        if (direct_done) {
            LLVMBasicBlockRef fallback = LLVMGetInsertBlock(state->builder);
            LLVMBuildBr(state->builder, direct_done);
            if (direct_result)
                LLVMAddIncoming(direct_result, &result, &fallback, 1);
            LLVMPositionBuilderAtEnd(state->builder, direct_done);
            if (direct_result) result = direct_result;
        }
        if (instruction->dest) {
            th_type *call_signature = ir_instr_call_signature(instruction);
            result = adapt_call_result(state, result, handler_type,
                call_signature ? call_signature : handler_type);
            if (!result) break;
        }
        return instruction->dest ? save_result(state, instruction, result)
                                 : true;
    }
    case IR_OP_CALL: {
        if (!count || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        ir_func_t *target = operands[0].value->kind == IR_VAL_FUNC
            ? operands[0].value->owner : nullptr;
        ir_annot_t *native_call = target
            ? ir_annot_get(&target->annots, "n00bb.abi.native") : nullptr;
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s
            && !strcmp(native_call->s, "__n00bb_hash_string")) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef integer = LLVMInt64TypeInContext(context);
            LLVMTypeRef result_type = LLVMInt128TypeInContext(context);
            LLVMValueRef value = lowered_value(state, operands[1].value);
            if (value && LLVMTypeOf(value) == box_llvm_type(context))
                value = unbox_value(state, value, operands[1].value->type);
            if (!value || LLVMGetTypeKind(LLVMTypeOf(value))
                              != LLVMStructTypeKind
                || LLVMCountStructElementTypes(LLVMTypeOf(value)) != 2)
                break;
            LLVMValueRef result_slot = entry_alloca(state, result_type);
            if (!result_slot) break;
            LLVMTypeRef parameters[] = {integer, integer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 3, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_hash_string");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_hash_string", signature);
            if (!helper || LLVMGlobalGetValueType(helper) != signature)
                break;
            LLVMValueRef args[] = {
                LLVMBuildExtractValue(state->builder, value, 0,
                                      "hash.string.low"),
                LLVMBuildExtractValue(state->builder, value, 1,
                                      "hash.string.high"), result_slot};
            LLVMBuildCall2(state->builder, signature, helper, args, 3, "");
            result = LLVMBuildLoad2(state->builder, result_type, result_slot,
                                    "hash.string");
            return save_result(state, instruction, result);
        }
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s
            && !strcmp(native_call->s, "__n00bb_hash_scalar")) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box_type = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef result_type = LLVMInt128TypeInContext(context);
            ir_machine_t machine = ir_machine_of(state->function->owner,
                operands[1].value->type);
            bool known = th_is_closed(ir_module_types(state->function->owner),
                operands[1].value->type)
                && machine.width > 0 && machine.width <= 128
                && (machine.class == IR_CLASS_BOOL
                    || machine.class == IR_CLASS_SINT
                    || machine.class == IR_CLASS_UINT
                    || machine.class == IR_CLASS_FLOAT
                    || machine.class == IR_CLASS_PTR);
            if (known
                && (machine.class == IR_CLASS_SINT
                    || machine.class == IR_CLASS_UINT)) {
                LLVMValueRef number = lowered_value(state, operands[1].value);
                bool boxed = number && LLVMTypeOf(number) == box_type;
                if (boxed && machine.width <= 64)
                    number = LLVMBuildExtractValue(state->builder, number, 3,
                        "hash.integer.payload");
                if (number && LLVMGetTypeKind(LLVMTypeOf(number))
                    == LLVMIntegerTypeKind) {
                    if (boxed && machine.width < 64)
                        number = LLVMBuildTrunc(state->builder, number,
                            LLVMIntTypeInContext(context, machine.width),
                            "hash.integer.bits");
                    LLVMValueRef result = LLVMBuildIntCast2(state->builder,
                        number, result_type,
                        machine.class == IR_CLASS_SINT, "hash.integer");
                    return save_result(state, instruction, result);
                }
            }
            LLVMValueRef result_slot = entry_alloca(state, result_type);
            if (!result_slot) break;
            LLVMValueRef value = lowered_value(state, operands[1].value);
            if (value && LLVMTypeOf(value) != box_type)
                value = box_value(state, value, operands[1].value->type);
            if (!value || LLVMTypeOf(value) != box_type) break;
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef parameters[] = {i32,
                LLVMInt64TypeInContext(context), i32, i32, pointer};
            bool known_helper = known
                && machine.class != IR_CLASS_SINT
                && machine.class != IR_CLASS_UINT;
            if (!known_helper) parameters[2] = pointer;
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters,
                known_helper ? 5 : 3, false);
            const char *helper_name = known_helper
                ? "n00bb_bootstrap_hash_scalar_known"
                : "n00bb_bootstrap_hash_scalar";
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                helper_name);
            if (!helper) helper = LLVMAddFunction(state->module,
                helper_name, signature);
            if (!helper || LLVMGlobalGetValueType(helper) != signature)
                break;
            LLVMValueRef args[] = {
                LLVMBuildExtractValue(state->builder, value, 2, "hash.type"),
                LLVMBuildExtractValue(state->builder, value, 3,
                                      "hash.payload"),
                known_helper ? LLVMConstInt(i32, machine.width, false)
                    : result_slot,
                LLVMConstInt(i32, machine.class == IR_CLASS_FLOAT, false),
                result_slot};
            LLVMBuildCall2(state->builder, signature, helper, args,
                known_helper ? 5 : 3, "");
            result = LLVMBuildLoad2(state->builder, result_type, result_slot,
                                    "hash.scalar");
            return save_result(state, instruction, result);
        }
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s
            && !strcmp(native_call->s, "__n00bb_xxh3_128")) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef array_type = value_type(state->function->owner,
                operands[1].value->type, context);
            LLVMTypeRef result_type = LLVMInt128TypeInContext(context);
            LLVMValueRef array = lowered_value(state, operands[1].value);
            if (array && LLVMTypeOf(array) == box_llvm_type(context))
                array = unbox_value(state, array,
                                    operands[1].value->type);
            if (!array || !array_type || LLVMTypeOf(array) != array_type
                || LLVMGetTypeKind(array_type) != LLVMStructTypeKind
                || LLVMCountStructElementTypes(array_type) != 2) break;
            LLVMValueRef array_slot = entry_alloca(state, array_type);
            LLVMValueRef result_slot = entry_alloca(state, result_type);
            if (!array_slot || !result_slot) break;
            LLVMBuildStore(state->builder, array, array_slot);
            LLVMTypeRef parameters[] = {pointer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 2, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_xxh3_128_array");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_xxh3_128_array", signature);
            if (!helper || LLVMGlobalGetValueType(helper) != signature)
                break;
            LLVMValueRef args[] = {array_slot, result_slot};
            LLVMBuildCall2(state->builder, signature, helper, args, 2, "");
            result = LLVMBuildLoad2(state->builder, result_type, result_slot,
                                    "hash.xxh3_128");
            return save_result(state, instruction, result);
        }
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s
            && !strcmp(native_call->s, "__n00bb_array_clear")) {
            if (count != 3 || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMValueRef address = lowered_value(state, operands[1].value);
            LLVMValueRef index = lowered_value(state, operands[2].value);
            LLVMValueRef array_type_index = nullptr;
            if (address && LLVMTypeOf(address) == box) {
                LLVMValueRef reference_type_index = LLVMBuildExtractValue(
                    state->builder, address, 2, "array.clear.ref.type");
                array_type_index = reference_type_index
                    ? runtime_type_component(state, reference_type_index, 0)
                    : nullptr;
                address = boxed_reference_address(state, address);
            } else if (address) {
                th_type *referent = th_ind_referent(
                    operands[1].value->type);
                array_type_index = referent
                    ? runtime_type_value(state, referent) : nullptr;
            }
            if (!address || LLVMTypeOf(address) != pointer
                || !array_type_index || LLVMTypeOf(array_type_index) != i32
                || !index || LLVMTypeOf(index) != i64) break;
            LLVMTypeRef parameters[] = {pointer, i32, i64};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 3, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_array_clear_slot");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_array_clear_slot", signature);
            if (!helper || LLVMGlobalGetValueType(helper) != signature)
                break;
            LLVMValueRef args[] = {address, array_type_index, index};
            return LLVMBuildCall2(state->builder, signature, helper,
                                  args, 3, "") != nullptr;
        }
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s
            && !strcmp(native_call->s, "__n00bb_array_prefix")) {
            if (count != 3 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMValueRef source = lowered_value(state, operands[1].value);
            LLVMValueRef length = lowered_value(state, operands[2].value);
            if (!source || !length || LLVMTypeOf(length) != i64) break;
            if (LLVMTypeOf(source) != box)
                source = box_value(state, source, operands[1].value->type);
            LLVMValueRef source_slot = source ? entry_alloca(state, box)
                                                : nullptr;
            LLVMValueRef result_slot = source_slot
                ? entry_alloca(state, box) : nullptr;
            if (!result_slot) break;
            LLVMBuildStore(state->builder, source, source_slot);
            LLVMTypeRef allocator_params[] = {i32, i32, i64, i32, pointer};
            LLVMTypeRef allocator_type = LLVMFunctionType(pointer,
                allocator_params, 5, false);
            const char *allocator_name = ir_func_execution_phase(
                state->function) == IR_PHASE_COMPTIME
                ? "n00b_comptime_allocate" : "n00b_allocate";
            LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                          allocator_name);
            if (!allocator) allocator = LLVMAddFunction(state->module,
                allocator_name, allocator_type);
            LLVMTypeRef parameters[] = {pointer, i64, pointer, pointer};
            LLVMTypeRef signature = LLVMFunctionType(
                LLVMVoidTypeInContext(context), parameters, 4, false);
            LLVMValueRef helper = LLVMGetNamedFunction(state->module,
                "n00bb_bootstrap_array_prefix_box");
            if (!helper) helper = LLVMAddFunction(state->module,
                "n00bb_bootstrap_array_prefix_box", signature);
            if (!allocator || LLVMGlobalGetValueType(allocator)
                    != allocator_type || !helper
                || LLVMGlobalGetValueType(helper) != signature) break;
            LLVMValueRef args[] = {source_slot, length, allocator,
                                   result_slot};
            LLVMBuildCall2(state->builder, signature, helper, args, 4, "");
            result = LLVMBuildLoad2(state->builder, box, result_slot,
                                    "array.prefix.box");
            result = adapt_call_value(state, result,
                operands[1].value->type, instruction->dest->type);
            return result && save_result(state, instruction, result);
        }
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
            if (box && LLVMTypeOf(box) != box_llvm_type(context))
                box = box_value(state, box, operands[1].value->type);
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
            if (address) address = frame_root_pointer(state, address,
                operands[1].value->type);
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
                                   ir_instr_call_signature(instruction),
                                   false);
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
        LLVMValueRef *fresh_original = native_count
            ? calloc(native_count, sizeof(*fresh_original)) : nullptr;
        LLVMValueRef *fresh_boxed = native_count
            ? calloc(native_count, sizeof(*fresh_boxed)) : nullptr;
        th_type **fresh_types = native_count
            ? calloc(native_count, sizeof(*fresh_types)) : nullptr;
        if (native_count && (!arguments || !expected_types
            || !fresh_original || !fresh_boxed || !fresh_types)) {
            free(arguments);
            free(expected_types);
            free(fresh_original);
            free(fresh_boxed);
            free(fresh_types);
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
                LLVMTypeRef type = value_type(target ? target->owner
                    : state->function->owner,
                    formal->type, LLVMGetModuleContext(state->module));
                LLVMValueRef storage = nullptr;
                if (fresh) {
                    th_type *referent = operands[i].value
                        ? th_resolve(th_ind_referent(
                            operands[i].value->type)) : nullptr;
                    th_type *call_signature = ir_instr_call_signature(
                        instruction);
                    const th_param *bound = call_signature
                        ? th_func_param(call_signature, i - 1) : nullptr;
                    th_type *expected = bound ? bound->type : formal->type;
                    if (referent && type && !ir_type_eq(
                            state->function->owner, referent, expected)) {
                        core_buffer_t message = {0};
                        core_buffer_printf(&message,
                            "fresh construction target has type %s but '%s' requires %s",
                            th_print(ir_module_types(state->function->owner),
                                     referent),
                            target ? target->name : "indirect call",
                            th_print(ir_module_types(state->function->owner),
                                     expected));
                        backend_error(state->diagnostics, message.data);
                        core_buffer_release(&message);
                        return false;
                    }
                    storage = referent && type
                        && ir_type_eq(state->function->owner, referent,
                                      expected)
                        ? lowered_value(state, operands[i].value) : nullptr;
                    LLVMTypeRef concrete = storage ? value_type(
                        state->function->owner, referent,
                        LLVMGetModuleContext(state->module)) : nullptr;
                    if (storage && !concrete) {
                        core_buffer_t message = {0};
                        core_buffer_printf(&message,
                            "fresh construction type has no native representation: %s",
                            th_print(ir_module_types(
                                state->function->owner), referent));
                        backend_error(state->diagnostics, message.data);
                        core_buffer_release(&message);
                        return false;
                    }
                    bool formal_box = type == box_llvm_type(
                        LLVMGetModuleContext(state->module));
                    if (storage && formal_box
                        && LLVMTypeOf(storage) == type) {
                        LLVMContextRef context = LLVMGetModuleContext(
                            state->module);
                        LLVMTypeRef pointer = LLVMPointerTypeInContext(
                            context, 0);
                        LLVMTypeRef params[] = {pointer, pointer};
                        LLVMTypeRef signature = LLVMFunctionType(
                            LLVMVoidTypeInContext(context), params, 2,
                            false);
                        LLVMValueRef prepare = LLVMGetNamedFunction(
                            state->module,
                            "n00bb_bootstrap_fresh_ref_value_box");
                        if (!prepare) prepare = LLVMAddFunction(state->module,
                            "n00bb_bootstrap_fresh_ref_value_box",
                            signature);
                        LLVMValueRef reference = entry_alloca(state, type);
                        LLVMValueRef value_slot = entry_alloca(state, type);
                        if (!prepare || !reference || !value_slot
                            || LLVMGlobalGetValueType(prepare) != signature)
                            storage = nullptr;
                        else {
                            LLVMBuildStore(state->builder, storage,
                                           reference);
                            LLVMValueRef args[] = {reference, value_slot};
                            LLVMBuildCall2(state->builder, signature,
                                prepare, args, 2, "");
                            fresh_original[native_index] = reference;
                            fresh_boxed[native_index] = value_slot;
                            storage = value_slot;
                        }
                    } else if (storage && formal_box) {
                        if (concrete && concrete != type) {
                            LLVMValueRef boxed = box_fresh_storage(state,
                                storage, referent);
                            LLVMValueRef box_slot = boxed
                                ? entry_alloca(state, type) : nullptr;
                            if (box_slot) {
                                LLVMBuildStore(state->builder, boxed,
                                               box_slot);
                                fresh_original[native_index] = storage;
                                fresh_boxed[native_index] = box_slot;
                                fresh_types[native_index] = referent;
                                storage = box_slot;
                            } else storage = nullptr;
                        }
                    }
                } else if (type) storage = entry_alloca(state, type);
                if (storage && formal->mode == TH_INOUT && !fresh) {
                    LLVMValueRef input = lowered_value(state,
                                                       operands[i].value);
                    if (input && LLVMTypeOf(input) != type)
                        input = adapt_call_value(state, input,
                            operands[i].value->type, formal->type);
                    if (!input || LLVMTypeOf(input) != type) storage = nullptr;
                    else LLVMBuildStore(state->builder, input, storage);
                }
                valid = storage && save_call_output(state, instruction,
                    output_ordinal++, storage, formal->type);
                if (valid) arguments[native_index] = storage;
            } else {
                LLVMValueRef value = lowered_value(state, operands[i].value);
                if (optional) {
                    LLVMTypeRef expected = LLVMStructGetTypeAtIndex(
                        record_type, (unsigned)optional_index + 1);
                    if (value && LLVMTypeOf(value) != expected)
                        value = adapt_call_value(state, value,
                            operands[i].value->type, formal->type);
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
                    && LLVMTypeOf(arguments[native_index]) != expected)
                    arguments[native_index] = native
                        && expected == LLVMPointerTypeInContext(
                            LLVMGetModuleContext(state->module), 0)
                        && LLVMTypeOf(arguments[native_index])
                            == box_llvm_type(LLVMGetModuleContext(
                                state->module))
                        && th_kind_of(th_resolve(formal->type))
                            == TH_INDIRECTION
                        ? boxed_reference_address(state,
                            arguments[native_index])
                        : adapt_call_value(state,
                            arguments[native_index],
                            operands[i].value->type, formal->type);
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
        for (size_t i = 0; result && valid && i < native_count; i++) {
            if (!fresh_boxed[i]) continue;
            if (!fresh_types[i]) {
                LLVMContextRef context = LLVMGetModuleContext(state->module);
                LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
                LLVMTypeRef params[] = {pointer, pointer};
                LLVMTypeRef signature = LLVMFunctionType(
                    LLVMVoidTypeInContext(context), params, 2, false);
                LLVMValueRef write = LLVMGetNamedFunction(state->module,
                    "n00bb_bootstrap_ref_write_box");
                if (!write) write = LLVMAddFunction(state->module,
                    "n00bb_bootstrap_ref_write_box", signature);
                LLVMValueRef args[] = {fresh_original[i], fresh_boxed[i]};
                valid = write && LLVMGlobalGetValueType(write) == signature
                    && LLVMBuildCall2(state->builder, signature, write,
                        args, 2, "") != nullptr;
                continue;
            }
            LLVMValueRef boxed = LLVMBuildLoad2(state->builder,
                box_llvm_type(LLVMGetModuleContext(state->module)),
                fresh_boxed[i], "fresh.box.result");
            LLVMValueRef concrete = boxed
                ? unbox_value(state, boxed, fresh_types[i]) : nullptr;
            valid = concrete && LLVMBuildStore(state->builder, concrete,
                                                fresh_original[i]);
        }
        free(arguments);
        free(expected_types);
        free(fresh_original);
        free(fresh_boxed);
        free(fresh_types);
        if (!valid || !result) break;
        if (instruction->dest && !returns_void) {
            th_type *call_signature = ir_instr_call_signature(instruction);
            result = adapt_call_result(state, result, formal_signature,
                call_signature ? call_signature : formal_signature);
            if (!result) {
                state->failure_detail =
                    "callee result cannot be adapted to the call-site type";
                break;
            }
        }
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
        for (size_t i = 0; i < state->call_outputs.len; i++) {
            lowered_call_output_t *output = &state->call_outputs.data[i];
            if (output->call != call
                || output->ordinal != operands[1].lit.i.mag) continue;
            LLVMTypeRef type = value_type(state->function->owner,
                output->type, LLVMGetModuleContext(state->module));
            if (!type) break;
            result = LLVMBuildLoad2(state->builder, type, output->storage,
                                    "copyout");
            result = adapt_call_value(state, result, output->type,
                                      instruction->dest->type);
            if (!result) break;
            return save_result(state, instruction, result);
        }
        break;
    }
    case IR_OP_SELECT: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || operands[2].kind != IR_OPD_VALUE) break;
        LLVMValueRef condition = lowered_value(state, operands[0].value);
        LLVMValueRef yes = lowered_value(state, operands[1].value);
        LLVMValueRef no = lowered_value(state, operands[2].value);
        if (!condition || !yes || !no
            || LLVMTypeOf(yes) != LLVMTypeOf(no)) break;
        return save_result(state, instruction,
            LLVMBuildSelect(state->builder, condition, yes, no, "select"));
    }
    case IR_OP_BRANCH: {
        if (count != 3 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_BLOCK
            || operands[2].kind != IR_OPD_BLOCK) break;
        int fixed_arm = static_type_test_branch(state, instruction);
        if (fixed_arm >= 0) {
            LLVMBasicBlockRef target = lowered_block(state,
                operands[fixed_arm ? 1 : 2].block);
            if (!target) break;
            LLVMBuildBr(state->builder, target);
            return true;
        }
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
            LLVMValueRef output = LLVMGetParam(state->target_function,
                                                (unsigned)i);
            if (!(formal->mode == TH_INOUT
                ? store_gc_barrier_if_changed(state, output, value,
                    formal->type)
                : store_gc_barrier(state, value, formal->type))) {
                outputs_valid = false;
                break;
            }
            LLVMBuildStore(state->builder, value, output);
        }
        if (!outputs_valid || next_output != count) break;
        if (has_value) {
            if (operands[0].kind != IR_OPD_VALUE) break;
            result = lowered_value(state, operands[0].value);
            if (!result) break;
            if (th_kind_of(th_resolve(success)) == TH_SEQUENCE
                && !return_value_transfers_storage(state->function,
                                                    operands[0].value)) {
                result = copy_sequence_value(state, result, success);
                if (!result) break;
            }
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
        if (result && th_kind_of(th_resolve(
                th_func_error(state->function->type))) == TH_SEQUENCE
            && !return_value_transfers_storage(state->function,
                                                operands[0].value))
            result = copy_sequence_value(state, result,
                th_func_error(state->function->type));
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
        {
            const char *path = instruction->span.file >= 0
                ? ir_file_path(state->function->owner, instruction->span.file)
                : nullptr;
            core_buffer_t reason = {0};
            if (path)
                core_buffer_printf(&reason, "check failed at %s:%d:%d", path,
                    instruction->span.start_line, instruction->span.start_col);
            else core_buffer_printf(&reason, "check failed");
            emit_trap_reason(state, reason.data);
            core_buffer_release(&reason);
        }
        return true;
    default:
        break;
    }
    return unsupported(state, instruction);
}

static LLVMTypeRef
native_function_type(LLVMModuleRef target, ir_module_t *owner,
                     th_type *signature, bool native_binding)
{
    if (!owner || !signature
        || th_kind_of(th_resolve(signature)) != TH_FUNCTION) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(target, owner, signature,
                                            context);
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
        if (native_binding && parameter
            && th_kind_of(th_resolve(parameter->type)) == TH_INDIRECTION
            && !th_is_closed(ir_module_types(owner), parameter->type))
            value = LLVMPointerTypeInContext(context, 0);
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
        return native_function_type(target, function->owner, function->type,
            ir_annot_get(&function->annots, "n00bb.abi.native") != nullptr);
    LLVMTypeRef record = optional_record_type(target, function);
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(target, function->owner,
                                            function->type, context);
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
        "sys.internal.gc.alloc.mutator_for_native");
    if (!getter) getter = LLVMAddFunction(target,
        "sys.internal.gc.alloc.mutator_for_native", getter_type);
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
                      ir_value_t *value, ir_local_t *local)
{
    return (kind == N00BB_FRAME_ROOT_PARAM
            || kind == N00BB_FRAME_ROOT_INSTR
            || kind == N00BB_FRAME_ROOT_LOCAL)
        && (value || local)
        && n00bb_frame_native_pointer(state->function->owner,
                                      value ? value->type : local->type)
        && !(kind == N00BB_FRAME_ROOT_INSTR && value->def
             && value->def->op == IR_OP_OPTION_NONE)
        && (!value || !ir_storage_origin_unmanaged(state->storage_origin,
                                                  value))
        && (kind != N00BB_FRAME_ROOT_INSTR
            || frame_stable_root_source(state, value) == value);
}

typedef struct {
    lower_state_t *state;
    size_t count;
    bool possible;
} frame_root_count_t;

static bool
frame_root_unreachable_narrow(lower_state_t *state,
                              n00bb_frame_root_kind_t kind,
                              ir_value_t *value, ir_instr_t *instruction)
{
    if (kind != N00BB_FRAME_ROOT_INSTR || !instruction
        || instruction->op != IR_OP_TYPE_NARROW || instruction->dest != value
        || instruction->n_operands != 2
        || instruction->operands[0].kind != IR_OPD_VALUE
        || instruction->operands[1].kind != IR_OPD_TYPE
        || !instruction->operands[0].value) return false;
    th_type *source = instruction->operands[0].value->type;
    th_type *target = instruction->operands[1].type;
    th_ctx *types = ir_module_types(state->function->owner);
    if (!source || !target || !th_is_closed(types, source)
        || !th_is_closed(types, target)) return false;
    ir_class_t category = ir_machine_of(state->function->owner, source).class;
    if (category != IR_CLASS_BOOL && category != IR_CLASS_SINT
        && category != IR_CLASS_UINT && category != IR_CLASS_FLOAT)
        return false;
    uint32_t source_index = runtime_type_index(state, source);
    uint32_t target_index = runtime_type_index(state, target);
    return source_index && target_index && source_index != target_index;
}

static bool
count_frame_root(void *opaque, n00bb_frame_root_kind_t kind,
                 size_t index, ir_value_t *value, ir_local_t *local,
                 ir_instr_t *instruction)
{
    (void)index;
    frame_root_count_t *count = opaque;
    // Specialized trait dispatch can leave impossible typecase arms in IR.
    if (frame_root_unreachable_narrow(count->state, kind, value, instruction))
        return true;
    if ((kind != N00BB_FRAME_ROOT_PARAM
         && kind != N00BB_FRAME_ROOT_INSTR
         && kind != N00BB_FRAME_ROOT_LOCAL)
        || (!value && !local)
        || !n00bb_frame_native_pointer(count->state->function->owner,
                                       value ? value->type : local->type))
        count->possible = false;
    else if (frame_root_needs_slot(count->state, kind, value, local))
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
    if (!frame_root_needs_slot(state, kind, value, local)) return true;
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
    if (!core_indexed_table_reserve(state->frame_roots, (uint32_t)index,
                                    value ? (const void *)value
                                          : (const void *)local, nullptr))
        return false;
    state->frame_slots[index] = slot;
    if (kind == N00BB_FRAME_ROOT_PARAM) {
        LLVMValueRef parameter = lowered_value(state, value);
        LLVMValueRef root = parameter
            ? frame_root_pointer(state, parameter, value->type) : nullptr;
        if (!root || LLVMTypeOf(root) != pointer) return false;
        LLVMValueRef write = LLVMBuildStore(state->builder, root, slot);
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
            && category != IR_CLASS_UINT && category != IR_CLASS_FLOAT
            && !n00bb_frame_pointer_free_value(state->function->owner,
                                               local->type)
            && !n00bb_frame_native_pointer(state->function->owner,
                                           local->type))
            return mark_conservative_frame(state);
    }
    if (!n00bb_frame_roots(state->function, count_frame_root,
                           &selected, &total)) return false;
    if (!selected.possible) return mark_conservative_frame(state);
    if (!selected.count) return true;
    size_t count = selected.count;
    if (count > UINT32_MAX - 2) return false;
    state->frame_roots = core_indexed_table_new((uint32_t)count, UINT32_MAX,
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
prepare_parameter_addresses(lower_state_t *state)
{
    ir_func_t *function = state->function;
    for (uint32_t block = 0; block < function->n_blocks; block++) {
        ir_block_t *body = function->blocks[block];
        for (uint32_t i = 0; i < body->n_instrs; i++) {
            ir_instr_t *instruction = body->instrs[i];
            if (instruction->op != IR_OP_ADDRESS_OF
                || instruction->n_operands != 1
                || instruction->operands[0].kind != IR_OPD_VALUE) continue;
            ir_value_t *value = instruction->operands[0].value;
            if (!value || value->kind != IR_VAL_PARAM
                || value->owner != function || value->index >= function->n_params)
                continue;
            if (state->param_addresses[value->index]) continue;
            size_t input_index = 0, native_index = 0;
            const th_param *formal = nullptr;
            size_t parameter_count = th_func_nparams(function->type);
            for (size_t p = 0; p < parameter_count; p++) {
                const th_param *candidate = th_func_param(function->type, p);
                bool optional = ir_func_parameter_is_optional(function, p);
                if (candidate && th_mode_accepts_input(candidate->mode)
                    && input_index++ == value->index) {
                    if (optional) return backend_error(state->diagnostics,
                        "address of an optional parameter is not lowered yet");
                    formal = candidate;
                    break;
                }
                if (!optional) native_index++;
            }
            if (!formal || native_index >= LLVMCountParams(state->target_function))
                return false;
            LLVMValueRef native = LLVMGetParam(state->target_function,
                                               (unsigned)native_index);
            if (th_mode_produces_output(formal->mode)) {
                state->param_addresses[value->index] = native;
                continue;
            }
            LLVMTypeRef type = value_type(function->owner, value->type,
                LLVMGetModuleContext(state->module));
            if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind
                || LLVMTypeOf(native) != type) return false;
            LLVMValueRef slot = LLVMBuildAlloca(state->builder, type,
                                                "parameter.address");
            if (!slot || !LLVMBuildStore(state->builder, native, slot))
                return false;
            state->param_addresses[value->index] = slot;
        }
    }
    return true;
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
               const core_list_t(callback_binding_t) *callback_bindings,
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
        .target_function = target_function, .with_mutator = with_mutator,
        .diagnostics = diagnostics,
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
        .callback_bindings = callback_bindings,
        .layout_matches = layout_matches,
        .member_matches = member_matches};
    state.index_proofs = ir_index_proofs_new(function);
    state.blocks = calloc(function->n_blocks, sizeof(*state.blocks));
    state.block_tails = calloc(function->n_blocks,
                               sizeof(*state.block_tails));
    state.locals = calloc(function->n_locals ? function->n_locals : 1,
        sizeof(*state.locals));
    state.param_addresses = calloc(function->n_params ? function->n_params : 1,
        sizeof(*state.param_addresses));
    for (uint32_t i = 0; i < function->n_blocks; i++) {
        state.value_capacity += function->blocks[i]->n_instrs;
    }
    if (with_mutator)
        state.mutator = LLVMGetParam(target_function,
            (unsigned)native_count);
    state.values = calloc(state.value_capacity ? state.value_capacity : 1,
        sizeof(*state.values));
    state.builder = LLVMCreateBuilderInContext(context);
    uint32_t *block_order = calloc(function->n_blocks,
                                   sizeof(*block_order));
    bool *feasible_blocks = calloc(function->n_blocks,
                                   sizeof(*feasible_blocks));
    uint32_t reachable_blocks = 0;
    bool ordered = block_order && feasible_blocks
        && feasible_block_order(&state, block_order, feasible_blocks,
                                &reachable_blocks);
    bool ok = state.blocks && state.block_tails && state.locals
        && state.param_addresses
        && state.values && state.builder && ordered;
    if (ok) {
        for (uint32_t i = 0; i < function->n_blocks; i++)
            state.blocks[i] = LLVMAppendBasicBlockInContext(context,
                target_function, function->blocks[i]->name);
        LLVMPositionBuilderAtEnd(state.builder, state.blocks[0]);
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
        if (ok && emit_frame_maps && !prepare_frame_map(&state))
            ok = backend_error(diagnostics,
                "LLVM backend could not prepare a function frame map");
        if (ok && !prepare_parameter_addresses(&state))
            ok = backend_error(diagnostics,
                "LLVM backend could not prepare parameter addresses");
    }
    for (uint32_t at = 0; ok && at < reachable_blocks; at++) {
        uint32_t i = block_order[at];
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
                if (state.failure_detail)
                    core_buffer_printf(&message, ": %s",
                        state.failure_detail);
                backend_error(diagnostics, message.data);
                core_buffer_release(&message);
            }
        }
        state.block_tails[i] = LLVMGetInsertBlock(state.builder);
    }
    for (uint32_t at = reachable_blocks; ok && at < function->n_blocks; at++) {
        uint32_t i = block_order[at];
        LLVMPositionBuilderAtEnd(state.builder, state.blocks[i]);
        LLVMBuildUnreachable(state.builder);
        state.block_tails[i] = state.blocks[i];
    }
    for (uint32_t i = 0; ok && i < function->n_blocks; i++) {
        if (!feasible_blocks[i]) continue;
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
                if (!feasible_edge(&state, operand->phi.block, block,
                                   feasible_blocks)) continue;
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
    free(block_order);
    free(feasible_blocks);
    free(state.locals);
    free(state.param_addresses);
    free(state.values);
    core_list_free(state.call_outputs);
    ir_index_proofs_free(state.index_proofs);
    core_indexed_table_free(state.frame_roots);
    free(state.frame_slots);
    if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics))) {
        core_buffer_t message = {0};
        core_buffer_printf(&message, "could not lower LLVM function '%s.%s'",
            ir_module_name(function->owner), function->name);
        backend_error(diagnostics, message.data);
        core_buffer_release(&message);
    }
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
        if (ir_func_execution_phase(function) != IR_PHASE_COMPTIME
            && has_dynamic_allocation(function))
            core_list_push(*selected, function);
    }
    bool changed;
    do {
        changed = false;
        for (size_t f = 0; f < candidates->len; f++) {
            ir_func_t *function = candidates->data[f];
            if (ir_func_execution_phase(function) == IR_PHASE_COMPTIME
                || inline_list_contains(selected, function)) continue;
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
             const core_list_t(callback_binding_t) *callback_bindings,
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
                     && machine.class == IR_CLASS_FLOAT && floating_type(type))
                value = LLVMConstReal(type, init->floating);
            else if (init->kind == IR_GLOBAL_INIT_STRING
                     && is_string_type(global->type))
                value = string_value(target,
                    (const uint8_t *)init->string.bytes, init->string.length,
                    default_string_encoding(init->string.bytes,
                                            init->string.length));
            else if (init->kind == IR_GLOBAL_INIT_RECORD) {
                ir_annot_t *result_id = ir_annot_get(&global->annots,
                    "n00bb.comptime.global_result");
                lower_state_t cache = {
                    .module = target,
                    .phase_arrays = phase_arrays,
                    .phase_array_count = phase_array_count,
                };
                if (result_id && result_id->kind == IR_ANNOT_STR)
                    value = cached_record_array_value(&cache,
                    (ir_module_t *)source, global->type,
                    result_id->s);
            }
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
                "LLVM backend cannot lower signature for '%s.%s': %s",
                ir_module_name(source), function->name,
                th_print(ir_module_types((ir_module_t *)source),
                    function->type));
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
            if (strcmp(ir_module_name(function->owner), "sys.internal.gc.alloc")
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
                               callback_bindings,
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
                            callback_bindings,
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
    core_dict_init(&backend->native_types, native_type_key_hash,
                   native_type_key_eq);
    backend->native_types_ready = true;
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
    if (backend->native_types_ready) {
        core_dict_foreach(&backend->native_types, item)
            core_free(item->key);
        core_dict_free(&backend->native_types);
    }
    core_list_free(backend->callback_bindings);
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
        core_list_free(backend->callback_bindings);
        backend->callback_bindings = core_list_new(callback_binding_t);
        for (size_t i = 0; i < count; i++) {
            ir_func_t *function = functions[i];
            for (uint32_t b = 0; function && b < function->n_blocks; b++) {
                ir_block_t *block = function->blocks[b];
                for (uint32_t j = 0; block && j < block->n_instrs; j++) {
                    ir_instr_t *creation = block->instrs[j];
                    if (!creation || creation->op != IR_OP_CALLBACK_CREATE
                        || !creation->dest || !creation->n_operands
                        || creation->operands[0].kind != IR_OPD_VALUE)
                        continue;
                    ir_value_t *reference = creation->operands[0].value;
                    const ir_instr_t *retained = reference
                        ? reference->def : nullptr;
                    ir_value_t *handler = retained
                        && retained->op == IR_OP_RETAIN_REF
                        && retained->n_operands == 1
                        && retained->operands[0].kind == IR_OPD_VALUE
                        ? retained->operands[0].value : nullptr;
                    th_type *signature = nullptr;
                    if (!handler || handler->kind != IR_VAL_FUNC
                        || !ir_instr_callback_contract(creation, nullptr,
                            &signature, nullptr)) continue;
                    core_list_push(backend->callback_bindings,
                        ((callback_binding_t){handler->owner,
                            creation->dest->type, signature}));
                }
            }
        }
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

void
n00bb_llvm_backend_bind_native_cache(n00bb_llvm_backend_t *backend,
    n00bb_native_cache_t *cache)
{
    if (backend) backend->native_cache = cache;
}

static bool
lower_phase(n00bb_llvm_backend_t *backend, bool comptime,
            core_diagnostics_t *diagnostics)
{
    bool *lowered = comptime ? &backend->comptime_lowered
                             : &backend->runtime_lowered;
    if (*lowered) return true;
    core_dict_t *previous_native_types = active_native_types;
    active_native_types = &backend->native_types;
    bool module_lowered = lower_module(
                      comptime ? backend->comptime : backend->runtime,
                      comptime ? backend->comptime_source
                               : backend->runtime_source,
                      !comptime && backend->emit_frame_maps,
                      comptime ? nullptr : backend->phase_arrays,
                      comptime ? 0 : backend->phase_array_count,
                      backend->type_indices,
                      backend->type_index_count,
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
                      comptime ? nullptr : &backend->callback_bindings,
                      diagnostics);
    active_native_types = previous_native_types;
    if (!module_lowered) return false;
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
    const char *failed_function = nullptr;
    size_t failed_function_length = 0;
    const char *failed_step = "unknown step";
    for (LLVMValueRef function = LLVMGetFirstFunction(module);
         ok && function; function = LLVMGetNextFunction(function)) {
        failed_function = LLVMGetValueName2(function,
                                             &failed_function_length);
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
            if (!ok) failed_step = "discarding conservative roots";
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
            if (!ok) failed_step = "discarding transformed roots";
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
            failed_step = "finding entry root slots";
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
            failed_step = "initializing entry root slots";
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
    if (ok) return true;
    core_buffer_t message = {0};
    core_buffer_printf(&message,
        "could not emit native frame sentinels in %.*s (%s)",
        failed_function ? (int)failed_function_length
                        : (int)sizeof("unknown function") - 1,
        failed_function ? failed_function : "unknown function", failed_step);
    bool reported = backend_error(diagnostics, message.data);
    core_buffer_release(&message);
    return reported;
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

static bool emit_native_module(n00bb_llvm_backend_t *backend,
    LLVMModuleRef module, bool comptime, core_buffer_t *out,
    core_diagnostics_t *diagnostics);

static bool
emit_phase_image(n00bb_llvm_backend_t *backend,
                               bool comptime,
                               const char *const *entry_initializers,
                               size_t initializer_count,
                               const n00bb_llvm_array_producer_t *array_producers,
                               size_t array_producer_count,
                               size_t metadata_bytes, bool bitcode,
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
        if (metadata_bytes) {
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
            LLVMValueRef failed = LLVMBuildICmp(builder, LLVMIntNE, status,
                LLVMConstInt(result, 0, false), "registration.failed");
            if (!comptime) {
                LLVMValueRef bind = LLVMAddFunction(module,
                    "n00b_abi_v1_bind_runtime_function_addresses", bind_type);
                LLVMValueRef bind_args[] = {function_addresses,
                    LLVMConstInt(result, function_address_count, false)};
                LLVMValueRef bound = LLVMBuildCall2(builder, bind_type, bind,
                    bind_args, 2, "function.addresses");
                LLVMValueRef bind_failed = LLVMBuildICmp(builder, LLVMIntNE,
                    bound, LLVMConstNull(result), "function.addresses.failed");
                failed = LLVMBuildOr(builder, failed, bind_failed,
                                     "initialization.failed");
            }
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
            LLVMTypeRef record_writer_parameters[] = {
                result, pointer, result, result, pointer, pointer, result,
                pointer, result, pointer,
            };
            LLVMTypeRef record_writer_type = LLVMFunctionType(result,
                record_writer_parameters, 10, false);
            LLVMValueRef record_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_record_array", record_writer_type);
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
            LLVMValueRef graph_writer = LLVMAddFunction(module,
                "n00bb_comptime_emit_graph", value_writer_type);
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
                bool record_array = spec->kind
                    == N00BB_LLVM_LITERAL_RECORD_ARRAY
                    || spec->kind == N00BB_LLVM_LITERAL_RECORD_RELOC;
                bool record_fields = spec->kind
                    == N00BB_LLVM_LITERAL_RECORD_FIELDS_ARRAY
                    || spec->kind == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC;
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
                        || spec->kind == N00BB_LLVM_LITERAL_GRAPH
                        || spec->kind == N00BB_LLVM_LITERAL_RELOC
                        || record_array || record_fields)
                        && spec->function
                        ? value_type(spec->function->owner,
                            producer_result, context)
                        : nullptr;
                LLVMValueRef producer = spec->symbol
                    ? LLVMGetNamedFunction(module, spec->symbol) : nullptr;
                if (!producer && spec->symbol && expected)
                    producer = LLVMAddFunction(module, spec->symbol,
                        LLVMFunctionType(expected, nullptr, 0, false));
                if (!producer || ((spec->kind == N00BB_LLVM_LITERAL_ARRAY
                        || spec->kind == N00BB_LLVM_LITERAL_RECORD_ARRAY)
                        && !spec->element_stride)
                    || ((spec->kind == N00BB_LLVM_LITERAL_RELOC
                            || spec->kind == N00BB_LLVM_LITERAL_RECORD_RELOC)
                        && (!spec->element_stride || !spec->string_offsets
                            || !spec->byte_mask
                            || !spec->string_offset_count
                            || spec->string_offset_count > UINT32_MAX
                            || (!reloc_array && !record_array && (!expected
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
                    || ((spec->kind == N00BB_LLVM_LITERAL_RAW_VALUE
                            || spec->kind == N00BB_LLVM_LITERAL_GRAPH)
                        && (!spec->element_stride || !expected
                            || LLVMABISizeOfType(
                                LLVMGetModuleDataLayout(module), expected)
                                != spec->element_stride))
                    || (record_fields
                        && (!expected || !spec->record_size
                            || spec->array_field_offset > spec->record_size
                            || spec->record_size
                                - spec->array_field_offset
                                < sizeof(n00b_abi_array_value_v1)
                            || LLVMGetTypeKind(expected)
                                != LLVMStructTypeKind
                            || spec->array_field_index
                                >= LLVMCountStructElementTypes(expected)
                            || LLVMStructGetTypeAtIndex(expected,
                                spec->array_field_index) != array_type
                            || LLVMABISizeOfType(
                                LLVMGetModuleDataLayout(module), expected)
                                != spec->record_size))
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
                } else if (spec->kind == N00BB_LLVM_LITERAL_GRAPH) {
                    uint64_t typehash = th_hash(ir_module_types(
                        spec->function->owner), producer_result);
                    uint32_t type_index = 0;
                    for (size_t t = 0; t < backend->type_index_count; t++)
                        if (backend->type_indices[t].typehash == typehash) {
                            type_index = backend->type_indices[t].index;
                            break;
                        }
                    if (!type_index) {
                        LLVMDisposeBuilder(builder);
                        return backend_error(diagnostics,
                            "compile-time literal type has no ABI index");
                    }
                    LLVMValueRef storage = LLVMBuildAlloca(builder, expected,
                        "literal.graph.storage");
                    LLVMBuildStore(builder, value, storage);
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD, false),
                        storage,
                        LLVMConstInt(result, type_index, false),
                    };
                    status = LLVMBuildCall2(builder, value_writer_type,
                        graph_writer, arguments, 3, "literal.write");
                } else if (record_fields) {
                    bool reloc = spec->kind
                        == N00BB_LLVM_LITERAL_RECORD_FIELDS_RELOC;
                    LLVMValueRef offsets_data = LLVMConstNull(pointer);
                    LLVMValueRef mask_data = LLVMConstNull(pointer);
                    if (reloc) {
                        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                        LLVMValueRef *offsets = calloc(
                            spec->string_offset_count, sizeof(*offsets));
                        if (!offsets || !spec->string_offsets
                            || !spec->byte_mask) {
                            free(offsets);
                            LLVMDisposeBuilder(builder);
                            return backend_error(diagnostics,
                                "record literal relocations are unavailable");
                        }
                        for (size_t f = 0; f < spec->string_offset_count; f++)
                            offsets[f] = LLVMConstInt(i32,
                                spec->string_offsets[f], false);
                        LLVMValueRef offset_values = LLVMConstArray(i32,
                            offsets, (unsigned)spec->string_offset_count);
                        free(offsets);
                        offsets_data = LLVMAddGlobal(module,
                            LLVMTypeOf(offset_values),
                            "n00bb.literal.record.offsets");
                        LLVMSetInitializer(offsets_data, offset_values);
                        LLVMSetGlobalConstant(offsets_data, true);
                        LLVMSetLinkage(offsets_data, LLVMPrivateLinkage);
                        LLVMValueRef mask_values = LLVMConstStringInContext(
                            context, (const char *)spec->byte_mask,
                            spec->element_stride, true);
                        mask_data = LLVMAddGlobal(module,
                            LLVMTypeOf(mask_values),
                            "n00bb.literal.record.mask");
                        LLVMSetInitializer(mask_data, mask_values);
                        LLVMSetGlobalConstant(mask_data, true);
                        LLVMSetLinkage(mask_data, LLVMPrivateLinkage);
                    }
                    LLVMValueRef storage = LLVMBuildAlloca(builder, expected,
                        "literal.record.storage");
                    LLVMBuildStore(builder, value, storage);
                    LLVMValueRef array = LLVMBuildExtractValue(builder, value,
                        spec->array_field_index, "literal.record.array");
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        storage,
                        LLVMConstInt(result, spec->record_size, false),
                        LLVMConstInt(result, spec->array_field_offset,
                                     false),
                        LLVMBuildExtractValue(builder, array, 0,
                                              "array.data"),
                        LLVMBuildExtractValue(builder, array, 1,
                                              "array.descriptor"),
                        LLVMConstInt(result, spec->element_stride, false),
                        offsets_data,
                        LLVMConstInt(result, spec->string_offset_count,
                                     false),
                        mask_data,
                    };
                    status = LLVMBuildCall2(builder, record_writer_type,
                        record_writer, arguments, 10, "literal.write");
                } else if (spec->kind == N00BB_LLVM_LITERAL_RELOC
                    || spec->kind == N00BB_LLVM_LITERAL_RECORD_RELOC) {
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
                    if (reloc_array || record_array) {
                        LLVMValueRef array = record_array
                            ? LLVMBuildExtractValue(builder, value, 0,
                                "literal.record.array") : value;
                        LLVMValueRef arguments[] = {
                            LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                         false),
                            LLVMBuildExtractValue(builder, array, 0,
                                                  "array.data"),
                            LLVMBuildExtractValue(builder, array, 1,
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
                    LLVMValueRef array = record_array
                        ? LLVMBuildExtractValue(builder, value, 0,
                            "literal.record.array") : value;
                    LLVMValueRef arguments[] = {
                        LLVMConstInt(result, N00BB_COMPTIME_RESULT_FD,
                                     false),
                        LLVMBuildExtractValue(builder, array, 0,
                                              "array.data"),
                        LLVMBuildExtractValue(builder, array, 1,
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
    if (bitcode) {
        LLVMMemoryBufferRef serialized = LLVMWriteBitcodeToMemoryBuffer(module);
        if (!serialized)
            return backend_error(diagnostics, "could not serialize linkable LLVM bitcode");
        core_buffer_append(out, LLVMGetBufferStart(serialized),
                           LLVMGetBufferSize(serialized));
        LLVMDisposeMemoryBuffer(serialized);
        return true;
    }
    return emit_native_module(backend, module, comptime, out, diagnostics);
}

static bool
emit_code_map(LLVMModuleRef module, core_diagnostics_t *diagnostics)
{
    const char *triple = LLVMGetTarget(module);
    bool mach_o = triple && (strstr(triple, "apple") || strstr(triple, "darwin"));
    bool elf = triple && (strstr(triple, "linux") || strstr(triple, "elf")
        || strstr(triple, "bsd"));
    if (!mach_o && !elf) return true;
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef row_fields[] = {pointer, pointer, i64};
    LLVMTypeRef row_type = LLVMStructTypeInContext(context, row_fields, 3,
                                                   false);
    core_list_t(LLVMValueRef) rows = core_list_new(LLVMValueRef);
    for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
         function = LLVMGetNextFunction(function)) {
        if (!LLVMCountBasicBlocks(function)
            || LLVMGetLinkage(function) == LLVMAvailableExternallyLinkage)
            continue;
        size_t length = 0;
        const char *name = LLVMGetValueName2(function, &length);
        if (!name || !length) continue;
        LLVMValueRef text = LLVMConstStringInContext2(context, name, length,
                                                      true);
        LLVMValueRef label = LLVMAddGlobal(module, LLVMTypeOf(text),
                                           "__n00bb_code_name");
        LLVMSetInitializer(label, text);
        LLVMSetLinkage(label, LLVMPrivateLinkage);
        LLVMSetGlobalConstant(label, true);
        LLVMSetUnnamedAddress(label, LLVMGlobalUnnamedAddr);
        LLVMValueRef fields[] = {function, label,
                                 LLVMConstInt(i64, length, false)};
        core_list_push(rows, LLVMConstNamedStruct(row_type, fields, 3));
    }
    bool ok = true;
    if (rows.len) {
        LLVMValueRef table_value = LLVMConstArray2(row_type, rows.data,
                                                   rows.len);
        LLVMValueRef table = LLVMAddGlobal(module, LLVMTypeOf(table_value),
                                           "__n00bb_code_map");
        LLVMSetInitializer(table, table_value);
        LLVMSetLinkage(table, LLVMPrivateLinkage);
        LLVMSetGlobalConstant(table, true);
        LLVMSetAlignment(table, 8);
        LLVMSetSection(table, mach_o ? "__DATA,__n00b_code" : "n00b_code");
        LLVMValueRef used_value = LLVMConstArray2(pointer, &table, 1);
        LLVMValueRef used = LLVMAddGlobal(module, LLVMTypeOf(used_value),
                                          "llvm.used");
        LLVMSetInitializer(used, used_value);
        LLVMSetLinkage(used, LLVMAppendingLinkage);
        LLVMSetSection(used, "llvm.metadata");
        char *message = nullptr;
        ok = !LLVMVerifyModule(module, LLVMReturnStatusAction, &message);
        if (!ok) backend_error(diagnostics, message ? message
            : "invalid LLVM code map");
        if (message) LLVMDisposeMessage(message);
    }
    core_list_free(rows);
    return ok;
}

static bool
emit_native_module(n00bb_llvm_backend_t *backend, LLVMModuleRef module,
                   bool comptime, core_buffer_t *out,
                   core_diagnostics_t *diagnostics)
{
    char *message = nullptr;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()
        || LLVMInitializeNativeAsmParser())
        return backend_error(diagnostics, "LLVM native target is unavailable");
    char *triple = LLVMGetDefaultTargetTriple();
    core_uint128_t cache_key = 0;
    if (!comptime && backend->native_cache && triple) {
        LLVMMemoryBufferRef preopt = LLVMWriteBitcodeToMemoryBuffer(module);
        if (preopt) {
            cache_key = n00bb_native_cache_key(backend->native_cache,
                LLVMGetBufferStart(preopt), LLVMGetBufferSize(preopt),
                triple, backend->cpu_name, backend->cpu_features,
                backend->debug_enabled, backend->emit_frame_maps);
            LLVMDisposeMemoryBuffer(preopt);
            if (n00bb_native_cache_read(backend->native_cache, cache_key,
                    out)) {
                if (getenv("N00BB_PROFILE_COMPILE"))
                    fprintf(stderr, "PROFILE llvm-cache-hit %s\n",
                        ir_module_name(backend->runtime_source));
                LLVMDisposeMessage(triple);
                return true;
            }
        }
    }
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
        backend->debug_enabled ? LLVMCodeGenLevelDefault
                               : LLVMCodeGenLevelAggressive,
        LLVMRelocPIC,
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
    LLVMErrorRef pass_error = LLVMRunPasses(module,
        backend->debug_enabled ? "default<O2>" : "default<O3>", machine,
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
    if (!comptime && !emit_code_map(module, diagnostics)) {
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
    if (ok) {
        const char *bytes = LLVMGetBufferStart(object);
        size_t length = LLVMGetBufferSize(object);
        core_buffer_append(out, bytes, length);
        if (cache_key)
            n00bb_native_cache_write(backend->native_cache, cache_key,
                bytes, length);
    }
    else backend_error(diagnostics, message ? message : "LLVM object emission failed");
    if (object) LLVMDisposeMemoryBuffer(object);
    if (message) LLVMDisposeMessage(message);
    LLVMDisposeTargetMachine(machine);
    return ok;
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
    return emit_phase_image(backend, comptime, entry_initializers,
        initializer_count, array_producers, array_producer_count,
        metadata_bytes, false, out, diagnostics);
}

bool
n00bb_llvm_backend_emit_linkable_bitcode(n00bb_llvm_backend_t *backend,
    const char *const *entry_initializers, size_t initializer_count,
    size_t metadata_bytes, core_buffer_t *out,
    core_diagnostics_t *diagnostics)
{
    return emit_phase_image(backend, false, entry_initializers,
        initializer_count, nullptr, 0, metadata_bytes, true, out, diagnostics);
}

bool
n00bb_llvm_backend_link_bitcode(const n00bb_llvm_bitcode_input_t *inputs,
    size_t count, bool debug_enabled, bool native_cpu, bool emit_frame_maps,
    core_buffer_t *out, core_diagnostics_t *diagnostics)
{
    if (!inputs || !count || !out)
        return backend_error(diagnostics, "invalid LTO module inputs");
    LLVMContextRef context = LLVMContextCreate();
    if (!context)
        return backend_error(diagnostics, "could not create LTO context");
    LLVMModuleRef linked = nullptr;
    bool ok = true;
    for (size_t i = 0; ok && i < count; i++) {
        if (!inputs[i].data || !inputs[i].length) {
            ok = backend_error(diagnostics, "empty LTO bitcode module");
            break;
        }
        LLVMMemoryBufferRef buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
            inputs[i].data, inputs[i].length, "n00bb-lto-input");
        LLVMModuleRef next = nullptr;
        ok = buffer && !LLVMParseBitcodeInContext2(context, buffer, &next);
        if (buffer) LLVMDisposeMemoryBuffer(buffer);
        if (!ok) {
            backend_error(diagnostics, "could not parse LTO bitcode module");
            break;
        }
        char *triple = LLVMGetDefaultTargetTriple();
        if (triple) {
            LLVMSetTarget(next, triple);
            LLVMDisposeMessage(triple);
        }
        if (linked && strcmp(LLVMGetDataLayoutStr(linked),
                             LLVMGetDataLayoutStr(next))) {
            LLVMDisposeModule(next);
            ok = backend_error(diagnostics,
                "LTO modules have incompatible target data layouts");
            break;
        }
        if (linked && inputs[i].native_support) {
            // C frames scan conservatively; do not inline them into precise frames.
            LLVMContextRef module_context = LLVMGetModuleContext(next);
            unsigned kind = LLVMGetEnumAttributeKindForName(
                "noinline", sizeof("noinline") - 1);
            for (LLVMValueRef function = LLVMGetFirstFunction(next); function;
                 function = LLVMGetNextFunction(function)) {
                if (!LLVMCountBasicBlocks(function)) continue;
                LLVMValueRef import = LLVMGetNamedFunction(linked,
                    LLVMGetValueName(function));
                if (!import || LLVMCountBasicBlocks(import)) continue;
                LLVMAttributeRef noinline = LLVMCreateEnumAttribute(
                    module_context, kind, 0);
                LLVMAddAttributeAtIndex(function,
                    LLVMAttributeFunctionIndex, noinline);
            }
        }
        if (!linked) linked = next;
        else if (LLVMLinkModules2(linked, next)) {
            ok = backend_error(diagnostics, "could not link LTO bitcode modules");
        }
    }
    if (ok) {
        char *message = nullptr;
        if (LLVMVerifyModule(linked, LLVMReturnStatusAction, &message))
            ok = backend_error(diagnostics, message
                ? message : "invalid linked LTO module");
        if (message) LLVMDisposeMessage(message);
    }
    if (ok) {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        LLVMErrorRef error = LLVMRunPasses(linked,
            "function(simplifycfg),always-inline", nullptr, options);
        LLVMDisposePassBuilderOptions(options);
        if (error) {
            char *message = LLVMGetErrorMessage(error);
            ok = backend_error(diagnostics, message
                ? message : "could not simplify and inline linked LLVM functions");
            LLVMDisposeErrorMessage(message);
        }
    }
    if (ok) {
        LLVMContextRef module_context = LLVMGetModuleContext(linked);
        unsigned noinline_kind = LLVMGetEnumAttributeKindForName(
            "noinline", sizeof("noinline") - 1);
        unsigned alwaysinline_kind = LLVMGetEnumAttributeKindForName(
            "alwaysinline", sizeof("alwaysinline") - 1);
        for (LLVMValueRef function = LLVMGetFirstFunction(linked); function;
             function = LLVMGetNextFunction(function)) {
            bool has_roots = false;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function);
                 !has_roots && block; block = LLVMGetNextBasicBlock(block))
                for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
                     instruction; instruction = LLVMGetNextInstruction(instruction))
                    if (is_stack_map_call(instruction)) {
                        has_roots = true;
                        break;
                    }
            if (!has_roots) continue;
            if (LLVMGetEnumAttributeAtIndex(function,
                    LLVMAttributeFunctionIndex, alwaysinline_kind)) {
                for (LLVMUseRef use = LLVMGetFirstUse(function); use;
                     use = LLVMGetNextUse(use)) {
                    LLVMValueRef user = LLVMGetUser(use);
                    if (LLVMIsACallInst(user)
                        && LLVMGetCalledValue(user) == function) {
                        size_t length = 0;
                        const char *name = LLVMGetValueName2(function, &length);
                        core_buffer_t message = {0};
                        core_buffer_printf(&message,
                            "LTO could not inline managed function %.*s",
                            (int)length, name ? name : "");
                        ok = backend_error(diagnostics, message.data);
                        core_buffer_release(&message);
                        break;
                    }
                }
                if (!ok) break;
                LLVMRemoveEnumAttributeAtIndex(function,
                    LLVMAttributeFunctionIndex, alwaysinline_kind);
            }
            LLVMAttributeRef noinline = LLVMCreateEnumAttribute(module_context,
                noinline_kind, 0);
            LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex,
                                    noinline);
        }
    }
    if (ok) {
        n00bb_llvm_backend_t backend = {
            .debug_enabled = debug_enabled,
            .emit_frame_maps = emit_frame_maps,
        };
        if (native_cpu) {
            backend.cpu_name = LLVMGetHostCPUName();
            backend.cpu_features = LLVMGetHostCPUFeatures();
            ok = backend.cpu_name && backend.cpu_features;
        }
        if (ok) ok = emit_native_module(&backend, linked, false, out,
                                        diagnostics);
        else backend_error(diagnostics, "could not determine LTO target CPU");
        if (backend.cpu_name) LLVMDisposeMessage(backend.cpu_name);
        if (backend.cpu_features) LLVMDisposeMessage(backend.cpu_features);
    }
    if (linked) LLVMDisposeModule(linked);
    LLVMContextDispose(context);
    return ok;
}
