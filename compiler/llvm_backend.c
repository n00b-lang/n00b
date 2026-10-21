#include "llvm_backend.h"
#include "cached_body.h"
#include "compiler_axioms.h"
#include "sequence_transfer.h"

#include "ir/layout.h"
#include "ir/analyses.h"
#include "ir/doms.h"
#include "ir/analyses.h"
#include "ir/liveness.h"
#include "ir/ranges.h"
#include "ir/shape.h"
#include "ir/typelang.h"
#include "th/environment.h"
#include "abi_layout.h"
#include "abi_string.h"
#include "abi_type_index.h"
#include "frame_roots.h"
#include "llvm_stack_maps.h"
#include "llvm_native_cache.h"
#include "n00b-abi-v1.h"
#include "compiler_native_roots.h"
#include <time.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/Error.h>
#include <llvm-c/Linker.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/dict.h"
#include "lib/hash.h"
#include "lib/object_section.h"
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
core_list_decl(uint32_t);

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

static char *
llvm_text(core_string_t text)
{
    return core_string_to_cstr(text.data ? text : CORE_STRING_STATIC(""));
}

static void
set_value_name(LLVMValueRef value, core_string_t name)
{
    if (value && name.data && name.u8_bytes)
        LLVMSetValueName2(value, name.data, name.u8_bytes);
}

static LLVMValueRef
named_function(LLVMModuleRef module, core_string_t name)
{
    return name.data ? LLVMGetNamedFunctionWithLength(module, name.data,
                                                      name.u8_bytes)
                     : nullptr;
}

static LLVMValueRef
named_global(LLVMModuleRef module, core_string_t name)
{
    return name.data ? LLVMGetNamedGlobalWithLength(module, name.data,
                                                    name.u8_bytes)
                     : nullptr;
}

static LLVMValueRef
add_function(LLVMModuleRef module, core_string_t name, LLVMTypeRef type)
{
    LLVMValueRef function = LLVMAddFunction(module, "", type);
    set_value_name(function, name);
    return function;
}

static LLVMValueRef
add_global(LLVMModuleRef module, LLVMTypeRef type, core_string_t name)
{
    LLVMValueRef global = LLVMAddGlobal(module, type, "");
    set_value_name(global, name);
    return global;
}

static core_string_t
take_llvm_message(char *message)
{
    core_string_t text = core_string_from_cstr(message);
    if (message) LLVMDisposeMessage(message);
    return text;
}

static core_string_t
llvm_value_name(LLVMValueRef value)
{
    size_t length = 0;
    const char *name = value ? LLVMGetValueName2(value, &length) : nullptr;
    return core_string_borrow(name ? name : "", name ? length : 0);
}

typedef struct {
    core_string_t path;
    LLVMMetadataRef file;
} debug_file_t;
core_list_decl(debug_file_t);

typedef struct {
    LLVMDIBuilderRef builder;
    LLVMMetadataRef unit;
    core_list_t(debug_file_t) files;
} debug_info_t;

static _Thread_local debug_info_t *active_debug;

static LLVMMetadataRef
debug_file(debug_info_t *debug, core_string_t path)
{
    if (!path.data || !path.u8_bytes) path = CORE_STRING_STATIC("<unknown>");
    for (size_t i = 0; i < debug->files.len; i++)
        if (core_string_eq(debug->files.data[i].path, path))
            return debug->files.data[i].file;
    LLVMMetadataRef file = LLVMDIBuilderCreateFile(debug->builder, path.data,
        path.u8_bytes, "", 0);
    core_list_push(debug->files, ((debug_file_t){core_string_dup(path),
                                                 file}));
    return file;
}

static bool
debug_info_begin(debug_info_t *debug, LLVMModuleRef module,
                 const ir_module_t *source)
{
    *debug = (debug_info_t){
        .builder = LLVMCreateDIBuilder(module),
        .files = core_list_new(debug_file_t),
    };
    if (!debug->builder) return false;
    core_string_t path = ir_file_path(source, 0);
    LLVMMetadataRef file = debug_file(debug, path.data ? path
                                                       : ir_module_name(source));
    debug->unit = LLVMDIBuilderCreateCompileUnit(debug->builder,
        LLVMDWARFSourceLanguageC11, file, "n00b", 4, true, "", 0, 0, "", 0,
        LLVMDWARFEmissionFull, 0, false, false, "", 0, "", 0);
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMAddModuleFlag(module, LLVMModuleFlagBehaviorWarning,
        "Debug Info Version", 18, LLVMValueAsMetadata(
            LLVMConstInt(i32, LLVMDebugMetadataVersion(), false)));
    LLVMAddModuleFlag(module, LLVMModuleFlagBehaviorWarning, "Dwarf Version",
        13,
        LLVMValueAsMetadata(LLVMConstInt(i32, 4, false)));
    return debug->unit != nullptr;
}

static void
debug_info_end(debug_info_t *debug)
{
    if (debug->builder) {
        LLVMDIBuilderFinalize(debug->builder);
        LLVMDisposeDIBuilder(debug->builder);
    }
    for (size_t i = 0; i < debug->files.len; i++)
        core_free(debug->files.data[i].path.data);
    core_list_free(debug->files);
    *debug = (debug_info_t){0};
}

static int32_t
function_file(const ir_func_t *function)
{
    if (function->span.file >= 0) return function->span.file;
    for (uint32_t i = 0; i < function->n_blocks; i++)
        for (uint32_t j = 0; j < function->blocks[i]->n_instrs; j++)
            if (function->blocks[i]->instrs[j]->span.file >= 0)
                return function->blocks[i]->instrs[j]->span.file;
    return -1;
}

static LLVMMetadataRef
debug_subprogram(debug_info_t *debug, LLVMValueRef function,
                 const ir_func_t *source)
{
    int32_t file_id = function_file(source);
    core_string_t path = file_id >= 0
        ? ir_file_path(source->owner, file_id) : (core_string_t){0};
    LLVMMetadataRef file = debug_file(debug, path);
    size_t name_length = 0;
    const char *name = LLVMGetValueName2(function, &name_length);
    unsigned line = source->span.start_line > 0
        ? (unsigned)source->span.start_line : 0;
    LLVMMetadataRef type = LLVMDIBuilderCreateSubroutineType(debug->builder,
        file, nullptr, 0, LLVMDIFlagZero);
    LLVMMetadataRef subprogram = LLVMDIBuilderCreateFunction(debug->builder,
        file, source->name.data ? source->name.data : "",
        source->name.u8_bytes, name, name_length, file,
        line, type, false, true, line, LLVMDIFlagZero, true);
    if (subprogram) LLVMSetSubprogram(function, subprogram);
    return subprogram;
}

static void
debug_location(LLVMBuilderRef builder, LLVMMetadataRef subprogram,
               const ir_func_t *function, int32_t file, ir_span_t span)
{
    if (!subprogram) return;
    bool known = span.file >= 0 && span.file == file && span.start_line > 0;
    unsigned line = known ? (unsigned)span.start_line
        : function->span.start_line > 0 ? (unsigned)function->span.start_line
        : 0;
    unsigned column = known && span.start_col > 0 ? (unsigned)span.start_col
                                                   : 0;
    LLVMSetCurrentDebugLocation2(builder, LLVMDIBuilderCreateDebugLocation(
        LLVMGetBuilderContext(builder), line, column, subprogram, nullptr));
}

struct n00bb_llvm_backend_t {
    LLVMContextRef context;
    core_dict_t native_types;
    bool native_types_ready;
    LLVMModuleRef module;
    const ir_module_t *source;
    bool lowered;
    bool debug_enabled;
    core_string_t cpu_name;
    core_string_t cpu_features;
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
    const n00bb_llvm_function_binding_t *function_indices;
    size_t function_index_count;
    const n00bb_llvm_index_table_t *index_table;
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    const n00bb_llvm_finalizer_binding_t *finalizers;
    size_t finalizer_count;
    ir_func_t *const *reachable;
    size_t reachable_count;
    bool reachable_bound;
    ir_storage_origin_t *storage_origin;
    bool emit_frame_maps;
    n00bb_native_cache_t *native_cache;
    const n00bb_llvm_callback_bindings_t *callback_bindings;
    bool prepared;
    ir_summaries_t *summaries;
};

typedef struct {
    const ir_value_t *source;
    LLVMValueRef target;
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
    unsigned element;
    uint64_t shift;
    uint64_t width;
    bool packed;
    bool overlay;
    uint64_t overlay_offset;
} record_slot_t;

typedef struct {
    th_type *type;
    core_string_t name;
    LLVMTypeRef native;
    record_slot_t slot;
    bool found;
} member_match_t;
core_list_decl(member_match_t);

typedef ir_func_t *inline_function_ptr_t;
core_list_decl(inline_function_ptr_t);
core_list_decl(LLVMValueRef);

static const char gc_root_init_metadata[] = "n00bb.gc.root.init";
static const char code_name_attribute[] = "n00bb-code-name";

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
    return n00bb_cached_body(instruction->operands[0].value->owner);
}

typedef struct {
    const n00bb_llvm_index_table_t *binding;
    LLVMValueRef table;
} index_slots_t;

typedef struct {
    LLVMModuleRef module;
    LLVMBuilderRef builder;
    ir_func_t *function;
    const ir_module_t *source_module;
    index_slots_t *index_slots;
    LLVMValueRef target_function;
    bool with_mutator;
    unsigned parameter_base;
    LLVMValueRef result_box;
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
    const n00bb_llvm_type_index_t *type_indices;
    size_t type_index_count;
    const n00bb_llvm_function_binding_t *function_indices;
    size_t function_index_count;
    const n00bb_llvm_index_table_t *index_table;
    const n00bb_llvm_global_binding_t *global_bindings;
    size_t global_binding_count;
    ir_storage_origin_t *storage_origin;
    ir_ranges_t *ranges;
    size_t analysis_scope;
    core_indexed_table_t *frame_roots;
    LLVMValueRef *frame_slots;
    size_t frame_slot_count;
    bool conservative_frame;
    core_indexed_table_t *used_values;
    const ir_instr_t *current_instruction;
    ir_live_t *live;
    core_indexed_table_t *value_index;
    core_list_t(layout_match_t) *layout_matches;
    core_list_t(member_match_t) *member_matches;
    const core_list_t(callback_binding_t) *callback_bindings;
    core_string_t failure_detail;
} lower_state_t;

#define TEXT_ARGS(text) (int)(text).u8_bytes, (text).data
#define FAILURE_DETAIL(lit) CORE_STRING_STATIC(lit)

static bool
backend_report(core_diagnostics_t *diagnostics, core_string_t message)
{
    if (diagnostics)
        core_diagnostics_report(diagnostics, nullptr, CORE_DIAG_ERROR,
            CORE_STRING_STATIC("N00BB-B0006"), message);
    return false;
}

#define backend_error(diagnostics, lit) \
    backend_report((diagnostics), CORE_STRING_STATIC(lit))

static bool
backend_llvm_report(core_diagnostics_t *diagnostics, const char *llvm_message,
                    core_string_t fallback)
{
    core_string_t text = core_string_from_cstr(llvm_message);
    backend_report(diagnostics, text.data ? text : fallback);
    core_free(text.data);
    return false;
}

static LLVMValueRef
global_string_ptr(LLVMBuilderRef builder, core_string_t text,
                  const char *label)
{
    LLVMModuleRef module = LLVMGetGlobalParent(
        LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder)));
    LLVMValueRef contents = LLVMConstStringInContext2(
        LLVMGetModuleContext(module), text.data ? text.data : "",
        text.data ? text.u8_bytes : 0, false);
    LLVMValueRef global = LLVMAddGlobal(module, LLVMTypeOf(contents), label);
    LLVMSetInitializer(global, contents);
    LLVMSetGlobalConstant(global, true);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    LLVMSetUnnamedAddress(global, LLVMGlobalUnnamedAddr);
    LLVMSetAlignment(global, 1);
    return global;
}

static void
append_function_symbol(core_buffer_t *out, const ir_func_t *function)
{
    core_buffer_append_string(out, ir_module_name(function->owner));
    core_buffer_putc(out, '.');
    core_buffer_append_string(out, function->name);
}

static bool
unsupported(lower_state_t *state, const ir_instr_t *instruction)
{
    core_buffer_t message = {0};
    core_string_t spelling = ir_op_spelling(instruction->op);
    core_string_t module_name = ir_module_name(state->function->owner);
    core_buffer_printf(&message,
        "LLVM backend cannot lower '%.*s' in '%.*s.%.*s' yet",
        TEXT_ARGS(spelling), TEXT_ARGS(module_name),
        TEXT_ARGS(state->function->name));
    if (state->failure_detail.data)
        core_buffer_printf(&message, ": %.*s",
                           TEXT_ARGS(state->failure_detail));
    th_ctx *types = ir_module_types(state->function->owner);
    if (instruction->op == IR_OP_EXTRACT && instruction->n_operands == 2
        && instruction->operands[0].kind == IR_OPD_VALUE
        && instruction->operands[0].value
        && instruction->operands[1].kind == IR_OPD_NAME
        && instruction->operands[1].name.data) {
        core_string_t source = th_print(types,
            instruction->operands[0].value->type);
        core_buffer_printf(&message, "; source %.*s, field %.*s",
            TEXT_ARGS(source), TEXT_ARGS(instruction->operands[1].name));
    }
    if (instruction->op == IR_OP_AGGREGATE && instruction->dest
        && instruction->dest->type) {
        core_string_t type = th_print(types, instruction->dest->type);
        core_buffer_printf(&message, "; type %.*s", TEXT_ARGS(type));
    }
    if (instruction->op == IR_OP_TYPE_TEST && instruction->n_operands == 2
        && instruction->operands[0].kind == IR_OPD_VALUE
        && instruction->operands[0].value
        && instruction->operands[1].kind == IR_OPD_TYPE) {
        core_string_t subject = th_print(types,
            instruction->operands[0].value->type);
        core_buffer_printf(&message, "; subject %.*s, ", TEXT_ARGS(subject));
        core_string_t pattern = th_print(types, instruction->operands[1].type);
        core_buffer_printf(&message, "pattern %.*s", TEXT_ARGS(pattern));
    }
    ir_func_t *callee = direct_call_target(instruction);
    if (callee) {
        core_string_t callee_module = ir_module_name(callee->owner);
        core_buffer_printf(&message, " (callee '%.*s.%.*s')",
            TEXT_ARGS(callee_module), TEXT_ARGS(callee->name));
    }
    if (state->diagnostics) {
        core_diagnostics_report_at(state->diagnostics, nullptr,
            CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0006"),
            core_buffer_string(&message), (core_diag_location_t){
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
identified_struct_type(LLVMContextRef context, core_string_t name,
                       LLVMTypeRef *fields, unsigned count)
{
    char *native = llvm_text(name);
    LLVMTypeRef type = LLVMGetTypeByName2(context, native);
    if (!type) {
        type = LLVMStructCreateNamed(context, native);
        LLVMStructSetBody(type, fields, count, false);
    }
    core_free(native);
    return type;
}

static LLVMTypeRef
box_llvm_type(LLVMContextRef context)
{
    LLVMTypeRef fields[] = {
        LLVMInt16TypeInContext(context), LLVMInt16TypeInContext(context),
        LLVMInt32TypeInContext(context), LLVMInt64TypeInContext(context),
    };
    return identified_struct_type(context, CORE_STRING_STATIC("n00b.box"),
                                  fields, 4);
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
    return identified_struct_type(context, CORE_STRING_STATIC("n00b.bitptr"),
                                  fields, 4);
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
packed_value_width(ir_module_t *module, th_type *type)
{
    if (!type || th_builtin_kind_of(th_resolve(type)) == TH_BUILTIN_ATOMIC)
        return 0;
    th_type *definition = ir_type_definition(module, type);
    if (!definition) definition = th_resolve(type);
    uint64_t width = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    if (definition && th_kind_of(definition) == TH_BITINT
        && th_bitint_info(definition, &width, &flavor)
        && flavor == TH_BITINT_BITS)
        return width && width <= N00BB_PACKED_MAX_BITS ? width : 0;
    const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
        ir_module_target(module));
    return n00bb_abi_layout_packable(layout)
        && layout->occupied[0].width <= N00BB_PACKED_MAX_BITS
        ? layout->occupied[0].width : 0;
}

static uint64_t
packed_array_width(ir_module_t *module, th_type *type)
{
    uint64_t width = packed_value_width(module, type);
    if (!width || ir_machine_of(module, type).class == IR_CLASS_BOOL)
        return width;
    th_type *definition = ir_type_definition(module, type);
    if (!definition) definition = th_resolve(type);
    uint64_t bits = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    return definition && th_kind_of(definition) == TH_BITINT
        && th_bitint_info(definition, &bits, &flavor)
        && flavor == TH_BITINT_BITS ? width : 0;
}

static th_type *
atomic_element_type(th_type *type)
{
    type = th_builtin_applied(type, TH_BUILTIN_ATOMIC, 1);
    const th_arg *element = type ? th_named_arg(type, 0) : nullptr;
    return element && element->kind == TH_ARG_TYPE
        ? element->type : nullptr;
}

static bool
is_atomic_bool(ir_module_t *module, th_type *type)
{
    th_type *element = atomic_element_type(type);
    return element
        && ir_machine_of(module, element).class == IR_CLASS_BOOL;
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

typedef struct {
    const ir_final_layout_t *layout;
    th_type *definition;
    record_slot_t *slots;
    LLVMTypeRef *elements;
    uint64_t *offsets;
    bool *byte_runs;
    unsigned n_elements;
} record_shape_t;

static void
record_shape_release(record_shape_t *shape)
{
    free(shape->slots);
    free(shape->elements);
    free(shape->offsets);
    free(shape->byte_runs);
    *shape = (record_shape_t){0};
}

static th_type *
record_member_type(const record_shape_t *shape,
                   const ir_final_member_layout_t *member)
{
    th_type *field = member->name.data && shape->definition
        ? th_record_lookup(shape->definition, member->name) : nullptr;
    return field ? field : member->type;
}

static bool
record_member_extent(ir_module_t *module,
                     const ir_final_member_layout_t *member,
                     uint64_t *bytes, uint32_t *alignment)
{
    th_type *storage = member->inline_element_count
        ? member->inline_element_type : member->type;
    const ir_final_layout_t *child = storage
        ? ir_layout_final_visible(module, storage, ir_module_target(module))
        : nullptr;
    uint64_t stride = 0;
    if (!child || !ir_layout_final_stride_bits(child, &stride)
        || !child->alignment_bytes) return false;
    uint64_t bits = member->inline_element_count
        ? member->inline_element_stride_bits * member->inline_element_count
        : stride;
    if (bits % 8) return false;
    *bytes = bits / 8;
    *alignment = child->alignment_bytes;
    return true;
}

static bool
record_shape_push(record_shape_t *shape, size_t capacity, LLVMTypeRef type,
                  uint64_t offset, bool byte_run)
{
    if (!type || shape->n_elements >= capacity
        || shape->n_elements >= UINT32_MAX) return false;
    shape->elements[shape->n_elements] = type;
    shape->offsets[shape->n_elements] = offset;
    shape->byte_runs[shape->n_elements] = byte_run;
    shape->n_elements++;
    return true;
}

static bool
record_shape_pad(record_shape_t *shape, size_t capacity,
                 LLVMContextRef context, uint64_t *cursor, uint64_t offset,
                 uint32_t alignment)
{
    uint64_t aligned = (*cursor + alignment - 1) / alignment * alignment;
    if (offset < *cursor) return false;
    if (offset != aligned && (offset - *cursor > UINT32_MAX
        || !record_shape_push(shape, capacity, LLVMArrayType(
            LLVMInt8TypeInContext(context), (unsigned)(offset - *cursor)),
            *cursor, true))) return false;
    *cursor = offset;
    return true;
}

static bool
record_shape_build(ir_module_t *requester, th_type *type,
                   LLVMContextRef context, record_shape_t *shape)
{
    *shape = (record_shape_t){0};
    ir_module_t *module = requester;
    ir_module_t *owner = type
        ? ir_type_definition_owner(requester, type) : nullptr;
    if (owner && ir_layout_final(owner, type, ir_module_target(owner)))
        module = owner;
    th_type *definition = type ? ir_type_definition(module, type) : nullptr;
    if (type && !definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    const ir_final_layout_t *layout = type ? ir_layout_final_visible(module,
        type, ir_module_target(module)) : nullptr;
    size_t count = layout ? layout->n_members : 0;
    uint64_t stride_bits = 0;
    if (!definition || th_kind_of(definition) != TH_RECORD || !layout
        || count != th_record_len(definition) || count > UINT32_MAX
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8) return false;
    size_t capacity = 2 * count + 1;
    shape->layout = layout;
    shape->definition = definition;
    shape->slots = calloc(count ? count : 1, sizeof(*shape->slots));
    shape->elements = calloc(capacity, sizeof(*shape->elements));
    shape->offsets = calloc(capacity, sizeof(*shape->offsets));
    shape->byte_runs = calloc(capacity, sizeof(*shape->byte_runs));
    size_t *order = calloc(count ? count : 1, sizeof(*order));
    bool ok = shape->slots && shape->elements && shape->offsets
        && shape->byte_runs && order;
    if (ok) record_layout_order(layout, order);
    uint64_t cursor = 0;
    uint32_t record_alignment = 1;
    for (size_t i = 0; ok && i < count;) {
        const ir_final_member_layout_t *member = &layout->members[order[i]];
        uint64_t width = member->inline_element_count ? 0
            : packed_value_width(module, record_member_type(shape, member));
        uint64_t bytes = 0;
        uint32_t alignment = 1;
        bool sized = record_member_extent(module, member, &bytes,
                                          &alignment);
        uint64_t first = member->bit_offset / 8;
        uint64_t last = width ? (member->bit_offset + width + 7) / 8
                              : first + bytes;
        if (!width && (!sized || member->bit_offset % 8)) {
            ok = false;
            break;
        }
        size_t j = i + 1;
        bool overlay = false;
        uint32_t overlay_alignment = alignment;
        for (; j < count; j++) {
            const ir_final_member_layout_t *next = &layout->members[order[j]];
            if (next->bit_offset / 8 >= last) break;
            uint64_t next_width = next->inline_element_count ? 0
                : packed_value_width(module, record_member_type(shape, next));
            uint64_t next_bytes = 0;
            uint32_t next_alignment = 1;
            if (!width || !next_width || overlay) {
                if (!record_member_extent(module, next, &next_bytes,
                        &next_alignment) || next->bit_offset % 8
                    || member->bit_offset % 8 || !sized) {
                    ok = false;
                    break;
                }
                overlay = true;
                if (next_alignment > overlay_alignment)
                    overlay_alignment = next_alignment;
                uint64_t next_last = next->bit_offset / 8 + next_bytes;
                if (first + bytes > last) last = first + bytes;
                if (next_last > last) last = next_last;
                continue;
            }
            uint64_t next_last = (next->bit_offset + next_width + 7) / 8;
            if (next_last > last) last = next_last;
        }
        if (!ok) break;
        if (overlay) {
            uint64_t span = last - first;
            unsigned unit_bits = overlay_alignment * 8;
            ok = !(first % overlay_alignment) && !(span % overlay_alignment)
                && span / overlay_alignment <= UINT32_MAX
                && record_shape_pad(shape, capacity, context, &cursor, first,
                                    overlay_alignment)
                && record_shape_push(shape, capacity, LLVMArrayType(
                    LLVMIntTypeInContext(context, unit_bits),
                    (unsigned)(span / overlay_alignment)), first, false);
            for (size_t k = i; ok && k < j; k++)
                shape->slots[order[k]] = (record_slot_t){
                    .element = shape->n_elements - 1,
                    .overlay = true,
                    .overlay_offset = layout->members[order[k]].bit_offset / 8
                        - first,
                };
            if (overlay_alignment > record_alignment)
                record_alignment = overlay_alignment;
            cursor = last;
            i = j;
            continue;
        }
        uint64_t bound = j < count
            ? layout->members[order[j]].bit_offset / 8 : stride_bits / 8;
        bool natural = j == i + 1 && !(member->bit_offset % 8) && sized
            && first + bytes <= bound;
        if (natural) {
            th_type *field = record_member_type(shape, member);
            LLVMTypeRef element = member->inline_element_count
                ? inline_member_type(module, member, context)
                : ir_machine_of(module, field).class == IR_CLASS_BOOL
                    ? LLVMInt8TypeInContext(context)
                    : value_type(module, field, context);
            ok = element && LLVMGetTypeKind(element) != LLVMVoidTypeKind
                && record_shape_pad(shape, capacity, context, &cursor, first,
                                    alignment)
                && record_shape_push(shape, capacity, element, first, false);
            if (!ok) break;
            shape->slots[order[i]] = (record_slot_t){
                .element = shape->n_elements - 1,
            };
            if (alignment > record_alignment) record_alignment = alignment;
            cursor = first + bytes;
        }
        else {
            ok = last - first <= UINT32_MAX
                && record_shape_pad(shape, capacity, context, &cursor, first,
                                    1)
                && record_shape_push(shape, capacity, LLVMArrayType(
                    LLVMInt8TypeInContext(context), (unsigned)(last - first)),
                    first, true);
            for (size_t k = i; ok && k < j; k++) {
                const ir_final_member_layout_t *packed =
                    &layout->members[order[k]];
                shape->slots[order[k]] = (record_slot_t){
                    .element = shape->n_elements - 1,
                    .shift = packed->bit_offset - first * 8,
                    .width = packed_value_width(module,
                        record_member_type(shape, packed)),
                    .packed = true,
                };
            }
            cursor = last;
        }
        i = j;
    }
    uint64_t stride = stride_bits / 8;
    uint64_t natural_end = (cursor + record_alignment - 1) / record_alignment
        * record_alignment;
    if (ok && (cursor > stride || record_alignment != layout->alignment_bytes))
        ok = false;
    if (ok && natural_end != stride)
        ok = stride - cursor <= UINT32_MAX
            && record_shape_push(shape, capacity, LLVMArrayType(
                LLVMInt8TypeInContext(context), (unsigned)(stride - cursor)),
                cursor, true);
    free(order);
    if (!ok) record_shape_release(shape);
    return ok;
}

static LLVMTypeRef
shared_option_value_type(ir_module_t *module, th_type *type,
                         LLVMContextRef context, uint8_t *mask)
{
    const th_arg *argument = th_named_arg(type, 0);
    th_type *child = argument && argument->kind == TH_ARG_TYPE
        ? th_builtin_applied(argument->type, TH_BUILTIN_OPTION, 1) : nullptr;
    if (!child) return nullptr;
    core_string_t target = ir_module_target(module);
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
    if (!child || !th_builtin_applied(type, TH_BUILTIN_OPTION, 1)
        || !th_is_closed(ir_module_types(module), type)) return nullptr;
    core_string_t target = ir_module_target(module);
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
        || (aggregate_payload ? outer_bytes < inner_bytes
                              : outer_bytes != inner_bytes)
        || !outer_bytes
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
    uint64_t unit_bits = outer->alignment_bytes * 8;
    if (aggregate_payload && unit_bits >= 8 && unit_bits <= 64
        && outer_stride > unit_bits && outer_stride % unit_bits == 0
        && outer_stride / unit_bits <= UINT32_MAX)
        return LLVMArrayType(LLVMIntTypeInContext(context,
                                                  (unsigned)unit_bits),
                             (unsigned)(outer_stride / unit_bits));
    return LLVMIntTypeInContext(context, (unsigned)(aggregate_payload
        ? outer_stride : outer_bytes * 8));
}

static LLVMTypeRef
aggregate_option_type(ir_module_t *module, th_type *type,
                      LLVMContextRef context)
{
    type = th_builtin_applied(type, TH_BUILTIN_OPTION, 1);
    if (!type) return nullptr;
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
    core_string_t target = ir_module_target(module);
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
        && th_named_info(type, nullptr, &open_variant, nullptr)
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
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef fields[] = {pointer, pointer};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    size_t arguments = 0;
    bool variant = false;
    if (!th_named_info(th_resolve(type), nullptr, &variant, &arguments)
        && (!definition || th_kind_of(definition) != TH_RECORD))
        return nullptr;
    th_builtin_kind_t builtin = th_builtin_kind_of(type);
    if (builtin == TH_BUILTIN_ANY && !arguments)
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
    if (builtin == TH_BUILTIN_ATOMIC && arguments == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        return argument && argument->kind == TH_ARG_TYPE
            ? value_type(module, argument->type, context) : nullptr;
    }
    if (n00bb_frame_pointer_option(type)
        && th_is_closed(ir_module_types(module), type))
        return LLVMPointerTypeInContext(context, 0);
    if (builtin == TH_BUILTIN_OPTION && arguments == 1)
        return aggregate_option_type(module, type, context);
    if (builtin == TH_BUILTIN_TUPLE && arguments <= UINT32_MAX) {
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
    if (builtin == TH_BUILTIN_STRING && !arguments) {
        LLVMTypeRef word = LLVMInt64TypeInContext(context);
        LLVMTypeRef fields[] = {word, word};
        return LLVMStructTypeInContext(context, fields, 2, false);
    }
    if (definition && th_kind_of(definition) == TH_RECORD) {
        record_shape_t shape;
        LLVMTypeRef record = record_shape_build(module, type, context, &shape)
            ? LLVMStructTypeInContext(context, shape.elements,
                                      shape.n_elements, false)
            : nullptr;
        record_shape_release(&shape);
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
static bool returns_through_box(ir_module_t *module, th_type *signature,
                                LLVMContextRef context);
static LLVMTypeRef mutator_function_type(LLVMModuleRef target,
                                        ir_func_t *function);
static LLVMValueRef mutator_function(LLVMModuleRef target,
                                    ir_func_t *function);
static void mutator_symbol(core_buffer_t *name, ir_func_t *function);
static LLVMTypeRef native_function_type(LLVMModuleRef target,
                                        ir_module_t *owner,
                                        th_type *signature,
                                        bool native_binding);
static LLVMTypeRef optional_record_type(LLVMModuleRef target,
                                        ir_func_t *function);
static void emit_trap_if(lower_state_t *state, LLVMValueRef condition);
static uint32_t runtime_type_index(lower_state_t *state, th_type *type);
static uint32_t runtime_function_index(lower_state_t *state,
                                       const ir_func_t *function);
static LLVMValueRef runtime_type_index_value(lower_state_t *state,
                                             th_type *type);
static LLVMValueRef runtime_function_index_value(lower_state_t *state,
                                                 const ir_func_t *function);
static LLVMValueRef runtime_type_value(lower_state_t *state, th_type *type);
static LLVMValueRef box_value_with(lower_state_t *state, LLVMValueRef value,
                                   th_type *type, bool in_frame);
static LLVMValueRef escape_boxes(lower_state_t *state, LLVMValueRef value);
static bool escape_box_contents(lower_state_t *state, LLVMValueRef slot);
static LLVMValueRef location_slot(lower_state_t *state);
static bool prepare_box_slot_in(lower_state_t *state, bool in_frame,
                                LLVMValueRef type_index, LLVMValueRef slot);
static bool write_location(lower_state_t *state, LLVMValueRef location,
                           LLVMValueRef value_slot);

static LLVMValueRef
box_runtime_call_named(lower_state_t *state, core_string_t name,
                       LLVMTypeRef result, LLVMTypeRef *parameters,
                       LLVMValueRef *arguments, unsigned count)
{
    LLVMTypeRef signature = LLVMFunctionType(result, parameters, count,
                                             false);
    LLVMValueRef callee = named_function(state->module, name);
    if (!callee) callee = add_function(state->module, name, signature);
    if (!callee || LLVMGlobalGetValueType(callee) != signature)
        return nullptr;
    return LLVMBuildCall2(state->builder, signature, callee, arguments,
                          count, "");
}

#define box_runtime_call(state, lit, ...) \
    box_runtime_call_named((state), CORE_STRING_STATIC(lit), __VA_ARGS__)
static bool box_in_frame(lower_state_t *state, const ir_instr_t *instruction);
static LLVMValueRef runtime_sized_payload(lower_state_t *state,
    const ir_instr_t *instruction, LLVMValueRef type_index,
    LLVMValueRef bytes);
static LLVMValueRef runtime_sized_payload_in(lower_state_t *state,
    bool use_frame, LLVMValueRef type_index, LLVMValueRef bytes);
static bool llvm_type_holds_box(LLVMTypeRef type, LLVMTypeRef box);

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
    core_string_t name = global->name;
    for (size_t i = 0; i < count; i++)
        if (bindings[i].global == identity) {
            name = bindings[i].native_name;
            break;
        }
    LLVMValueRef target = named_global(module, name);
    if (target) return target;
    LLVMTypeRef type = value_type(global->owner, global->type,
                                 LLVMGetModuleContext(module));
    return type ? add_global(module, type, name) : nullptr;
}

static LLVMValueRef
global_initialized_ref(LLVMModuleRef module, const ir_global_t *global,
                       const n00bb_llvm_global_binding_t *bindings,
                       size_t count)
{
    LLVMValueRef storage = global_ref(module, global, bindings, count);
    if (!storage) return nullptr;
    core_string_t name = llvm_value_name(storage);
    core_buffer_t key = {0};
    core_buffer_printf(&key, "__n00bb_initialized.%.*s", TEXT_ARGS(name));
    LLVMValueRef flag = named_global(module, core_buffer_string(&key));
    if (!flag)
        flag = add_global(module,
            LLVMInt8TypeInContext(LLVMGetModuleContext(module)),
            core_buffer_string(&key));
    core_buffer_release(&key);
    return flag;
}

static bool
guard_global_read(lower_state_t *state, const ir_global_t *global)
{
    const ir_global_t *identity = ir_global_identity((ir_global_t *)global);
    if (!identity || identity->is_extern
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
    if (!identity || identity->is_extern) return true;
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
    if (!identity || identity->is_extern
        || identity->mutability != IR_MUT_CONST
        || ir_global_storage_duration(identity) == IR_STORAGE_DURATION_THREAD
        || ir_global_can_write(identity, IR_PHASE_RUNTIME)
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

static uint64_t
pointer_key_hash(void *key)
{
    return core_hash_raw(&key, sizeof(key));
}

static bool
pointer_key_equal(const void *left, const void *right)
{
    return left == right;
}

static LLVMValueRef
local_slot(lower_state_t *state, const ir_local_t *local)
{
    const ir_func_t *function = state->function;
    if (!local) return nullptr;
    if (local->index < function->n_locals
        && function->locals[local->index] == local)
        return state->locals[local->index];
    for (uint32_t i = 0; i < function->n_locals; i++)
        if (function->locals[i] == local) return state->locals[i];
    return nullptr;
}

static lowered_value_t *
lowered_entry(lower_state_t *state, const ir_value_t *value)
{
    const core_indexed_entry_t *entry = value && state->value_index
        ? core_indexed_table_find(state->value_index, value) : nullptr;
    return entry ? entry->value : nullptr;
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
        unsigned native_position = state->parameter_base
            + (unsigned)formal_index;
        if (native_position >= LLVMCountParams(state->target_function))
            return nullptr;
        LLVMValueRef parameter = LLVMGetParam(state->target_function,
                                               native_position);
        if (!formal || !th_mode_produces_output(formal->mode))
            return parameter;
        LLVMTypeRef type = value_type(state->function->owner, value->type,
            LLVMGetModuleContext(state->module));
        return type ? LLVMBuildLoad2(state->builder, type, parameter,
                                     "copyin") : nullptr;
    }
    if (value->kind == IR_VAL_FUNC && value->owner) {
        ir_func_t *target = n00bb_cached_body(value->owner);
        core_buffer_t symbol = {0};
        append_function_symbol(&symbol, target);
        LLVMValueRef function = named_function(state->module,
            core_buffer_string(&symbol));
        if (!function && target->owner != state->source_module) {
            LLVMTypeRef signature = function_type(state->module, target);
            if (signature)
                function = add_function(state->module,
                    core_buffer_string(&symbol), signature);
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
        LLVMValueRef slot = local_slot(state, value->local);
        if (slot) {
            LLVMTypeRef type = value_type(state->function->owner,
                value->local->type, LLVMGetModuleContext(state->module));
            return type ? LLVMBuildLoad2(state->builder, type, slot, "local")
                        : nullptr;
        }
    }
    lowered_value_t *entry = lowered_entry(state, value);
    return entry ? entry->target : nullptr;
}

static LLVMBasicBlockRef
lowered_block(lower_state_t *state, const ir_block_t *block)
{
    if (block && block->index < state->function->n_blocks
        && state->function->blocks[block->index] == block)
        return state->blocks[block->index];
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

static LLVMValueRef entry_alloca(lower_state_t *state, LLVMTypeRef type);
static bool large_aggregate(lower_state_t *state, LLVMTypeRef type);
static LLVMValueRef aggregate_element(lower_state_t *state,
                                      LLVMValueRef aggregate,
                                      unsigned element, const char *name);

static bool
spill_root_slot(lower_state_t *state, LLVMValueRef value)
{
    LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(value));
    LLVMValueRef write = slot
        ? LLVMBuildStore(state->builder, value, slot) : nullptr;
    if (write) LLVMSetVolatile(write, true);
    return write != nullptr;
}

static bool spill_conservative_root(lower_state_t *state, th_type *type,
                                    LLVMValueRef value);

static bool
spill_conservative_fields(lower_state_t *state, th_type *type,
                          LLVMValueRef value, bool *spilled)
{
    *spilled = false;
    th_type *definition = ir_type_definition(state->function->owner, type);
    if (!definition && th_kind_of(th_resolve(type)) == TH_RECORD)
        definition = th_resolve(type);
    bool variant = false;
    th_named_info(th_resolve(type), nullptr, &variant, nullptr);
    if (variant || !definition || th_kind_of(definition) != TH_RECORD
        || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMStructTypeKind)
        return true;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    record_shape_t shape;
    if (!layout
        || !record_shape_build(state->function->owner, type, context, &shape))
        return true;
    LLVMTypeRef shape_type = LLVMTypeOf(value);
    unsigned elements = LLVMCountStructElementTypes(shape_type);
    bool ok = true;
    for (size_t i = 0; ok && i < shape.layout->n_members; i++) {
        const ir_final_member_layout_t *member = &shape.layout->members[i];
        const record_slot_t *slot = &shape.slots[i];
        if (slot->element >= elements) {
            ok = false;
            break;
        }
        th_type *field = member->name.data
            ? th_record_lookup(definition, member->name) : nullptr;
        bool raw = !field || member->inline_element_count || slot->packed
            || slot->overlay;
        if (raw && LLVMStoreSizeOfType(layout,
                LLVMStructGetTypeAtIndex(shape_type, slot->element))
                < LLVMPointerSize(layout))
            continue;
        if (!raw && !n00bb_frame_native_pointer(state->function->owner, field)
            && !n00bb_frame_traced_value(state->function, field))
            continue;
        LLVMValueRef part = aggregate_element(state, value, slot->element,
                                              "root.field");
        if (!part) ok = false;
        else ok = raw ? spill_root_slot(state, part)
                      : spill_conservative_root(state, field, part);
    }
    record_shape_release(&shape);
    *spilled = ok;
    return ok;
}

static bool
spill_conservative_root(lower_state_t *state, th_type *type,
                        LLVMValueRef value)
{
    if (!state->conservative_frame || !value || LLVMIsUndef(value)
        || LLVMIsPoison(value) || !type
        || (!n00bb_frame_native_pointer(state->function->owner, type)
            && !n00bb_frame_traced_value(state->function, type)))
        return true;
    if (large_aggregate(state, LLVMTypeOf(value))) {
        bool spilled = false;
        if (!spill_conservative_fields(state, type, value, &spilled))
            return false;
        if (spilled) return true;
    }
    return spill_root_slot(state, value);
}

static bool
conservative_spill_needed(lower_state_t *state,
                          const ir_instr_t *instruction, LLVMValueRef value)
{
    if (!state->conservative_frame) return false;
    if (state->used_values
        && !core_indexed_table_find(state->used_values, instruction->dest))
        return false;
    if (instruction->op != IR_OP_COPY || instruction->n_operands != 1
        || instruction->operands[0].kind != IR_OPD_VALUE
        || !instruction->operands[0].value
        || instruction->operands[0].value->kind != IR_VAL_INSTR)
        return true;
    return lowered_value(state, instruction->operands[0].value) != value;
}

static bool
save_result(lower_state_t *state, const ir_instr_t *instruction,
            LLVMValueRef value)
{
    if (instruction->dest_kind == IR_DEST_NONE) return true;
    if (!value) return false;
    if (instruction->dest_kind == IR_DEST_LOCAL) {
        LLVMValueRef slot = local_slot(state, instruction->dest_local);
        if (!slot) return false;
        LLVMBuildStore(state->builder, value, slot);
        if (state->frame_roots) {
            const core_indexed_entry_t *root = core_indexed_table_find(
                state->frame_roots, instruction->dest_local);
            if (root) {
                LLVMValueRef pointer = frame_root_pointer(state, value,
                    instruction->dest_local->type);
                if (!pointer) return false;
                LLVMValueRef write = LLVMBuildStore(state->builder, pointer,
                    state->frame_slots[root->index]);
                LLVMSetVolatile(write, true);
            }
        }
        return true;
    }
    if (instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || instruction->dest->kind != IR_VAL_INSTR
        || state->value_count == state->value_capacity) return false;
    lowered_value_t *entry = &state->values[state->value_count++];
    *entry = (lowered_value_t){
        .source = instruction->dest, .target = value,
    };
    if (!state->value_index
        || !core_indexed_table_intern(state->value_index, instruction->dest,
                                      entry, nullptr)) return false;
    if (instruction->op != IR_OP_PHI
        && conservative_spill_needed(state, instruction, value)
        && !spill_conservative_root(state, instruction->dest->type, value))
        return false;
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
spill_conservative_phis(lower_state_t *state, const ir_block_t *block,
                        uint32_t last_phi)
{
    for (uint32_t i = 0; i <= last_phi; i++) {
        ir_instr_t *instruction = block->instrs[i];
        if (instruction->op != IR_OP_PHI || !instruction->dest) continue;
        if (!spill_conservative_root(state, instruction->dest->type,
                lowered_value(state, instruction->dest))) return false;
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
emit_trap_reason(lower_state_t *state, core_string_t reason)
{
    if (!reason.data) {
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
    LLVMValueRef text = global_string_ptr(state->builder, reason,
                                          "trap.reason");
    LLVMValueRef arguments[] = {text,
        LLVMConstInt(i64, reason.u8_bytes, false)};
    LLVMBuildCall2(state->builder, trap_type, trap, arguments, 2, "");
    LLVMBuildUnreachable(state->builder);
}

static void
emit_trap(lower_state_t *state)
{
    emit_trap_reason(state, CORE_STRING_STATIC("runtime check failed"));
}

static LLVMValueRef
bitset_mask(lower_state_t *state, th_type *type)
{
    ir_module_t *source = state->function->owner;
    th_type *representation = ir_type_definition(source, type);
    uint64_t bits = 0;
    th_bitint_flavor flavor = TH_BITINT_UNSIGNED;
    LLVMTypeRef storage = value_type(source, type,
                                     LLVMGetModuleContext(state->module));
    if (!representation || !storage
        || !th_bitint_info(representation, &bits, &flavor)
        || LLVMGetTypeKind(storage) != LLVMIntegerTypeKind
        || bits > LLVMGetIntTypeWidth(storage) || bits > 128) return nullptr;
    uint64_t words[2] = {
        bits >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1,
        bits <= 64 ? 0 : bits >= 128 ? ~(uint64_t)0
                                     : ((uint64_t)1 << (bits - 64)) - 1,
    };
    return LLVMConstIntOfArbitraryPrecision(storage, 2, words);
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
static LLVMValueRef runtime_type_component(lower_state_t *state,
                                           LLVMValueRef type_index,
                                           uint32_t ordinal);
static LLVMValueRef load_array_extent(lower_state_t *state,
                                      LLVMValueRef address, const char *name);

static LLVMValueRef
make_packed_bitptr(lower_state_t *state, LLVMValueRef data,
                   LLVMValueRef bit_position, th_type *element)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i16 = LLVMInt16TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    uint32_t type_index = runtime_type_index(state, element);
    uint64_t width = packed_value_width(state->function->owner, element);
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
        runtime_type_index_value(state, element), 1, "bitptr.with.type");
    value = LLVMBuildInsertValue(state->builder, value, low, 2,
        "bitptr.with.start");
    return LLVMBuildInsertValue(state->builder, value, end, 3,
        "bitptr.with.end");
}

static LLVMValueRef
boxed_bitptr(lower_state_t *state, LLVMValueRef box)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i16 = LLVMInt16TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef parameters[] = {i32};
    LLVMTypeRef signature = LLVMFunctionType(pointer, parameters, 1, false);
    LLVMValueRef lookup = LLVMGetNamedFunction(state->module,
                                               "n00b_builtin_abi_type");
    if (!lookup) lookup = LLVMAddFunction(state->module,
                                          "n00b_builtin_abi_type", signature);
    if (!lookup || LLVMGlobalGetValueType(lookup) != signature)
        return nullptr;
    LLVMTypeRef bitptr_type = bitptr_llvm_type(context);
    LLVMValueRef type_field = LLVMBuildExtractValue(state->builder, box, 2,
                                                    "bitptr.box.type");
    LLVMValueRef payload = LLVMBuildExtractValue(state->builder, box, 3,
                                                 "bitptr.box.payload");
    LLVMValueRef data = LLVMBuildIntToPtr(state->builder, payload, pointer,
                                          "bitptr.box.data");
    LLVMValueRef reference_index = LLVMBuildAnd(state->builder, type_field,
        LLVMConstInt(i32, 0x7fffffff, false), "bitptr.box.reference");
    LLVMValueRef reference_row = LLVMBuildCall2(state->builder, signature,
        lookup, &reference_index, 1, "bitptr.box.reference.row");
    LLVMValueRef offset[] = {LLVMConstInt(i64, 8, false)};
    LLVMValueRef reference_bits = LLVMBuildLoad2(state->builder, i64,
        LLVMBuildGEP2(state->builder, LLVMInt8TypeInContext(context),
            reference_row, offset, 1, "bitptr.box.reference.size"),
        "bitptr.box.reference.bits");
    LLVMValueRef wide = LLVMBuildICmp(state->builder, LLVMIntEQ,
        reference_bits, LLVMConstInt(i64, 128, false), "bitptr.box.wide");
    LLVMBasicBlockRef stored = LLVMAppendBasicBlockInContext(context,
        state->target_function, "bitptr.box.stored");
    LLVMBasicBlockRef address = LLVMAppendBasicBlockInContext(context,
        state->target_function, "bitptr.box.address");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        state->target_function, "bitptr.box.done");
    LLVMBuildCondBr(state->builder, wide, stored, address);

    LLVMPositionBuilderAtEnd(state->builder, stored);
    LLVMValueRef loaded = LLVMBuildLoad2(state->builder, bitptr_type, data,
                                         "bitptr.box.loaded");
    LLVMBuildBr(state->builder, done);

    LLVMPositionBuilderAtEnd(state->builder, address);
    LLVMValueRef referent = runtime_type_component(state, reference_index, 0);
    if (!referent) return nullptr;
    LLVMValueRef referent_row = LLVMBuildCall2(state->builder, signature,
        lookup, &referent, 1, "bitptr.box.referent.row");
    LLVMValueRef referent_bits = LLVMBuildLoad2(state->builder, i64,
        LLVMBuildGEP2(state->builder, LLVMInt8TypeInContext(context),
            referent_row, offset, 1, "bitptr.box.referent.size"),
        "bitptr.box.referent.bits");
    LLVMValueRef built = LLVMGetUndef(bitptr_type);
    built = LLVMBuildInsertValue(state->builder, built, data, 0,
                                 "bitptr.box.from.base");
    built = LLVMBuildInsertValue(state->builder, built, referent, 1,
                                 "bitptr.box.from.type");
    built = LLVMBuildInsertValue(state->builder, built,
        LLVMConstInt(i16, 0, false), 2, "bitptr.box.from.start");
    built = LLVMBuildInsertValue(state->builder, built,
        LLVMBuildTrunc(state->builder, referent_bits, i16, "bitptr.box.end"),
        3, "bitptr.box.from.end");
    LLVMBasicBlockRef address_end = LLVMGetInsertBlock(state->builder);
    LLVMBuildBr(state->builder, done);

    LLVMPositionBuilderAtEnd(state->builder, done);
    LLVMValueRef result = LLVMBuildPhi(state->builder, bitptr_type,
                                       "bitptr.box");
    LLVMValueRef values[] = {loaded, built};
    LLVMBasicBlockRef blocks[] = {stored, address_end};
    LLVMAddIncoming(result, values, blocks, 2);
    return result;
}

static LLVMValueRef
reference_bitptr(lower_state_t *state, LLVMValueRef reference,
                 th_type *reference_type, th_type *result_type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    ir_module_t *module = state->function->owner;
    LLVMTypeRef target = value_type(module, result_type, context);
    th_type *referent = reference_type
        ? th_ind_referent(th_resolve(reference_type)) : nullptr;
    if (!reference || !referent || !target
        || LLVMGetTypeKind(target) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(target) != 4) {
        backend_error(state->diagnostics,
            "bitptr requires a reference and the Bitptr record");
        return nullptr;
    }
    LLVMValueRef bitptr = reference;
    if (LLVMTypeOf(reference) == box_llvm_type(context))
        bitptr = boxed_bitptr(state, reference);
    else if (LLVMTypeOf(reference) != bitptr_llvm_type(context)) {
        if (LLVMGetTypeKind(LLVMTypeOf(reference)) != LLVMPointerTypeKind) {
            backend_error(state->diagnostics,
                "bitptr requires a reference whose referent type is known "
                "at compile time");
            return nullptr;
        }
        const ir_final_layout_t *layout = ir_layout_final_visible(module,
            referent, ir_module_target(module));
        uint64_t width = 0;
        uint32_t type_index = runtime_type_index(state, referent);
        if (!layout || !ir_layout_final_stride_bits(layout, &width)
            || !width || width > UINT16_MAX || !type_index) {
            backend_error(state->diagnostics,
                "referent has no runtime type index or bit width");
            return nullptr;
        }
        bitptr = LLVMGetUndef(bitptr_llvm_type(context));
        bitptr = LLVMBuildInsertValue(state->builder, bitptr, reference, 0,
            "bitptr.from.base");
        bitptr = LLVMBuildInsertValue(state->builder, bitptr,
            runtime_type_index_value(state, referent), 1,
            "bitptr.from.type");
        bitptr = LLVMBuildInsertValue(state->builder, bitptr,
            LLVMConstInt(LLVMInt16TypeInContext(context), 0, false), 2,
            "bitptr.from.start");
        bitptr = LLVMBuildInsertValue(state->builder, bitptr,
            LLVMConstInt(LLVMInt16TypeInContext(context), width, false), 3,
            "bitptr.from.end");
    }
    if (!bitptr) return nullptr;
    if (LLVMTypeOf(bitptr) == target) return bitptr;
    LLVMValueRef record = LLVMGetUndef(target);
    for (unsigned i = 0; i < 4; i++)
        record = LLVMBuildInsertValue(state->builder, record,
            LLVMBuildExtractValue(state->builder, bitptr, i, "bitptr.field"),
            i, "bitptr.record");
    return record;
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

static LLVMValueRef
packed_value_from_bits(lower_state_t *state, LLVMValueRef bits,
                       uint64_t width, th_type *type)
{
    ir_module_t *module = state->function->owner;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(module, type, context);
    if (!bits || !native || !width
        || LLVMGetTypeKind(LLVMTypeOf(bits)) != LLVMIntegerTypeKind)
        return nullptr;
    LLVMValueRef narrow = LLVMBuildIntCast2(state->builder, bits,
        LLVMIntTypeInContext(context, (unsigned)width), false,
        "packed.narrow");
    bool sign = ir_machine_of(module, type).class == IR_CLASS_SINT;
    if (LLVMGetTypeKind(native) == LLVMIntegerTypeKind)
        return LLVMBuildIntCast2(state->builder, narrow, native, sign,
                                 "packed.value");
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    uint64_t bytes = LLVMABISizeOfType(target, native);
    LLVMValueRef slot = entry_alloca(state, native);
    if (!slot || !bytes || bytes > 16) return nullptr;
    LLVMBuildStore(state->builder, LLVMBuildIntCast2(state->builder, narrow,
        LLVMIntTypeInContext(context, (unsigned)(bytes * 8)), sign,
        "packed.storage"), slot);
    return LLVMBuildLoad2(state->builder, native, slot, "packed.value");
}

static LLVMValueRef
packed_bits_from_value(lower_state_t *state, LLVMValueRef value,
                       uint64_t width, LLVMTypeRef wide)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (!value || !width) return nullptr;
    LLVMTypeRef native = LLVMTypeOf(value);
    LLVMValueRef integer = value;
    if (LLVMGetTypeKind(native) != LLVMIntegerTypeKind) {
        LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
        uint64_t bytes = LLVMABISizeOfType(target, native);
        LLVMValueRef slot = entry_alloca(state, native);
        if (!slot || !bytes || bytes > 16) return nullptr;
        LLVMBuildStore(state->builder, value, slot);
        integer = LLVMBuildLoad2(state->builder,
            LLVMIntTypeInContext(context, (unsigned)(bytes * 8)), slot,
            "packed.storage");
    }
    LLVMValueRef narrow = LLVMBuildIntCast2(state->builder, integer,
        LLVMIntTypeInContext(context, (unsigned)width), false,
        "packed.narrow");
    return LLVMBuildIntCast2(state->builder, narrow, wide, false,
                             "packed.bits");
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
    uint64_t width = packed_value_width(state->function->owner, element);
    if (width == 1)
        return packed_value_from_bits(state, load_bool_bitptr(state, bitptr),
                                      width, element);
    if (!width) return nullptr;
    packed_bitptr_io_t io = {0};
    if (!packed_bitptr_prepare(state, bitptr, width, &io)) return nullptr;
    LLVMValueRef bits = LLVMBuildLShr(state->builder,
        LLVMBuildLoad2(state->builder, io.wide, io.scratch,
            "bitptr.load.bits"),
        LLVMBuildZExt(state->builder, io.start, io.wide,
            "bitptr.load.shift"), "bitptr.load.shifted");
    return packed_value_from_bits(state, bits, width, element);
}

static bool
store_packed_bitptr(lower_state_t *state, LLVMValueRef bitptr,
                    LLVMValueRef value, th_type *element)
{
    uint64_t width = packed_value_width(state->function->owner, element);
    if (width == 1)
        return store_bool_bitptr(state, bitptr, packed_bits_from_value(state,
            value, width,
            LLVMInt1TypeInContext(LLVMGetModuleContext(state->module))));
    if (!width || !value) return false;
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
    LLVMValueRef narrowed = packed_bits_from_value(state, value, width,
        io.wide);
    if (!narrowed) return false;
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
    const ir_final_layout_t *layout = ir_layout_final_visible(module, type,
        ir_module_target(module));
    uint64_t stride_bits = 0;
    if (!layout || !native || !target
        || !ir_layout_final_stride_bits(layout, &stride_bits)
        || stride_bits % 8
        || LLVMABISizeOfType(target, native) != stride_bits / 8
        || LLVMABIAlignmentOfType(target, native)
            != layout->alignment_bytes) return false;
    bool variant = false;
    (void)th_named_info(type, nullptr, &variant, nullptr);
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
    if (machine.class == IR_CLASS_BOOL)
        return literal_boolean_field_matches(module, type, native, target);
    th_type *definition = ir_type_definition(module, type);
    if (!definition && (th_kind_of(th_resolve(type)) == TH_RECORD
                        || th_kind_of(th_resolve(type)) == TH_BITINT))
        definition = th_resolve(type);
    if (definition && th_kind_of(definition) == TH_BITINT) {
        uint64_t semantic_bits = 0;
        return th_bitint_info(definition, &semantic_bits, nullptr)
            && semantic_bits && semantic_bits <= layout->intrinsic_bits
            && layout->intrinsic_bits <= 128
            && LLVMGetTypeKind(native) == LLVMIntegerTypeKind
            && (LLVMGetIntTypeWidth(native) == layout->intrinsic_bits
                || (LLVMGetIntTypeWidth(native) == semantic_bits
                    && LLVMABISizeOfType(target, native) * 8
                        == stride_bits));
    }
    if (machine.class == IR_CLASS_SINT || machine.class == IR_CLASS_UINT) {
        bool padded_char = machine.class == IR_CLASS_UINT
            && machine.role == IR_ROLE_CHAR
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

    if (th_kind_of(th_resolve(type)) == TH_SEQUENCE
        && native == box_llvm_type(LLVMGetTypeContext(native)))
        return layout->intrinsic_bits == 128;
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

    if (definition && definition != type
        && th_builtin_kind_of(definition) == TH_BUILTIN_TUPLE)
        return literal_element_layout_matches(module, definition, native,
                                              target);
    size_t count = 0;
    if (!th_named_info(th_resolve(type), nullptr, nullptr, &count)
        && (!definition || th_kind_of(definition) != TH_RECORD))
        return false;
    th_builtin_kind_t builtin = th_builtin_kind_of(type);
    if (builtin == TH_BUILTIN_TUPLE) {
        if (LLVMGetTypeKind(native) != LLVMStructTypeKind
            || LLVMCountStructElementTypes(native) != count) return false;
        for (size_t i = 0; i < count; i++) {
            const th_arg *argument = th_named_arg(th_resolve(type), i);
            if (!argument || argument->kind != TH_ARG_TYPE
                || !literal_element_layout_matches(module, argument->type,
                    LLVMStructGetTypeAtIndex(native, (unsigned)i), target))
                return false;
        }
        return true;
    }
    if (builtin == TH_BUILTIN_ANY && !count)
        return layout->intrinsic_bits == 128
            && native == box_llvm_type(LLVMGetTypeContext(native));
    if (builtin == TH_BUILTIN_ATOMIC && count == 1) {
        const th_arg *argument = th_named_arg(th_resolve(type), 0);
        return argument && argument->kind == TH_ARG_TYPE
            && literal_element_layout_matches(module, argument->type,
                                              native, target);
    }
    if (n00bb_frame_pointer_option(type))
        return LLVMGetTypeKind(native) == LLVMPointerTypeKind
            && layout->intrinsic_bits == sizeof(void *) * 8
            && layout->has_discriminant
            && layout->discriminant_is_niche
            && layout->discriminant.unit_bit_offset == 0
            && layout->discriminant.unit_width_bits == sizeof(void *) * 8;
    if (builtin == TH_BUILTIN_OPTION && count == 1) {
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
    if (builtin == TH_BUILTIN_STRING && !count)
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
        record_shape_t shape;
        if (LLVMGetTypeKind(native) != LLVMStructTypeKind
            || !record_shape_build(module, type, LLVMGetTypeContext(native),
                                   &shape)) return false;
        bool valid = shape.n_elements == LLVMCountStructElementTypes(native);
        for (unsigned e = 0; valid && e < shape.n_elements; e++)
            valid = LLVMStructGetTypeAtIndex(native, e) == shape.elements[e]
                && LLVMOffsetOfElement(target, native, e)
                    == shape.offsets[e];
        for (size_t i = 0; valid && i < layout->n_members; i++) {
            const ir_final_member_layout_t *member = &layout->members[i];
            const record_slot_t *slot = &shape.slots[i];
            if (slot->packed || slot->overlay) continue;
            th_type *field = member->name.data
                ? th_record_lookup(definition, member->name) : nullptr;
            LLVMTypeRef child = shape.elements[slot->element];
            bool inline_match = member->inline_element_count
                && child == inline_member_type(module, member,
                    LLVMGetTypeContext(native))
                && LLVMABISizeOfType(target, LLVMGetElementType(child))
                    == member->inline_element_stride_bits / 8
                && literal_element_layout_matches(module,
                    member->inline_element_type,
                    LLVMGetElementType(child), target);
            valid = field
                && member->bit_offset == shape.offsets[slot->element] * 8
                && (inline_match
                    || literal_element_layout_matches(module, field, child,
                                                      target));
        }
        record_shape_release(&shape);
        return valid;
    }
    if (builtin != TH_BUILTIN_TUPLE
        || LLVMGetTypeKind(native) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(native) != count
        || layout->n_members != count) return false;
    for (size_t i = 0; i < count; i++) {
        const th_arg *argument = th_named_arg(th_resolve(type), i);
        uint64_t offset = LLVMOffsetOfElement(target, native, (unsigned)i);
        if (!argument || argument->kind != TH_ARG_TYPE
            || offset > UINT64_MAX / 8
            || layout->members[i].bit_offset != offset * 8
            || !literal_element_layout_matches(module, argument->type,
                LLVMStructGetTypeAtIndex(native, (unsigned)i), target))
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

static LLVMValueRef copy_sequence_value(lower_state_t *state,
                                        LLVMValueRef value, th_type *type);

static LLVMValueRef
aggregate_operand_value(lower_state_t *state, ir_operand_t *operand)
{
    ir_value_t *source = operand && operand->kind == IR_OPD_VALUE
        ? operand->value : nullptr;
    LLVMValueRef value = source ? lowered_value(state, source) : nullptr;
    return value && th_kind_of(th_resolve(source->type)) == TH_SEQUENCE
        && !n00bb_sequence_transfers_storage(source)
        ? copy_sequence_value(state, value, source->type) : value;
}

static LLVMValueRef
record_byte_run_bits(lower_state_t *state, LLVMValueRef run)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    unsigned bytes = LLVMGetArrayLength(LLVMTypeOf(run));
    LLVMTypeRef wide = LLVMIntTypeInContext(context, bytes * 8);
    LLVMValueRef bits = LLVMConstNull(wide);
    for (unsigned k = 0; k < bytes; k++) {
        LLVMValueRef byte = LLVMBuildZExt(state->builder,
            LLVMBuildExtractValue(state->builder, run, k, "record.run.byte"),
            wide, "record.run.wide");
        bits = LLVMBuildOr(state->builder, bits, LLVMBuildShl(state->builder,
            byte, LLVMConstInt(wide, 8 * (uint64_t)k, false),
            "record.run.shift"), "record.run.bits");
    }
    return bits;
}

static LLVMValueRef
record_byte_run_from_bits(lower_state_t *state, LLVMValueRef bits,
                          LLVMTypeRef run_type)
{
    LLVMTypeRef i8 = LLVMInt8TypeInContext(
        LLVMGetModuleContext(state->module));
    LLVMTypeRef wide = LLVMTypeOf(bits);
    LLVMValueRef run = LLVMConstNull(run_type);
    for (unsigned k = 0; k < LLVMGetArrayLength(run_type); k++)
        run = LLVMBuildInsertValue(state->builder, run,
            LLVMBuildTrunc(state->builder, LLVMBuildLShr(state->builder,
                bits, LLVMConstInt(wide, 8 * (uint64_t)k, false),
                "record.run.byte.shift"), i8, "record.run.byte"),
            k, "record.run.insert");
    return run;
}

static LLVMValueRef entry_alloca(lower_state_t *state, LLVMTypeRef type);

static LLVMValueRef
overlay_member_address(lower_state_t *state, LLVMValueRef storage,
                       const record_slot_t *slot)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(context),
                                       slot->overlay_offset, false);
    return LLVMBuildInBoundsGEP2(state->builder,
        LLVMInt8TypeInContext(context), storage, &offset, 1,
        "record.overlay.member");
}

static bool
large_aggregate(lower_state_t *state, LLVMTypeRef type)
{
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind != LLVMStructTypeKind && kind != LLVMArrayTypeKind) return false;
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    return layout && LLVMABISizeOfType(layout, type) > 64;
}

static LLVMValueRef
aggregate_element(lower_state_t *state, LLVMValueRef aggregate,
                  unsigned element, const char *name)
{
    LLVMTypeRef type = LLVMTypeOf(aggregate);
    if (!LLVMIsALoadInst(aggregate) || LLVMGetVolatile(aggregate)
        || LLVMGetOrdering(aggregate) != LLVMAtomicOrderingNotAtomic
        || LLVMGetTypeKind(type) != LLVMStructTypeKind
        || !large_aggregate(state, type))
        return LLVMBuildExtractValue(state->builder, aggregate, element, name);
    LLVMBasicBlockRef resume = LLVMGetInsertBlock(state->builder);
    LLVMValueRef after = LLVMGetNextInstruction(aggregate);
    if (after) LLVMPositionBuilderBefore(state->builder, after);
    else LLVMPositionBuilderAtEnd(state->builder,
                                  LLVMGetInstructionParent(aggregate));
    LLVMValueRef address = LLVMBuildStructGEP2(state->builder, type,
        LLVMGetOperand(aggregate, 0), element, "record.field.address");
    LLVMValueRef value = LLVMBuildLoad2(state->builder,
        LLVMStructGetTypeAtIndex(type, element), address, name);
    LLVMPositionBuilderAtEnd(state->builder, resume);
    return value;
}

static LLVMValueRef
record_value_get(lower_state_t *state, LLVMValueRef record,
                 const record_slot_t *slot, th_type *type)
{
    LLVMValueRef element = aggregate_element(state, record, slot->element,
                                             "record.field");
    if (!element) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (slot->overlay) {
        LLVMTypeRef member = value_type(state->function->owner, type,
                                        context);
        LLVMValueRef storage = member
            ? entry_alloca(state, LLVMTypeOf(element)) : nullptr;
        if (!storage) return nullptr;
        LLVMBuildStore(state->builder, element, storage);
        return LLVMBuildLoad2(state->builder, member,
            overlay_member_address(state, storage, slot),
            "record.overlay.read");
    }
    if (!slot->packed)
        return ir_machine_of(state->function->owner, type).class
                == IR_CLASS_BOOL
            && LLVMTypeOf(element) == LLVMInt8TypeInContext(context)
            ? LLVMBuildTrunc(state->builder, element,
                LLVMInt1TypeInContext(context), "record.field.bool")
            : element;
    LLVMValueRef bits = record_byte_run_bits(state, element);
    if (slot->shift)
        bits = LLVMBuildLShr(state->builder, bits,
            LLVMConstInt(LLVMTypeOf(bits), slot->shift, false),
            "record.field.shift");
    return packed_value_from_bits(state, bits, slot->width, type);
}

static LLVMValueRef
inline_array_from_value(lower_state_t *state, LLVMValueRef array,
                        LLVMTypeRef storage)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef source = LLVMTypeOf(array);
    if (LLVMGetTypeKind(source) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(source) != 2
        || LLVMStructGetTypeAtIndex(source, 0) != pointer
        || LLVMStructGetTypeAtIndex(source, 1) != pointer) return nullptr;
    LLVMValueRef data = LLVMBuildExtractValue(state->builder, array, 0,
                                              "inline.source.data");
    LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder, array, 1,
                                                    "inline.source.descriptor");
    LLVMValueRef length = load_array_extent(state,
        LLVMBuildStructGEP2(state->builder, array_descriptor_type(context),
                            descriptor, 5, "inline.source.length.address"),
        "inline.source.length");
    emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE, length,
        LLVMConstInt(LLVMInt64TypeInContext(context),
                     LLVMGetArrayLength(storage), false),
        "inline.source.mismatch"));
    return LLVMBuildLoad2(state->builder, storage, data, "inline.source");
}

static LLVMValueRef
record_value_set(lower_state_t *state, LLVMValueRef record,
                 const record_slot_t *slot, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (!record || !value
        || LLVMGetTypeKind(LLVMTypeOf(record)) != LLVMStructTypeKind
        || slot->element >= LLVMCountStructElementTypes(LLVMTypeOf(record)))
        return nullptr;
    LLVMTypeRef storage = LLVMStructGetTypeAtIndex(LLVMTypeOf(record),
                                                   slot->element);
    if (slot->overlay) {
        LLVMValueRef scratch = entry_alloca(state, storage);
        if (!scratch) return nullptr;
        LLVMBuildStore(state->builder, LLVMBuildExtractValue(state->builder,
            record, slot->element, "record.overlay"), scratch);
        LLVMBuildStore(state->builder, value,
                       overlay_member_address(state, scratch, slot));
        return LLVMBuildInsertValue(state->builder, record,
            LLVMBuildLoad2(state->builder, storage, scratch,
                           "record.overlay.updated"),
            slot->element, "record.overlay.set");
    }
    if (!slot->packed) {
        if (storage == LLVMInt8TypeInContext(context)
            && LLVMTypeOf(value) == LLVMInt1TypeInContext(context))
            value = LLVMBuildZExt(state->builder, value, storage,
                                  "record.field.bool");
        if (LLVMGetTypeKind(storage) == LLVMArrayTypeKind
            && LLVMTypeOf(value) != storage)
            value = inline_array_from_value(state, value, storage);
        return value && LLVMTypeOf(value) == storage
            ? LLVMBuildInsertValue(state->builder, record, value,
                slot->element, "record.field.set")
            : nullptr;
    }
    LLVMValueRef bits = record_byte_run_bits(state,
        LLVMBuildExtractValue(state->builder, record, slot->element,
                              "record.run"));
    LLVMTypeRef wide = LLVMTypeOf(bits);
    LLVMValueRef field = packed_bits_from_value(state, value, slot->width,
                                                wide);
    if (!field) return nullptr;
    LLVMValueRef shift = LLVMConstInt(wide, slot->shift, false);
    LLVMValueRef mask = LLVMBuildShl(state->builder,
        LLVMBuildZExt(state->builder, LLVMConstAllOnes(LLVMIntTypeInContext(
            context, (unsigned)slot->width)), wide, "record.field.ones"),
        shift, "record.field.mask");
    bits = LLVMBuildOr(state->builder,
        LLVMBuildAnd(state->builder, bits,
            LLVMBuildNot(state->builder, mask, "record.field.keep"),
            "record.field.cleared"),
        LLVMBuildShl(state->builder, field, shift, "record.field.placed"),
        "record.field.merged");
    return LLVMBuildInsertValue(state->builder, record,
        record_byte_run_from_bits(state, bits, storage), slot->element,
        "record.run.set");
}

static LLVMValueRef
tuple_aggregate(lower_state_t *state, ir_instr_t *instruction)
{
    th_type *type = instruction->dest
        ? th_resolve(instruction->dest->type) : nullptr;
    if (!type || th_builtin_kind_of(type) != TH_BUILTIN_TUPLE) return nullptr;
    size_t count = th_named_nargs(type);
    if (count != instruction->n_operands) {
        state->failure_detail =
            FAILURE_DETAIL("tuple definition and operands disagree");
        return nullptr;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(state->function->owner, type, context);
    if (!cached_literal_element_layout_matches(state, type, native)) {
        state->failure_detail =
            FAILURE_DETAIL("tuple fields differ from finalized layout");
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
    if (!valid) state->failure_detail =
        FAILURE_DETAIL("tuple operand LLVM types disagree");
    return result;
}

static bool
union_member_absent(core_string_t list, uint32_t ordinal)
{
    size_t at = 0;
    while (list.data && at < list.u8_bytes) {
        uint64_t value = 0;
        size_t consumed = 0;
        core_string_parse_u64(core_string_slice(list, at,
            list.u8_bytes - at), 10, &value, &consumed);
        if (!consumed) return false;
        if (value == ordinal) return true;
        at += consumed;
        if (at < list.u8_bytes && list.data[at] == ',') at++;
    }
    return false;
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
    th_named_info(type, nullptr, &variant, nullptr);
    size_t count = definition && th_kind_of(definition) == TH_RECORD
        ? th_record_len(definition) : 0;
    if (!definition || th_kind_of(definition) != TH_RECORD || variant
        || (layout && layout->n_members != count)
        || count != instruction->n_operands || count > UINT32_MAX) {
        state->failure_detail = !definition
            ? FAILURE_DETAIL("record type has no definition")
            : th_kind_of(definition) != TH_RECORD
                ? FAILURE_DETAIL("record definition is not a record")
            : variant ? FAILURE_DETAIL("record literal has a variant type")
            : layout && layout->n_members != count
                ? FAILURE_DETAIL("record layout and definition have different fields")
            : count != instruction->n_operands
                ? FAILURE_DETAIL("record definition and literal have different fields")
                : FAILURE_DETAIL("record has too many fields");
        return nullptr;
    }
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(module, type, context);
    record_shape_t shape;
    bool shaped = layout && native
        && record_shape_build(module, type, context, &shape);
    if (!shaped || native != LLVMStructTypeInContext(context, shape.elements,
            shape.n_elements, false)
        || !cached_literal_element_layout_matches(state, type, native)) {
        state->failure_detail = !layout
            ? FAILURE_DETAIL("record has no finalized layout")
            : !native ? FAILURE_DETAIL("record has no LLVM value type")
            : !shaped ? FAILURE_DETAIL("record layout has no native shape")
            : FAILURE_DETAIL("record LLVM fields differ from finalized layout");
        if (shaped) record_shape_release(&shape);
        return nullptr;
    }
    LLVMValueRef result = LLVMConstNull(native);
    ir_annot_t *absent = ir_annot_get(&instruction->annots,
                                      CORE_STRING_STATIC("n00bb.union_absent"));
    for (size_t i = 0; result && i < count; i++) {
        uint32_t ordinal = layout->members[i].ordinal;
        if (absent && absent->kind == IR_ANNOT_STR
            && union_member_absent(absent->s, ordinal)) continue;
        ir_operand_t *operand = ordinal < count
            ? &instruction->operands[ordinal] : nullptr;
        result = record_value_set(state, result, &shape.slots[i],
            aggregate_operand_value(state, operand));
    }
    record_shape_release(&shape);
    if (!result)
        state->failure_detail = FAILURE_DETAIL(
            "record field values do not match storage types");
    return result;
}

static bool
record_member_slot(lower_state_t *state, th_type *record_type,
                   core_string_t name, LLVMTypeRef *native_out,
                   record_slot_t *slot_out)
{
    record_type = th_resolve(record_type);
    if (!record_type) return false;
    core_list_t(member_match_t) *cache = state->member_matches;
    for (size_t i = 0; cache && i < cache->len; i++) {
        const member_match_t *match = &cache->data[i];
        if (match->type != record_type || !core_string_eq(match->name, name))
            continue;
        if (match->found) {
            *native_out = match->native;
            *slot_out = match->slot;
        }
        return match->found;
    }
    ir_module_t *module = state->function->owner;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(module, record_type, context);
    record_shape_t shape;
    bool shaped = native && record_shape_build(module, record_type, context,
                                               &shape);
    if (shaped && !cached_literal_element_layout_matches(state, record_type,
                                                          native)) {
        state->failure_detail =
            FAILURE_DETAIL("record ABI layout differs from LLVM type");
        record_shape_release(&shape);
        shaped = false;
    }
    member_match_t match = {
        .type = record_type, .name = name, .native = nullptr,
        .slot = {0}, .found = false,
    };
    for (size_t i = 0; shaped && i < shape.layout->n_members; i++) {
        const ir_final_member_layout_t *field = &shape.layout->members[i];
        if (!field->name.data || !core_string_eq(field->name, name)) continue;
        match.native = native;
        match.slot = shape.slots[i];
        match.found = true;
        break;
    }
    if (shaped) record_shape_release(&shape);
    if (cache) core_list_push(*cache, match);
    if (match.found) {
        *native_out = match.native;
        *slot_out = match.slot;
    }
    return match.found;
}

static LLVMValueRef
nominal_record_value(lower_state_t *state, LLVMValueRef source,
                     th_type *source_type, th_type *target_type,
                     th_type *source_shape, th_type *target_shape,
                     LLVMTypeRef target)
{
    size_t count = th_record_len(target_shape);
    if (count != th_record_len(source_shape) || count > UINT32_MAX
        || LLVMGetTypeKind(LLVMTypeOf(source)) != LLVMStructTypeKind
        || LLVMGetTypeKind(target) != LLVMStructTypeKind) return nullptr;
    LLVMValueRef result = LLVMConstNull(target);
    for (size_t i = 0; result && i < count; i++) {
        const th_member *member = th_record_member(target_shape, i);
        th_type *source_field = member && member->name.data
            ? th_record_lookup(source_shape, member->name) : nullptr;
        LLVMTypeRef source_native = nullptr, target_native = nullptr;
        record_slot_t source_slot = {0}, target_slot = {0};
        if (!source_field || !ir_type_eq(state->function->owner,
                source_field, member->type)
            || !record_member_slot(state, source_type, member->name,
                &source_native, &source_slot)
            || !record_member_slot(state, target_type, member->name,
                &target_native, &target_slot)
            || source_native != LLVMTypeOf(source)
            || target_native != target) return nullptr;
        result = record_value_set(state, result, &target_slot,
            record_value_get(state, source, &source_slot, source_field));
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
        || !field->operands[0].value || !field->operands[1].name.data)
        return nullptr;
    ir_module_t *module = state->function->owner;
    th_type *record = th_ind_referent(field->operands[0].value->type);
    const ir_final_layout_t *layout = record
        ? ir_layout_final(module, record, ir_module_target(module)) : nullptr;
    if (!layout) return nullptr;
    for (size_t i = 0; i < layout->n_members; i++) {
        const ir_final_member_layout_t *member = &layout->members[i];
        if (!member->name.data
            || !core_string_eq(member->name, field->operands[1].name))
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
    const ir_final_layout_t *layout = ir_layout_final(source, element,
        ir_module_target(source));
    LLVMTypeRef element_type = value_type(source, element, context);
    LLVMTypeRef result_type = value_type(source, type, context);
    uint64_t stride_bits = 0;
    uint64_t packed_width = packed_array_width(source, element);
    if (!layout || !layout->alignment_bytes || !element_type || !result_type
        || !ir_layout_final_stride_bits(layout, &stride_bits)) {
        state->failure_detail = !layout || !layout->alignment_bytes
            ? FAILURE_DETAIL("array element type has no final layout")
            : FAILURE_DETAIL("array element or array type has no LLVM type");
        return nullptr;
    }
    if (!packed_width && !literal_element_layout_matches(source, element,
            element_type, LLVMGetModuleDataLayout(state->module))) {
        state->failure_detail = FAILURE_DETAIL(
            "array element layout does not match its LLVM type");
        return nullptr;
    }
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
    const char *allocator_name = "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator)
        allocator = LLVMAddFunction(state->module, allocator_name,
                                    allocator_type);
    LLVMValueRef allocator_args[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        runtime_type_index_value(state, type),
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
        item = escape_boxes(state, item);
        if (!item || !store_gc_barrier(state, item, element)) return nullptr;
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
    return boxed_result ? box_value_with(state, array, type,
                                         box_in_frame(state, instruction))
        : array;
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
    const char *allocator_name = "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        runtime_type_index_value(state, type), bytes,
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
        const ir_final_layout_t *layout = ir_layout_final_visible(module,
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
    const char *allocator_name = "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
        allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
        allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        runtime_type_index_value(state, child),
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

static LLVMValueRef copy_boxed_array(lower_state_t *state,
                                     LLVMValueRef value, bool in_frame);

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
        return copy_boxed_array(state, value, false);
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
    const char *allocator_name = "n00b_allocate";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                   allocator_name);
    if (!allocator) allocator = LLVMAddFunction(state->module,
                                                 allocator_name, signature);
    if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
        return nullptr;
    LLVMValueRef arguments[] = {
        LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
        runtime_type_index_value(state, type), extent,
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
            th_name(ir_module_types(module), CORE_STRING_STATIC("bool")));
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
    return th_builtin_applied(type, TH_BUILTIN_STRING, 0) != nullptr;
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
        || ir_annot_get(&instruction->annots,
                        CORE_STRING_STATIC("n00bb.rich.metadata")))
        return nullptr;
    ir_annot_t *modifier = ir_annot_get(&instruction->annots,
        CORE_STRING_STATIC("n00bb.literal.modifier"));
    uint64_t encoding = modifier && modifier->kind == IR_ANNOT_STR
        && modifier->s.data
        && core_string_eq(modifier->s, CORE_STRING_STATIC("binary"))
        ? N00B_ABI_STRING_BINARY
        : default_string_encoding((const char *)bytes, length);
    return string_value(state->module, bytes, length, encoding);
}

typedef enum : uint8_t {
    OPTION_REPR_POINTER,
    OPTION_REPR_PACKED,
    OPTION_REPR_SHARED,
    OPTION_REPR_TAGGED,
} option_repr_kind_t;

typedef struct {
    option_repr_kind_t kind;
    th_type *payload_type;
    LLVMTypeRef storage;
    LLVMTypeRef payload;
    uint64_t presence_bit;
    uint8_t presence_mask;
} option_repr_t;

static bool
option_repr(ir_module_t *module, th_type *type, LLVMContextRef context,
            option_repr_t *out)
{
    th_type *payload_type = type ? option_payload_type(type) : nullptr;
    LLVMTypeRef storage = payload_type
        ? value_type(module, type, context) : nullptr;
    if (!storage || !out) return false;
    *out = (option_repr_t){
        .payload_type = payload_type,
        .storage = storage,
        .payload = value_type(module, payload_type, context),
        .presence_mask = 1,
    };
    if (LLVMGetTypeKind(storage) == LLVMPointerTypeKind) {
        out->kind = OPTION_REPR_POINTER;
        return true;
    }
    uint64_t bit = 0;
    if (packed_option_type(module, type, context, &bit) == storage) {
        out->kind = OPTION_REPR_PACKED;
        out->presence_bit = bit;
        return true;
    }
    if (LLVMGetTypeKind(storage) != LLVMStructTypeKind
        || LLVMCountStructElementTypes(storage) != 2) return false;
    uint8_t mask = 0;
    if (shared_option_value_type(module, type, context, &mask) == storage) {
        out->kind = OPTION_REPR_SHARED;
        out->presence_mask = mask;
        return true;
    }
    out->kind = OPTION_REPR_TAGGED;
    return true;
}

static bool
constant_image(LLVMTargetDataRef target, LLVMValueRef value,
               uint8_t *bytes, uint64_t size)
{
    LLVMTypeRef type = LLVMTypeOf(value);
    uint64_t extent = LLVMStoreSizeOfType(target, type);
    if (extent > size) return false;
    if (LLVMIsNull(value)) return true;
    switch (LLVMGetTypeKind(type)) {
    case LLVMIntegerTypeKind: {
        unsigned width = LLVMGetIntTypeWidth(type);
        if (width > 64) return false;
        uint64_t bits = LLVMConstIntGetZExtValue(value);
        for (uint64_t b = 0; b < extent && b < 8; b++)
            bytes[b] = (uint8_t)(bits >> (b * 8));
        return true;
    }
    case LLVMStructTypeKind: {
        unsigned count = LLVMCountStructElementTypes(type);
        for (unsigned i = 0; i < count; i++) {
            uint64_t offset = LLVMOffsetOfElement(target, type, i);
            LLVMValueRef element = LLVMGetAggregateElement(value, i);
            if (!element || offset > size
                || !constant_image(target, element, bytes + offset,
                                   size - offset)) return false;
        }
        return true;
    }
    case LLVMArrayTypeKind: {
        LLVMTypeRef element_type = LLVMGetElementType(type);
        uint64_t stride = LLVMABISizeOfType(target, element_type);
        unsigned count = LLVMGetArrayLength(type);
        for (unsigned i = 0; i < count; i++) {
            LLVMValueRef element = LLVMGetAggregateElement(value, i);
            if (!element || i * stride > size
                || !constant_image(target, element, bytes + i * stride,
                                   size - i * stride)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

static LLVMValueRef
constant_struct_like(LLVMTypeRef type, LLVMValueRef *fields, unsigned count)
{
    return LLVMGetStructName(type)
        ? LLVMConstNamedStruct(type, fields, count)
        : LLVMConstStructInContext(LLVMGetTypeContext(type), fields, count,
                                   LLVMIsPackedStruct(type));
}

static LLVMValueRef
option_some_constant(LLVMModuleRef module, const option_repr_t *repr,
                     LLVMValueRef payload)
{
    if (!repr || !payload) return nullptr;
    switch (repr->kind) {
    case OPTION_REPR_POINTER:
        return LLVMTypeOf(payload) == repr->storage ? payload : nullptr;
    case OPTION_REPR_SHARED: {
        if (LLVMTypeOf(payload) != repr->storage) return nullptr;
        LLVMValueRef tag = LLVMGetAggregateElement(payload, 1);
        LLVMValueRef fields[] = {
            LLVMGetAggregateElement(payload, 0),
            tag ? LLVMConstInt(LLVMTypeOf(tag),
                (LLVMConstIntGetZExtValue(tag) | repr->presence_mask) & 0xffu,
                false) : nullptr,
        };
        return fields[0] && fields[1]
            ? constant_struct_like(repr->storage, fields, 2) : nullptr;
    }
    case OPTION_REPR_TAGGED: {
        if (LLVMStructGetTypeAtIndex(repr->storage, 0) != LLVMTypeOf(payload))
            return nullptr;
        LLVMValueRef fields[] = {
            payload,
            LLVMConstInt(LLVMStructGetTypeAtIndex(repr->storage, 1),
                         repr->presence_mask, false),
        };
        return constant_struct_like(repr->storage, fields, 2);
    }
    case OPTION_REPR_PACKED: {
        LLVMTargetDataRef target = LLVMGetModuleDataLayout(module);
        uint64_t size = LLVMStoreSizeOfType(target, repr->storage);
        if (!size || repr->presence_bit >= size * 8) return nullptr;
        uint64_t words_count = (size + 7) / 8;
        uint64_t *words = calloc(words_count, sizeof(*words));
        uint8_t *bytes = calloc(words_count * 8, 1);
        bool valid = words && bytes
            && constant_image(target, payload, bytes, size);
        LLVMValueRef result = nullptr;
        if (valid) {
            bytes[repr->presence_bit / 8] |=
                (uint8_t)(1u << (repr->presence_bit % 8));
            for (uint64_t w = 0; w < words_count; w++)
                for (unsigned b = 0; b < 8; b++)
                    words[w] |= (uint64_t)bytes[w * 8 + b] << (b * 8);
            if (LLVMGetTypeKind(repr->storage) == LLVMIntegerTypeKind)
                result = LLVMConstIntOfArbitraryPrecision(repr->storage,
                    (unsigned)words_count, words);
            else if (LLVMGetTypeKind(repr->storage) == LLVMArrayTypeKind) {
                LLVMTypeRef unit = LLVMGetElementType(repr->storage);
                unsigned unit_bytes = LLVMGetTypeKind(unit)
                    == LLVMIntegerTypeKind
                    ? LLVMGetIntTypeWidth(unit) / 8 : 0;
                unsigned count = LLVMGetArrayLength(repr->storage);
                LLVMValueRef *elements = unit_bytes && unit_bytes <= 8
                    && (uint64_t)count * unit_bytes == size
                    ? calloc(count, sizeof(*elements)) : nullptr;
                if (elements) {
                    for (unsigned e = 0; e < count; e++) {
                        uint64_t value = 0;
                        for (unsigned b = 0; b < unit_bytes; b++)
                            value |= (uint64_t)bytes[e * unit_bytes + b]
                                << (b * 8);
                        elements[e] = LLVMConstInt(unit, value, false);
                    }
                    result = LLVMConstArray(unit, elements, count);
                    free(elements);
                }
            }
        }
        free(words);
        free(bytes);
        return result;
    }
    }
    return nullptr;
}

static LLVMValueRef
static_packed_array_value(lower_state_t *state, ir_module_t *source,
                          th_type *element,
                          const ir_global_init_value_t *init,
                          uint64_t packed_width, bool constant)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef element_type = value_type(source, element, context);
    uint64_t count = init->aggregate.count;
    if (!element_type || LLVMGetTypeKind(element_type) != LLVMIntegerTypeKind
        || packed_width > 64 || count > UINT32_MAX
        || count > (UINT64_MAX - 7) / packed_width) return nullptr;
    uint64_t payload_bytes = (count * packed_width + 7) / 8;
    uint8_t *bytes = calloc(payload_bytes ? payload_bytes : 1, 1);
    if (!bytes) return nullptr;
    ir_machine_t machine = ir_machine_of(source, element);
    bool valid = true;
    for (uint64_t i = 0; valid && i < count; i++) {
        const ir_global_init_value_t *item = &init->aggregate.items[i];
        LLVMValueRef constant_item = item->kind == IR_GLOBAL_INIT_BOOL
            ? integer_constant(machine, element_type,
                               (ir_lit_t){.mag = item->boolean})
            : item->kind == IR_GLOBAL_INIT_INT
                ? integer_constant(machine, element_type, item->integer)
                : nullptr;
        valid = constant_item != nullptr;
        if (!valid) break;
        uint64_t bits = LLVMConstIntGetZExtValue(constant_item);
        if (packed_width < 64) bits &= (UINT64_C(1) << packed_width) - 1;
        for (uint64_t b = 0; b < packed_width; b++)
            if ((bits >> b) & 1) {
                uint64_t position = i * packed_width + b;
                bytes[position >> 3] |= (uint8_t)(1u << (position & 7));
            }
    }
    if (!valid) {
        free(bytes);
        return nullptr;
    }
    LLVMValueRef contents = LLVMConstStringInContext(context,
        (const char *)bytes, (unsigned)(payload_bytes ? payload_bytes : 1),
        true);
    free(bytes);
    LLVMValueRef data = LLVMAddGlobal(state->module, LLVMTypeOf(contents),
                                      "n00bb.static.packed.array");
    LLVMSetInitializer(data, contents);
    LLVMSetAlignment(data, 8);
    LLVMSetGlobalConstant(data, constant);
    LLVMSetLinkage(data, LLVMPrivateLinkage);
    return array_value_with_descriptor(state, data, count);
}

static LLVMValueRef
static_init_value(lower_state_t *state, ir_module_t *source, th_type *type,
                  const ir_global_init_value_t *init, bool constant);

static const ir_global_init_value_t *
static_record_item(const ir_global_init_value_t *init, core_string_t member)
{
    for (size_t i = 0; i < init->aggregate.count; i++) {
        const ir_global_init_value_t *item = &init->aggregate.items[i];
        if (item->member.data && member.data
            && core_string_eq(item->member, member))
            return item;
    }
    return nullptr;
}

static LLVMValueRef
static_inline_array_value(lower_state_t *state, ir_module_t *source,
                          th_type *array_type, LLVMTypeRef field_type,
                          const ir_global_init_value_t *item, bool constant)
{
    th_type *element = th_kind_of(th_resolve(array_type)) == TH_SEQUENCE
        ? th_seq_elem(th_resolve(array_type)) : nullptr;
    if (!element || LLVMGetTypeKind(field_type) != LLVMArrayTypeKind
        || item->kind != IR_GLOBAL_INIT_SEQUENCE
        || item->aggregate.count != LLVMGetArrayLength(field_type))
        return nullptr;
    LLVMTypeRef element_type = LLVMGetElementType(field_type);
    unsigned count = LLVMGetArrayLength(field_type);
    LLVMValueRef *items = calloc(count ? count : 1, sizeof(*items));
    if (!items) return nullptr;
    bool valid = true;
    for (unsigned i = 0; valid && i < count; i++) {
        items[i] = static_init_value(state, source, element,
                                     &item->aggregate.items[i], constant);
        valid = items[i] && LLVMTypeOf(items[i]) == element_type;
    }
    LLVMValueRef value = valid
        ? LLVMConstArray(element_type, items, count) : nullptr;
    free(items);
    return value;
}

static bool
constant_bytes(LLVMTargetDataRef target, LLVMValueRef value, uint8_t *bytes,
               uint64_t length)
{
    if (!value) return false;
    if (LLVMIsNull(value) || LLVMIsUndef(value)) return true;
    LLVMTypeRef type = LLVMTypeOf(value);
    if (LLVMIsAConstantInt(value)) {
        unsigned width = LLVMGetIntTypeWidth(type);
        if (width > 64) return false;
        uint64_t bits = LLVMConstIntGetZExtValue(value);
        for (uint64_t b = 0; b < (width + 7) / 8 && b < length; b++)
            bytes[b] = (uint8_t)(bits >> (8 * b));
        return true;
    }
    bool record = LLVMGetTypeKind(type) == LLVMStructTypeKind;
    if (!record && LLVMGetTypeKind(type) != LLVMArrayTypeKind) return false;
    unsigned count = record ? LLVMCountStructElementTypes(type)
                            : LLVMGetArrayLength(type);
    uint64_t stride = record ? 0
        : LLVMABISizeOfType(target, LLVMGetElementType(type));
    for (unsigned i = 0; i < count; i++) {
        uint64_t offset = record ? LLVMOffsetOfElement(target, type, i)
                                 : stride * i;
        if (offset > length || !constant_bytes(target,
                LLVMGetAggregateElement(value, i), bytes + offset,
                length - offset)) return false;
    }
    return true;
}

static bool
static_packed_field(LLVMTargetDataRef target, LLVMValueRef value,
                    const record_slot_t *slot, uint8_t *run)
{
    uint64_t size = value ? LLVMABISizeOfType(target, LLVMTypeOf(value)) : 0;
    uint8_t *bits = size ? calloc(size, 1) : nullptr;
    bool valid = bits && slot->width <= size * 8
        && constant_bytes(target, value, bits, size);
    for (uint64_t b = 0; valid && b < slot->width; b++)
        if ((bits[b >> 3] >> (b & 7)) & 1) {
            uint64_t at = slot->shift + b;
            run[at >> 3] |= (uint8_t)(1u << (at & 7));
        }
    free(bits);
    return valid;
}

static LLVMValueRef
static_record_value(lower_state_t *state, ir_module_t *source, th_type *type,
                    LLVMTypeRef native, const ir_global_init_value_t *init,
                    bool constant)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
    record_shape_t shape;
    if (LLVMGetTypeKind(native) != LLVMStructTypeKind
        || !record_shape_build(source, type, context, &shape)) return nullptr;
    unsigned count = shape.n_elements;
    LLVMValueRef *fields = calloc(count ? count : 1, sizeof(*fields));
    uint8_t **runs = calloc(count ? count : 1, sizeof(*runs));
    bool valid = fields && runs
        && count == LLVMCountStructElementTypes(native);
    for (unsigned e = 0; valid && e < count; e++)
        if (shape.byte_runs[e]) {
            unsigned length = LLVMGetArrayLength(shape.elements[e]);
            runs[e] = calloc(length ? length : 1, 1);
            valid = runs[e] != nullptr;
        }
    for (size_t i = 0; valid && i < shape.layout->n_members; i++) {
        const ir_final_member_layout_t *member = &shape.layout->members[i];
        const record_slot_t *slot = &shape.slots[i];
        const ir_global_init_value_t *item = static_record_item(init,
                                                                member->name);
        if (!item) continue;
        if (slot->overlay) {
            valid = false;
            break;
        }
        if (slot->packed) {
            valid = static_packed_field(target, static_init_value(state,
                source, record_member_type(&shape, member), item, constant),
                slot, runs[slot->element]);
            continue;
        }
        LLVMTypeRef field_type = shape.elements[slot->element];
        LLVMValueRef value = member->inline_element_count
            ? static_inline_array_value(state, source, member->type,
                                        field_type, item, constant)
            : static_init_value(state, source, member->type, item, constant);
        if (value && LLVMTypeOf(value) != field_type
            && LLVMGetTypeKind(field_type) == LLVMIntegerTypeKind
            && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMIntegerTypeKind
            && LLVMGetIntTypeWidth(LLVMTypeOf(value)) == 1)
            value = LLVMConstInt(field_type, LLVMConstIntGetZExtValue(value),
                                 false);
        valid = value && LLVMTypeOf(value) == field_type;
        fields[slot->element] = value;
    }
    for (unsigned e = 0; valid && e < count; e++) {
        if (runs[e])
            fields[e] = LLVMConstStringInContext(context, (const char *)runs[e],
                LLVMGetArrayLength(shape.elements[e]), true);
        else if (!fields[e]) fields[e] = LLVMConstNull(shape.elements[e]);
        valid = fields[e] && LLVMTypeOf(fields[e]) == shape.elements[e];
    }
    LLVMValueRef record = valid ? constant_struct_like(native, fields, count)
                                : nullptr;
    for (unsigned e = 0; runs && e < count; e++) free(runs[e]);
    free(runs);
    free(fields);
    record_shape_release(&shape);
    return record;
}

static LLVMValueRef
static_init_value(lower_state_t *state, ir_module_t *source, th_type *type,
                  const ir_global_init_value_t *init, bool constant)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    type = type ? th_resolve(type) : nullptr;
    LLVMTypeRef native = type ? value_type(source, type, context) : nullptr;
    if (!native || !init) return nullptr;
    if (init->kind != IR_GLOBAL_INIT_NIL && option_payload_type(type)) {
        option_repr_t repr;
        if (!option_repr(source, type, context, &repr)) return nullptr;
        LLVMValueRef payload = static_init_value(state, source,
            repr.payload_type, init, constant);
        return payload
            ? option_some_constant(state->module, &repr, payload) : nullptr;
    }
    if (init->kind == IR_GLOBAL_INIT_RECORD)
        return static_record_value(state, source, type, native, init,
                                   constant);
    ir_machine_t machine = ir_machine_of(source, type);
    switch (init->kind) {
    case IR_GLOBAL_INIT_INT:
        return integer_constant(machine, native, init->integer);
    case IR_GLOBAL_INIT_BOOL:
        return integer_constant(machine, native,
                                (ir_lit_t){.mag = init->boolean});
    case IR_GLOBAL_INIT_FLOAT:
        return machine.class == IR_CLASS_FLOAT && floating_type(native)
            ? LLVMConstReal(native, init->floating) : nullptr;
    case IR_GLOBAL_INIT_STRING:
        return is_string_type(type)
            ? string_value(state->module, (const uint8_t *)init->string.data,
                init->string.u8_bytes,
                default_string_encoding(init->string.data,
                                        init->string.u8_bytes))
            : nullptr;
    case IR_GLOBAL_INIT_NIL:
        return LLVMConstNull(native);
    case IR_GLOBAL_INIT_SEQUENCE:
        break;
    default:
        return nullptr;
    }
    if (th_kind_of(type) != TH_SEQUENCE
        || init->aggregate.count > UINT32_MAX) return nullptr;
    th_type *element = th_resolve(th_seq_elem(type));
    const ir_final_layout_t *layout = element
        ? ir_layout_final(source, element, ir_module_target(source)) : nullptr;
    LLVMTypeRef element_type = element
        ? value_type(source, element, context) : nullptr;
    uint64_t packed_width = element ? packed_array_width(source, element) : 0;
    if (packed_width)
        return static_packed_array_value(state, source, element, init,
                                         packed_width, constant);
    if (!layout || !layout->alignment_bytes
        || !literal_element_layout_matches(source, element, element_type,
            LLVMGetModuleDataLayout(state->module))) return nullptr;
    LLVMTypeRef storage_type = element_type;
    unsigned count = (unsigned)init->aggregate.count;
    LLVMValueRef *items = calloc(count ? count : 1, sizeof(*items));
    if (!items) return nullptr;
    bool valid = true;
    for (unsigned i = 0; valid && i < count; i++) {
        items[i] = static_init_value(state, source, element,
                                     &init->aggregate.items[i], constant);
        valid = items[i] && LLVMTypeOf(items[i]) == element_type;
    }
    if (!valid) {
        free(items);
        return nullptr;
    }
    if (!count) items[0] = LLVMConstNull(storage_type);
    LLVMValueRef values = LLVMConstArray(storage_type, items,
                                          count ? count : 1);
    free(items);
    LLVMValueRef data = LLVMAddGlobal(state->module, LLVMTypeOf(values),
                                      "n00bb.static.array");
    LLVMSetInitializer(data, values);
    LLVMSetAlignment(data, (unsigned)layout->alignment_bytes);
    LLVMSetGlobalConstant(data, constant);
    LLVMSetLinkage(data, LLVMPrivateLinkage);
    LLVMValueRef array = array_value_with_descriptor(state, data, count);
    return array && LLVMTypeOf(array) == native ? array : nullptr;
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

static LLVMValueRef
mutator_allocation(lower_state_t *state, LLVMValueRef type_index, uint64_t bytes,
                   uint32_t alignment, bool finalizable)
{
    LLVMValueRef mutator = allocation_mutator(state);
    if (!mutator) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i1 = LLVMInt1TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef params[] = {pointer, i32, i64, i32, i1, pointer, pointer};
    LLVMTypeRef allocator_type = LLVMFunctionType(pointer, params, 7, false);
    const char *name =
        "sys.internal.gc.alloc.allocate_runtime_known_on_mutator$gc_mutator";
    LLVMValueRef allocator = LLVMGetNamedFunction(state->module, name);
    if (!allocator)
        allocator = LLVMAddFunction(state->module, name, allocator_type);
    LLVMValueRef args[] = {
        mutator,
        type_index,
        LLVMConstInt(i64, bytes, false),
        LLVMConstInt(i32, alignment, false),
        LLVMConstInt(i1, finalizable, false),
        LLVMConstNull(pointer),
        mutator,
    };
    LLVMValueRef result = LLVMBuildCall2(state->builder, allocator_type,
                                         allocator, args, 7, "storage");
    emit_trap_if(state, LLVMBuildIsNull(state->builder, result,
                                        "allocation.failed"));
    return result;
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

static LLVMValueRef
index_slot_value(lower_state_t *state, bool found, size_t slot,
                 uint32_t value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    if (!value) return nullptr;
    index_slots_t *slots = state->index_slots;
    if (!slots || !slots->binding) return LLVMConstInt(i32, value, false);
    if (!found) {
        state->failure_detail = FAILURE_DETAIL(
            "a value needs an index the module's index table does not hold");
        return nullptr;
    }
    if (!slots->table) {
        slots->table = named_global(state->module, slots->binding->symbol);
        if (!slots->table)
            slots->table = add_global(state->module,
                LLVMArrayType(i32, 0), slots->binding->symbol);
        LLVMSetGlobalConstant(slots->table, true);
    }
    LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(context), slot,
                                       false);
    LLVMValueRef address = LLVMBuildInBoundsGEP2(state->builder, i32,
        slots->table, &offset, 1, "index.slot");
    LLVMValueRef loaded = LLVMBuildLoad2(state->builder, i32, address,
                                         "index");
    unsigned invariant = LLVMGetMDKindIDInContext(context, "invariant.load",
                                                  14);
    LLVMSetMetadata(loaded, invariant, LLVMMDNodeInContext(context, nullptr,
                                                           0));
    return loaded;
}

static LLVMValueRef
runtime_type_index_value(lower_state_t *state, th_type *type)
{
    const n00bb_llvm_index_table_t *table = state->index_slots
        ? state->index_slots->binding : nullptr;
    uint64_t hash = th_hash(ir_module_types(state->function->owner), type);
    size_t slot = 0;
    while (table && slot < table->type_count
           && table->type_hashes[slot] != hash) slot++;
    return index_slot_value(state, table && slot < table->type_count, slot,
                            runtime_type_index(state, type));
}

static LLVMValueRef
runtime_function_index_value(lower_state_t *state, const ir_func_t *function)
{
    const n00bb_llvm_index_table_t *table = state->index_slots
        ? state->index_slots->binding : nullptr;
    size_t slot = 0;
    while (table && slot < table->function_count
           && table->functions[slot] != function) slot++;
    return index_slot_value(state, table && slot < table->function_count,
                            table ? table->type_count + slot : 0,
                            runtime_function_index(state, function));
}

static LLVMValueRef
runtime_type_component(lower_state_t *state, LLVMValueRef type_index,
                       uint32_t ordinal)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef parameters[] = {i32, i32};
    if (!type_index) return nullptr;
    LLVMValueRef args[] = {type_index, LLVMConstInt(i32, ordinal, false)};
    return box_runtime_call(state, "sys.internal.box.type_component", i32,
                            parameters, args, 2);
}

static LLVMValueRef variable_from_inputs(lower_state_t *state,
                                         th_type *variable);

static bool
type_contains_variable(th_type *pattern, th_type *variable)
{
    pattern = th_resolve(pattern);
    variable = th_resolve(variable);
    if (!pattern || !variable) return false;
    if (th_kind_of(pattern) == TH_VAR) return pattern == variable;
    if (th_kind_of(pattern) == TH_SEQUENCE)
        return type_contains_variable(th_seq_elem(pattern), variable);
    if (th_kind_of(pattern) == TH_INDIRECTION)
        return type_contains_variable(th_ind_referent(pattern), variable);
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
    if (th_kind_of(pattern) == TH_SEQUENCE
        || th_kind_of(pattern) == TH_INDIRECTION) {
        th_type *child = th_kind_of(pattern) == TH_SEQUENCE
            ? th_seq_elem(pattern) : th_ind_referent(pattern);
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
            state->parameter_base + (unsigned)native++);
        if (!th_mode_accepts_input(formal->mode)
            || !type_contains_variable(formal->type, variable)
            || value_type(state->function->owner, formal->type, context)
                != box) continue;
        if (th_mode_produces_output(formal->mode))
            input = LLVMBuildLoad2(state->builder, box, input,
                                   "generic.input.box");
        LLVMValueRef field = LLVMBuildExtractValue(state->builder, input, 2,
                                                    "generic.input.field");
        LLVMValueRef actual = LLVMBuildAnd(state->builder, field,
            LLVMConstInt(LLVMTypeOf(field), UINT32_C(0x7fffffff), false),
            "generic.input.type");
        return variable_from_pattern(state, formal->type, variable, actual);
    }
    th_type *success = th_func_ret(state->function->type);
    if (!state->result_box || !success
        || !type_contains_variable(success, variable)) return nullptr;
    LLVMValueRef result = LLVMBuildLoad2(state->builder, box,
        state->result_box, "generic.result.box");
    LLVMValueRef field = LLVMBuildExtractValue(state->builder, result, 2,
                                               "generic.result.field");
    LLVMValueRef actual = LLVMBuildAnd(state->builder, field,
        LLVMConstInt(LLVMTypeOf(field), UINT32_C(0x7fffffff), false),
        "generic.result.type");
    return variable_from_pattern(state, success, variable, actual);
}

static LLVMValueRef
runtime_type_value(lower_state_t *state, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    type = th_resolve(type);
    if (th_is_closed(ir_module_types(state->function->owner), type)) {
        return runtime_type_index_value(state, type);
    }
    if (th_kind_of(type) == TH_VAR)
        return variable_from_inputs(state, type);
    if (th_kind_of(type) == TH_SEQUENCE) {
        LLVMValueRef element = runtime_type_value(state, th_seq_elem(type));
        LLVMTypeRef parameters[] = {i32};
        return element ? box_runtime_call(state,
            "sys.internal.box.array_type_of", i32, parameters, &element, 1)
            : nullptr;
    }
    if (th_kind_of(type) == TH_INDIRECTION) {
        LLVMValueRef element = runtime_type_value(state,
            th_ind_referent(type));
        LLVMTypeRef parameters[] = {i32};
        return element ? box_runtime_call(state,
            "sys.internal.box.reference_type_of", i32, parameters, &element,
            1) : nullptr;
    }
    core_string_t name = {0};
    size_t count = 0;
    if (!th_named_info(type, &name, nullptr, &count)
        || !name.data || !count || count > UINT32_MAX) return nullptr;
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
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef parameters[] = {pointer, i64, pointer, i32};
    LLVMValueRef args[] = {global_string_ptr(state->builder, name,
        "generic.constructor"), LLVMConstInt(i64, name.u8_bytes, false),
        values, LLVMConstInt(i32, count, false)};
    return box_runtime_call(state, "sys.internal.box.named_type", i32,
                            parameters, args, 4);
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
native_call_outcome_within(ir_module_t *module, const ir_value_t *value,
                           unsigned depth)
{
    const ir_instr_t *instruction = value ? value->def : nullptr;
    th_type *signature = nullptr;
    if (!instruction || depth > 64) return false;
    if (instruction->op == IR_OP_COPY || instruction->op == IR_OP_PHI) {
        bool any = false;
        for (uint32_t i = 0; i < instruction->n_operands; i++) {
            const ir_operand_t *operand = &instruction->operands[i];
            const ir_value_t *source = operand->kind == IR_OPD_VALUE
                ? operand->value
                : operand->kind == IR_OPD_PHI_ARM ? operand->phi.value
                : nullptr;
            if (!source) continue;
            if (!native_call_outcome_within(module, source, depth + 1))
                return false;
            any = true;
        }
        return any;
    }
    if (instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE)
        signature = ir_instr_call_signature(instruction);
    else if (instruction->op == IR_OP_CALLBACK_CREATE)
        ir_instr_callback_contract(instruction, nullptr, nullptr, &signature);
    th_type *failure = signature ? th_func_error(signature) : nullptr;
    return failure && ir_machine_of(module, failure).class != IR_CLASS_VOID;
}

static bool
native_call_outcome(ir_module_t *module, const ir_value_t *value)
{
    return native_call_outcome_within(module, value, 0);
}

static th_type *
native_call_outcome_signature(const ir_value_t *value, unsigned depth)
{
    const ir_instr_t *instruction = value ? value->def : nullptr;
    if (!instruction || depth > 64) return nullptr;
    if (instruction->op == IR_OP_COPY || instruction->op == IR_OP_PHI) {
        for (uint32_t i = 0; i < instruction->n_operands; i++) {
            const ir_operand_t *operand = &instruction->operands[i];
            const ir_value_t *source = operand->kind == IR_OPD_VALUE
                ? operand->value
                : operand->kind == IR_OPD_PHI_ARM ? operand->phi.value
                : nullptr;
            th_type *signature = source
                ? native_call_outcome_signature(source, depth + 1) : nullptr;
            if (signature) return signature;
        }
        return nullptr;
    }
    th_type *signature = nullptr;
    if (instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE)
        signature = ir_instr_call_signature(instruction);
    else if (instruction->op == IR_OP_CALLBACK_CREATE)
        ir_instr_callback_contract(instruction, nullptr, nullptr, &signature);
    return signature;
}

static bool
store_gc_barrier(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    if (!type) return backend_error(state->diagnostics,
        "GC barrier store has no value type");
    th_type *atomic_element = atomic_element_type(type);
    ir_class_t category = ir_machine_of(state->function->owner,
                                       atomic_element ? atomic_element : type)
                              .class;
    if (category == IR_CLASS_VOID || category == IR_CLASS_BOOL
        || category == IR_CLASS_SINT || category == IR_CLASS_UINT
        || category == IR_CLASS_FLOAT
        || n00bb_frame_pointer_free_value(state->function->owner,
               atomic_element ? atomic_element : type)) return true;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    if (LLVMTypeOf(value) == box_llvm_type(context)) {
        LLVMValueRef slot = entry_alloca(state, box_llvm_type(context));
        if (!slot) return false;
        LLVMBuildStore(state->builder, value, slot);
        LLVMTypeRef parameters[] = {LLVMPointerTypeInContext(context, 0)};
        return box_runtime_call(state, "sys.internal.gc.box_store.barrier_box",
            LLVMVoidTypeInContext(context), parameters, &slot, 1) != nullptr;
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
    th_named_info(th_resolve(type), nullptr, &variant, nullptr);
    if (!variant && definition
        && th_kind_of(definition) == TH_RECORD
        && LLVMGetTypeKind(LLVMTypeOf(value)) == LLVMStructTypeKind) {
        record_shape_t shape;
        if (record_shape_build(state->function->owner, type, context,
                               &shape)) {
            bool supported = true;
            for (size_t i = 0; supported && i < shape.layout->n_members; i++) {
                const ir_final_member_layout_t *member =
                    &shape.layout->members[i];
                supported = member->name.data && !member->inline_element_count
                    && th_record_lookup(definition, member->name);
            }
            for (size_t i = 0; supported && i < shape.layout->n_members;
                 i++) {
                if (shape.slots[i].packed || shape.slots[i].overlay) continue;
                th_type *field = th_record_lookup(definition,
                    shape.layout->members[i].name);
                th_type *atomic_field = atomic_element_type(field);
                ir_class_t category = ir_machine_of(state->function->owner,
                    atomic_field ? atomic_field : field).class;
                if (category == IR_CLASS_VOID || category == IR_CLASS_BOOL
                    || category == IR_CLASS_SINT
                    || category == IR_CLASS_UINT
                    || category == IR_CLASS_FLOAT
                    || n00bb_frame_pointer_free_value(
                           state->function->owner,
                           atomic_field ? atomic_field : field)) continue;
                LLVMValueRef part = aggregate_element(state, value,
                    shape.slots[i].element, "barrier.field");
                if (!part || !store_gc_barrier(state, part, field)) {
                    record_shape_release(&shape);
                    return false;
                }
            }
            record_shape_release(&shape);
            if (supported) return true;
        }
    }
    uint32_t index = runtime_type_index(state, type);
    if (!index) {
        core_buffer_t message = {0};
        core_string_t printed = th_print(
            ir_module_types(state->function->owner), type);
        core_buffer_printf(&message,
            "GC barrier store in '%.*s' has no ABI type index for %.*s",
            TEXT_ARGS(state->function->name), TEXT_ARGS(printed));
        bool reported = backend_report(state->diagnostics,
                                       core_buffer_string(&message));
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
    LLVMValueRef args[] = {slot, runtime_type_index_value(state, type)};
    return LLVMBuildCall2(state->builder, signature, barrier, args, 2,
                          "") != nullptr;
}

static LLVMValueRef
copy_literal_value(lower_state_t *state, LLVMValueRef value, th_type *type)
{
    uint32_t index = runtime_type_index(state, type);
    if (!index) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef params[] = {pointer, pointer, i32};
    LLVMTypeRef signature = LLVMFunctionType(LLVMVoidTypeInContext(context),
                                             params, 3, false);
    LLVMValueRef copier = LLVMGetNamedFunction(state->module,
        "sys.internal.marshal.copy_literal");
    if (!copier) copier = LLVMAddFunction(state->module,
        "sys.internal.marshal.copy_literal", signature);
    LLVMValueRef source = entry_alloca(state, LLVMTypeOf(value));
    LLVMValueRef destination = entry_alloca(state, LLVMTypeOf(value));
    if (!source || !destination) return nullptr;
    LLVMBuildStore(state->builder, value, source);
    LLVMValueRef args[] = {source, destination,
                           runtime_type_index_value(state, type)};
    if (!LLVMBuildCall2(state->builder, signature, copier, args, 3, ""))
        return nullptr;
    return LLVMBuildLoad2(state->builder, LLVMTypeOf(value), destination,
                          "literal.copy");
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
    th_named_info(th_resolve(type), nullptr, &variant, nullptr);
    if (variant || !definition || th_kind_of(definition) != TH_RECORD
        || LLVMGetTypeKind(native) != LLVMStructTypeKind) return nullptr;
    record_shape_t shape;
    if (!record_shape_build(state->function->owner, type, context, &shape))
        return nullptr;
    LLVMValueRef same = LLVMConstInt(LLVMInt1TypeInContext(context), 1,
                                     false);
    for (size_t i = 0; i < shape.layout->n_members; i++) {
        const ir_final_member_layout_t *member = &shape.layout->members[i];
        th_type *field = member->name.data && !member->inline_element_count
            ? th_record_lookup(definition, member->name) : nullptr;
        if (!field) { same = nullptr; break; }
        ir_class_t category = ir_machine_of(state->function->owner,
                                             field).class;
        if (shape.slots[i].packed || shape.slots[i].overlay
            || category == IR_CLASS_VOID
            || category == IR_CLASS_BOOL || category == IR_CLASS_SINT
            || category == IR_CLASS_UINT || category == IR_CLASS_FLOAT)
            continue;
        LLVMValueRef old_field = aggregate_element(state, old_value,
            shape.slots[i].element, "gc.old.field");
        LLVMValueRef new_field = aggregate_element(state, new_value,
            shape.slots[i].element, "gc.new.field");
        LLVMValueRef field_same = old_field && new_field
            ? gc_references_equal(state, old_field, new_field, field)
            : nullptr;
        if (!field_same) { same = nullptr; break; }
        same = LLVMBuildAnd(state->builder, same, field_same,
                            "gc.same.references");
    }
    record_shape_release(&shape);
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
assemble_box(lower_state_t *state, LLVMValueRef index, LLVMValueRef bits)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef box = LLVMConstNull(box_llvm_type(context));
    box = LLVMBuildInsertValue(state->builder, box, index, 2, "box.type");
    return LLVMBuildInsertValue(state->builder, box, bits, 3, "box.value");
}

static LLVMValueRef
unwrap_any_box(lower_state_t *state, LLVMValueRef value, th_type *any_type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMValueRef any_index = runtime_type_index_value(state, any_type);
    if (!value || LLVMTypeOf(value) != box || !any_index) return nullptr;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMValueRef type_field = LLVMBuildAnd(state->builder,
        LLVMBuildExtractValue(state->builder, value, 2, "any.type.field"),
        LLVMConstInt(i32, 0x7fffffff, false), "any.type.index");
    LLVMValueRef wrapped = LLVMBuildICmp(state->builder, LLVMIntEQ,
        type_field, any_index, "any.wrapped");
    LLVMBasicBlockRef entry = LLVMGetInsertBlock(state->builder);
    LLVMBasicBlockRef inner = LLVMAppendBasicBlockInContext(context,
        state->target_function, "any.unwrap");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        state->target_function, "any.unwrap.done");
    LLVMBuildCondBr(state->builder, wrapped, inner, done);
    LLVMPositionBuilderAtEnd(state->builder, inner);
    LLVMValueRef address = LLVMBuildIntToPtr(state->builder,
        LLVMBuildExtractValue(state->builder, value, 3, "any.payload"),
        LLVMPointerTypeInContext(context, 0), "any.inner.address");
    LLVMValueRef unwrapped = LLVMBuildLoad2(state->builder, box, address,
                                            "any.inner");
    LLVMBuildBr(state->builder, done);
    LLVMPositionBuilderAtEnd(state->builder, done);
    LLVMValueRef result = LLVMBuildPhi(state->builder, box, "any.value");
    LLVMValueRef incoming[] = {value, unwrapped};
    LLVMBasicBlockRef blocks[] = {entry, inner};
    LLVMAddIncoming(result, incoming, blocks, 2);
    return result;
}

static LLVMValueRef
box_value_with(lower_state_t *state, LLVMValueRef value, th_type *type,
               bool in_frame)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box_type = box_llvm_type(context);
    LLVMTypeRef source_type = LLVMTypeOf(value);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!box_type) return nullptr;
    if (source_type == box_type
        && th_builtin_kind_of(th_resolve(type)) != TH_BUILTIN_ANY)
        return value;
    bool open = !th_is_closed(ir_module_types(state->function->owner), type);
    uint32_t index = open ? 0 : runtime_type_index(state, type);
    if (!layout || (!open && (!index || index >= (1u << 31)))
        || LLVMGetTypeKind(source_type) == LLVMVoidTypeKind) return nullptr;
    LLVMValueRef type_index = open ? runtime_type_value(state, type)
                                   : runtime_type_index_value(state, type);
    if (!type_index) return nullptr;
    uint64_t size = LLVMABISizeOfType(layout, source_type);
    LLVMTypeRef word_type = LLVMInt64TypeInContext(context);
    LLVMValueRef bits = nullptr;
    if (size <= sizeof(uint64_t)) {
        LLVMValueRef slot = entry_alloca(state, word_type);
        if (!slot) return nullptr;
        LLVMBuildStore(state->builder, LLVMConstNull(word_type), slot);
        LLVMBuildStore(state->builder, value, slot);
        bits = LLVMBuildLoad2(state->builder, word_type, slot, "box.bits");
    }
    else if (in_frame) {
        LLVMValueRef slot = entry_alloca(state, source_type);
        if (!slot) return nullptr;
        LLVMBuildStore(state->builder, value, slot);
        bits = LLVMBuildPtrToInt(state->builder, slot, word_type,
                                 "box.frame.pointer");
    }
    else {
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
        LLVMTypeRef parameters[] = {i32, i32, word_type, i32, pointer};
        LLVMTypeRef signature = LLVMFunctionType(pointer, parameters,
                                                  5, false);
        const char *allocator_name = "n00b_allocate";
        LLVMValueRef allocator = LLVMGetNamedFunction(state->module,
                                                      allocator_name);
        if (!allocator)
            allocator = LLVMAddFunction(state->module, allocator_name,
                                        signature);
        if (!allocator || LLVMGlobalGetValueType(allocator) != signature)
            return nullptr;
        LLVMValueRef arguments[] = {
            LLVMConstInt(i32, N00B_ABI_ALLOCATOR_GC, false),
            type_index,
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
    return assemble_box(state, type_index, bits);
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
    return assemble_box(state, runtime_type_index_value(state, type), bits);
}

static LLVMValueRef
unbox_value(lower_state_t *state, LLVMValueRef box, th_type *type)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef target = value_type(state->function->owner, type, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!target || !layout || LLVMGetTypeKind(target) == LLVMVoidTypeKind)
        return nullptr;
    if (target == box_llvm_type(context))
        return th_builtin_kind_of(th_resolve(type)) == TH_BUILTIN_ANY
            ? unwrap_any_box(state, box, type) : box;
    LLVMValueRef bits = LLVMBuildExtractValue(state->builder, box, 3,
                                              "unbox.bits");
    LLVMValueRef slot = LLVMABISizeOfType(layout, target) <= sizeof(uint64_t)
        ? entry_alloca(state, LLVMInt64TypeInContext(context))
        : LLVMBuildIntToPtr(state->builder, bits,
            LLVMPointerTypeInContext(context, 0), "unbox.pointer");
    if (!slot) return nullptr;
    if (LLVMABISizeOfType(layout, target) <= sizeof(uint64_t))
        LLVMBuildStore(state->builder, bits, slot);
    return LLVMBuildLoad2(state->builder, target, slot, "unboxed");
}

static bool
prepare_box_slot_in(lower_state_t *state, bool in_frame,
                    LLVMValueRef type_index, LLVMValueRef slot)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    if (!type_index || !slot) return false;
    LLVMTypeRef size_parameters[] = {i32};
    LLVMValueRef bytes = box_runtime_call(state,
        "sys.internal.box.payload_bytes", i64, size_parameters, &type_index,
        1);
    LLVMValueRef bits = bytes ? runtime_sized_payload_in(state, in_frame,
        type_index, bytes) : nullptr;
    if (!bits) return false;
    LLVMBuildStore(state->builder, assemble_box(state, type_index, bits),
                   slot);
    return true;
}

static LLVMValueRef
copy_boxed_array(lower_state_t *state, LLVMValueRef value, bool in_frame)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef source = entry_alloca(state, box);
    LLVMValueRef result = entry_alloca(state, box);
    if (!value || LLVMTypeOf(value) != box || !source || !result)
        return nullptr;
    LLVMBuildStore(state->builder, value, source);
    LLVMValueRef type_index = LLVMBuildAnd(state->builder,
        LLVMBuildExtractValue(state->builder, value, 2, "array.copy.type"),
        LLVMConstInt(LLVMInt32TypeInContext(context), 0x7fffffff, false),
        "array.copy.index");
    if (!prepare_box_slot_in(state, in_frame, type_index, result))
        return nullptr;
    LLVMTypeRef parameters[] = {pointer, pointer};
    LLVMValueRef arguments[] = {source, result};
    if (!box_runtime_call(state, "sys.internal.gc.box_store.copy_array",
            LLVMVoidTypeInContext(context), parameters, arguments, 2))
        return nullptr;
    return LLVMBuildLoad2(state->builder, box, result,
                          "generic.array.copy");
}

static LLVMValueRef
location_slot(lower_state_t *state)
{
    LLVMTypeRef i64 = LLVMInt64TypeInContext(
        LLVMGetModuleContext(state->module));
    return entry_alloca(state, LLVMArrayType2(i64, 4));
}

static LLVMValueRef
reference_location(lower_state_t *state, LLVMValueRef reference)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef source_slot = entry_alloca(state, box);
    LLVMValueRef location = location_slot(state);
    if (!reference || LLVMTypeOf(reference) != box || !source_slot
        || !location) return nullptr;
    LLVMBuildStore(state->builder, reference, source_slot);
    LLVMTypeRef parameters[] = {pointer, pointer};
    LLVMValueRef arguments[] = {source_slot, location};
    return box_runtime_call(state, "sys.internal.box.reference_locate",
        LLVMVoidTypeInContext(context), parameters, arguments, 2)
        ? location : nullptr;
}

static bool
write_location(lower_state_t *state, LLVMValueRef location,
               LLVMValueRef value_slot)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer, pointer};
    LLVMValueRef arguments[] = {location, value_slot};
    return location && value_slot && box_runtime_call(state,
        "sys.internal.gc.box_store.write_at", LLVMVoidTypeInContext(context),
        parameters, arguments, 2);
}


static LLVMValueRef
boxed_reference_address(lower_state_t *state, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    if (!value || LLVMTypeOf(value) != box) return nullptr;
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer};
    LLVMValueRef slot = entry_alloca(state, box);
    if (!slot) return nullptr;
    LLVMBuildStore(state->builder, value, slot);
    return box_runtime_call(state, "sys.internal.box.reference_address",
                            pointer, parameters, &slot, 1);
}

static th_type *
option_payload_type(th_type *type)
{
    type = th_builtin_applied(type, TH_BUILTIN_OPTION, 1);
    const th_arg *argument = type ? th_named_arg(type, 0) : nullptr;
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
        || LLVMGetTypeKind(LLVMGetElementType(storage)) != LLVMIntegerTypeKind)
        return nullptr;
    unsigned width = LLVMGetIntTypeWidth(LLVMGetElementType(storage));
    unsigned count = LLVMGetArrayLength(storage);
    return width && count && count <= UINT_MAX / width
        ? LLVMIntTypeInContext(context, count * width) : nullptr;
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
packed_option_presence_byte(lower_state_t *state, LLVMValueRef value,
                            uint64_t bit)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(context),
                                       bit / 8, false);
    LLVMValueRef mask = LLVMConstInt(i8, UINT64_C(1) << (bit % 8), false);
    bool in_memory = LLVMIsALoadInst(value) && !LLVMGetVolatile(value)
        && LLVMGetOrdering(value) == LLVMAtomicOrderingNotAtomic;
    LLVMBasicBlockRef resume = LLVMGetInsertBlock(state->builder);
    LLVMValueRef base = nullptr;
    if (in_memory) {
        LLVMValueRef after = LLVMGetNextInstruction(value);
        if (after) LLVMPositionBuilderBefore(state->builder, after);
        else LLVMPositionBuilderAtEnd(state->builder,
                                      LLVMGetInstructionParent(value));
        base = LLVMGetOperand(value, 0);
    }
    else {
        base = entry_alloca(state, LLVMTypeOf(value));
        if (!base) return nullptr;
        LLVMBuildStore(state->builder, value, base);
    }
    LLVMValueRef address = LLVMBuildInBoundsGEP2(state->builder, i8, base,
        &offset, 1, "option.presence.address");
    LLVMValueRef byte = LLVMBuildLoad2(state->builder, i8, address,
                                       "option.presence.byte");
    LLVMSetAlignment(byte, 1);
    LLVMValueRef presence = LLVMBuildAnd(state->builder, byte, mask,
                                         "option.presence");
    if (in_memory) LLVMPositionBuilderAtEnd(state->builder, resume);
    return presence;
}

static bool
wide_packed_option(LLVMTypeRef bits, LLVMTypeRef aggregate)
{
    LLVMTypeKind kind = aggregate ? LLVMGetTypeKind(aggregate)
                                  : LLVMVoidTypeKind;
    return bits && LLVMGetIntTypeWidth(bits) > 128
        && (kind == LLVMArrayTypeKind || kind == LLVMStructTypeKind);
}

static LLVMValueRef
packed_option_retag(lower_state_t *state, LLVMValueRef value,
                    LLVMTypeRef result, uint64_t bit, bool present)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    if (!value || !result || !layout
        || LLVMStoreSizeOfType(layout, LLVMTypeOf(value))
            != LLVMStoreSizeOfType(layout, result)) return nullptr;
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMValueRef slot = entry_alloca(state, LLVMTypeOf(value));
    if (!slot) return nullptr;
    LLVMBuildStore(state->builder, value, slot);
    LLVMValueRef offset = LLVMConstInt(LLVMInt64TypeInContext(context),
                                       bit / 8, false);
    LLVMValueRef address = LLVMBuildInBoundsGEP2(state->builder, i8, slot,
        &offset, 1, "option.tag.address");
    LLVMValueRef byte = LLVMBuildLoad2(state->builder, i8, address,
                                       "option.tag.byte");
    LLVMSetAlignment(byte, 1);
    uint64_t mask = UINT64_C(1) << (bit % 8);
    LLVMValueRef updated = present
        ? LLVMBuildOr(state->builder, byte, LLVMConstInt(i8, mask, false),
                      "option.tag.set")
        : LLVMBuildAnd(state->builder, byte,
                       LLVMConstInt(i8, ~mask & 0xff, false),
                       "option.tag.clear");
    LLVMValueRef store = LLVMBuildStore(state->builder, updated, address);
    LLVMSetAlignment(store, 1);
    LLVMValueRef loaded = LLVMBuildLoad2(state->builder, result, slot,
                                         "option.retagged");
    LLVMSetAlignment(loaded, 1);
    return loaded;
}

static LLVMValueRef
packed_option_presence(lower_state_t *state, LLVMValueRef value,
                       uint64_t bit)
{
    LLVMTypeRef bits = packed_option_bits_type(LLVMTypeOf(value),
        LLVMGetModuleContext(state->module));
    if (bits && LLVMGetIntTypeWidth(bits) > 128
        && LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind)
        return packed_option_presence_byte(state, value, bit);
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
    if (wide_packed_option(bits, source))
        return packed_option_retag(state, payload, target, bit, true);
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
    if (wide_packed_option(bits, target))
        return packed_option_retag(state, value, target, bit, false);
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
option_presence(lower_state_t *state, const option_repr_t *repr,
                LLVMValueRef value)
{
    if (!repr || !value || LLVMTypeOf(value) != repr->storage) return nullptr;
    switch (repr->kind) {
    case OPTION_REPR_POINTER:
        return value;
    case OPTION_REPR_PACKED:
        return packed_option_presence(state, value, repr->presence_bit);
    case OPTION_REPR_SHARED: {
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, value, 1,
                                                 "option.tag");
        return LLVMBuildAnd(state->builder, tag,
            LLVMConstInt(LLVMTypeOf(tag), repr->presence_mask, false),
            "option.outer.tag");
    }
    case OPTION_REPR_TAGGED:
        return LLVMBuildExtractValue(state->builder, value, 1, "option.tag");
    }
    return nullptr;
}

static LLVMValueRef
option_payload(lower_state_t *state, const option_repr_t *repr,
               th_type *type, LLVMValueRef value)
{
    if (!repr || !value || LLVMTypeOf(value) != repr->storage) return nullptr;
    switch (repr->kind) {
    case OPTION_REPR_POINTER:
        return value;
    case OPTION_REPR_PACKED:
        return unpack_option_integer(state, value, type);
    case OPTION_REPR_SHARED: {
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, value, 1,
                                                 "option.outer.tag");
        tag = LLVMBuildAnd(state->builder, tag,
            LLVMConstInt(LLVMTypeOf(tag), ~repr->presence_mask & 0xffu,
                         false), "option.inner.tag");
        return LLVMBuildInsertValue(state->builder, value, tag, 1,
                                    "option.payload");
    }
    case OPTION_REPR_TAGGED:
        return LLVMBuildExtractValue(state->builder, value, 0,
                                     "option.payload");
    }
    return nullptr;
}

static LLVMValueRef
option_some(lower_state_t *state, const option_repr_t *repr, th_type *type,
            LLVMValueRef payload)
{
    if (!repr || !payload) return nullptr;
    switch (repr->kind) {
    case OPTION_REPR_POINTER:
        return LLVMTypeOf(payload) == repr->storage ? payload : nullptr;
    case OPTION_REPR_PACKED:
        return pack_option_integer(state, payload, type);
    case OPTION_REPR_SHARED: {
        if (LLVMTypeOf(payload) != repr->storage) return nullptr;
        LLVMValueRef tag = LLVMBuildExtractValue(state->builder, payload, 1,
                                                 "option.inner.tag");
        tag = LLVMBuildOr(state->builder, tag,
            LLVMConstInt(LLVMTypeOf(tag), repr->presence_mask, false),
            "option.outer.tag");
        return LLVMBuildInsertValue(state->builder, payload, tag, 1,
                                    "option.some");
    }
    case OPTION_REPR_TAGGED: {
        if (LLVMStructGetTypeAtIndex(repr->storage, 0) != LLVMTypeOf(payload))
            return nullptr;
        LLVMValueRef stored = LLVMBuildInsertValue(state->builder,
            LLVMGetUndef(repr->storage), payload, 0, "option.payload");
        return LLVMBuildInsertValue(state->builder, stored,
            LLVMConstInt(LLVMStructGetTypeAtIndex(repr->storage, 1),
                         repr->presence_mask, false), 1, "option.some");
    }
    }
    return nullptr;
}

static bool
crosses_any_boundary(lower_state_t *state, th_type *source_type,
                     th_type *target_type)
{
    if (!source_type || !target_type) return false;
    th_ctx *types = ir_module_types(state->function->owner);
    bool source_any = th_builtin_kind_of(th_resolve(source_type))
        == TH_BUILTIN_ANY;
    bool target_any = th_builtin_kind_of(th_resolve(target_type))
        == TH_BUILTIN_ANY;
    return source_any != target_any
        && !th_is_closed(types, source_any ? target_type : source_type);
}

static LLVMValueRef
adapt_call_value_with(lower_state_t *state, LLVMValueRef value,
                      th_type *source_type, th_type *target_type,
                      bool in_frame)
{
    if (!value || !source_type || !target_type) return nullptr;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef target = value_type(state->function->owner, target_type,
                                   context);
    if (!target) return nullptr;
    th_ctx *types = ir_module_types(state->function->owner);
    bool source_any = th_builtin_kind_of(th_resolve(source_type))
        == TH_BUILTIN_ANY;
    bool target_any = th_builtin_kind_of(th_resolve(target_type))
        == TH_BUILTIN_ANY;
    if (source_any && !target_any && target == box_llvm_type(context)
        && !th_is_closed(types, target_type))
        return box_value_with(state, value, source_type, in_frame);
    if (target_any && !source_any && LLVMTypeOf(value) == target
        && !th_is_closed(types, source_type))
        return unwrap_any_box(state, value, target_type);
    if (LLVMTypeOf(value) == target) return value;
    option_repr_t source_repr, target_repr;
    bool source_option = option_payload_type(source_type)
        && option_repr(state->function->owner, source_type, context,
                       &source_repr)
        && source_repr.storage == LLVMTypeOf(value)
        && source_repr.kind != OPTION_REPR_POINTER;
    bool target_any_option = option_payload_type(target_type)
        && option_repr(state->function->owner, target_type, context,
                       &target_repr);
    bool target_pointer = target_any_option
        && target_repr.kind == OPTION_REPR_POINTER;
    bool target_option = target_any_option && !target_pointer;
    bool source_packed = source_option
        && source_repr.kind == OPTION_REPR_PACKED;
    bool target_packed = target_option
        && target_repr.kind == OPTION_REPR_PACKED;
    if (source_option && (target_pointer || (target_option
        && !(source_packed && target_packed)
        && !(!source_packed && !target_packed
            && LLVMStructGetTypeAtIndex(source_repr.storage, 1)
                != LLVMStructGetTypeAtIndex(target_repr.storage, 1))))) {
        th_type *source_payload = source_repr.payload_type;
        th_type *target_payload = target_repr.payload_type;
        LLVMValueRef presence = option_presence(state, &source_repr, value);
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
        LLVMValueRef payload = option_payload(state, &source_repr,
                                              source_type, value);
        LLVMValueRef converted = payload ? adapt_call_value_with(state,
            payload, source_payload, target_payload, in_frame) : nullptr;
        if (!converted) return nullptr;
        LLVMValueRef some = option_some(state, &target_repr, target_type,
                                        converted);
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
    LLVMTypeRef box = box_llvm_type(context);
    if (LLVMTypeOf(value) == box
        && LLVMGetTypeKind(target) == LLVMPointerTypeKind
        && th_kind_of(th_resolve(source_type)) == TH_INDIRECTION
        && th_kind_of(th_resolve(target_type)) == TH_INDIRECTION)
        return boxed_reference_address(state, value);
    if (LLVMTypeOf(value) == box) return unbox_value(state, value,
                                                     target_type);
    if (target == box) return box_value_with(state, value, source_type,
                                             in_frame);
    return nullptr;
}

static LLVMValueRef
adapt_call_value(lower_state_t *state, LLVMValueRef value,
                 th_type *source_type, th_type *target_type)
{
    return adapt_call_value_with(state, value, source_type, target_type,
                                 false);
}

static LLVMValueRef
adapt_call_argument(lower_state_t *state, LLVMValueRef value,
                    th_type *source_type, th_type *target_type)
{
    return adapt_call_value_with(state, value, source_type, target_type,
                                 true);
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
                                                  CORE_STRING_STATIC("nil"));
    if (!target_failure) target_failure = th_name(ir_module_types(owner),
                                                  CORE_STRING_STATIC("nil"));
    if (!source_success || !source_failure || !target_success
        || !target_failure) return nullptr;
    bool source_fails = ir_machine_of(owner, source_failure).class
        != IR_CLASS_VOID;
    bool target_fails = ir_machine_of(owner, target_failure).class
        != IR_CLASS_VOID;
    if (source_fails != target_fails) return nullptr;
    bool in_frame = box_in_frame(state, state->current_instruction);
    if (!source_fails) return adapt_call_value_with(state, value,
        source_success, target_success, in_frame);

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
        converted = adapt_call_value_with(state, source_value,
            source_success, target_success, in_frame);
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
    converted = adapt_call_value_with(state, source_value, source_failure,
        target_failure, in_frame);
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

static bool
variant_layout(ir_module_t *module, th_type *type,
               const ir_final_layout_t **out, uint64_t *stride_bytes)
{
    bool variant = false;
    if (!th_named_info(th_resolve(type), nullptr, &variant, nullptr)
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
variant_member(const ir_final_layout_t *layout, core_string_t name)
{
    for (size_t i = 0; layout && name.data && i < layout->n_members; i++)
        if (layout->members[i].name.data
            && core_string_eq(layout->members[i].name, name))
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

static bool
value_only_returned(const ir_func_t *function, const ir_value_t *value,
                    unsigned depth)
{
    if (depth > 8) return false;
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        const ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            const ir_instr_t *instruction = block->instrs[i];
            for (uint32_t o = 0; o < instruction->n_operands; o++) {
                if (ir_operand_value(&instruction->operands[o]) != value)
                    continue;
                if ((instruction->op == IR_OP_RETURN
                        || instruction->op == IR_OP_FAIL) && o == 0)
                    continue;
                if (instruction->op == IR_OP_VARIANT_TAG) continue;
                if ((instruction->op == IR_OP_COPY
                        || instruction->op == IR_OP_VARIANT_PAYLOAD)
                    && instruction->dest_kind == IR_DEST_VALUE
                    && instruction->dest
                    && value_only_returned(function, instruction->dest,
                                           depth + 1)) continue;
                return false;
            }
        }
    }
    return true;
}

static bool
inherits_result_box(lower_state_t *state, const ir_instr_t *call)
{
    th_type *signature = ir_instr_call_signature(call);
    th_type *returned = signature ? th_func_ret(signature) : nullptr;
    th_type *own = th_func_ret(state->function->type);
    return state->result_box && call->dest_kind == IR_DEST_VALUE
        && call->dest && returned && own
        && ir_type_eq(state->function->owner, returned, own)
        && value_only_returned(state->function, call->dest, 0);
}

enum { frame_payload_capacity = 64 };

static LLVMValueRef
runtime_sized_payload_in(lower_state_t *state, bool use_frame,
                         LLVMValueRef type_index, LLVMValueRef bytes)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef i8 = LLVMInt8TypeInContext(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef frame = use_frame
        ? entry_alloca(state, LLVMArrayType2(i8, frame_payload_capacity))
        : nullptr;
    if (frame) LLVMSetAlignment(frame, 16);
    LLVMBasicBlockRef start = LLVMGetInsertBlock(state->builder);
    LLVMBasicBlockRef sized = LLVMAppendBasicBlockInContext(context,
        state->target_function, "payload.sized");
    LLVMBasicBlockRef in_frame = LLVMAppendBasicBlockInContext(context,
        state->target_function, "payload.frame");
    LLVMBasicBlockRef on_heap = LLVMAppendBasicBlockInContext(context,
        state->target_function, "payload.heap");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(context,
        state->target_function, "payload.done");
    LLVMBuildCondBr(state->builder, LLVMBuildICmp(state->builder, LLVMIntEQ,
        bytes, LLVMConstNull(i64), "payload.inline"), done, sized);
    LLVMPositionBuilderAtEnd(state->builder, sized);
    LLVMValueRef fits = frame ? LLVMBuildICmp(state->builder, LLVMIntULE,
        bytes, LLVMConstInt(i64, frame_payload_capacity, false),
        "payload.fits") : LLVMConstInt(LLVMInt1TypeInContext(context), 0,
        false);
    LLVMBuildCondBr(state->builder, fits, in_frame, on_heap);
    LLVMPositionBuilderAtEnd(state->builder, in_frame);
    LLVMValueRef frame_bits = LLVMConstNull(i64);
    if (frame) {
        LLVMBuildMemSet(state->builder, frame, LLVMConstNull(i8), bytes, 16);
        frame_bits = LLVMBuildPtrToInt(state->builder, frame, i64,
                                       "payload.frame.bits");
    }
    LLVMBuildBr(state->builder, done);
    LLVMBasicBlockRef frame_end = LLVMGetInsertBlock(state->builder);
    LLVMPositionBuilderAtEnd(state->builder, on_heap);
    LLVMTypeRef parameters[] = {i32, i64, i32, pointer};
    LLVMValueRef arguments[] = {type_index, bytes,
        LLVMConstInt(i32, 16, false), LLVMConstNull(pointer)};
    LLVMValueRef heap = box_runtime_call(state,
        "sys.internal.gc.alloc.allocate_runtime", pointer, parameters,
        arguments, 4);
    if (!heap) return nullptr;
    emit_trap_if(state, LLVMBuildIsNull(state->builder, heap,
                                        "payload.heap.missing"));
    LLVMValueRef heap_bits = LLVMBuildPtrToInt(state->builder, heap, i64,
                                               "payload.heap.bits");
    LLVMBuildBr(state->builder, done);
    LLVMBasicBlockRef heap_end = LLVMGetInsertBlock(state->builder);
    LLVMPositionBuilderAtEnd(state->builder, done);
    LLVMValueRef bits = LLVMBuildPhi(state->builder, i64, "payload.bits");
    LLVMValueRef incoming[] = {LLVMConstNull(i64), frame_bits, heap_bits};
    LLVMBasicBlockRef blocks[] = {start, frame_end, heap_end};
    LLVMAddIncoming(bits, incoming, blocks, 3);
    return bits;
}

static LLVMValueRef
runtime_sized_payload(lower_state_t *state, const ir_instr_t *instruction,
                      LLVMValueRef type_index, LLVMValueRef bytes)
{
    return runtime_sized_payload_in(state, box_in_frame(state, instruction),
                                    type_index, bytes);
}

static LLVMValueRef
prepare_result_box(lower_state_t *state, const ir_instr_t *call,
                   th_type *formal)
{
    if (inherits_result_box(state, call)) return state->result_box;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
    LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
    th_type *signature = ir_instr_call_signature(call);
    th_type *success = th_func_ret(signature ? signature : formal);
    LLVMValueRef type_index = success
        ? runtime_type_value(state, success) : nullptr;
    LLVMValueRef slot = type_index ? entry_alloca(state, box) : nullptr;
    if (!slot) return nullptr;
    LLVMTypeRef native = value_type(state->function->owner, success, context);
    LLVMTargetDataRef layout = LLVMGetModuleDataLayout(state->module);
    LLVMValueRef bits = LLVMConstNull(i64);
    if (native && native != box && layout) {
        if (LLVMABISizeOfType(layout, native) > sizeof(uint64_t)) {
            LLVMValueRef storage = entry_alloca(state, native);
            if (!storage) return nullptr;
            LLVMBuildStore(state->builder, LLVMConstNull(native), storage);
            bits = LLVMBuildPtrToInt(state->builder, storage, i64,
                                     "result.box.storage");
        }
    }
    else {
        LLVMTypeRef parameters[] = {i32};
        LLVMValueRef bytes = box_runtime_call(state,
            "sys.internal.box.payload_bytes", i64, parameters, &type_index,
            1);
        bits = bytes ? runtime_sized_payload(state, call, type_index, bytes)
            : nullptr;
        if (!bits) return nullptr;
    }
    LLVMValueRef value = assemble_box(state, type_index, bits);
    if (!value) return nullptr;
    LLVMBuildStore(state->builder, value, slot);
    return slot;
}

static LLVMValueRef
fill_result_box(lower_state_t *state, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    if (!value || LLVMTypeOf(value) != box) return nullptr;
    LLVMValueRef source = entry_alloca(state, box);
    if (!source) return nullptr;
    LLVMBuildStore(state->builder, value, source);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMTypeRef parameters[] = {pointer, pointer};
    LLVMValueRef arguments[] = {state->result_box, source};
    if (!box_runtime_call(state, "sys.internal.box.fill",
            LLVMVoidTypeInContext(context), parameters, arguments, 2))
        return nullptr;
    return LLVMBuildLoad2(state->builder, box, state->result_box,
                          "result.box");
}

static bool
flow_carries_box(lower_state_t *state, const ir_instr_t *instruction)
{
    if (instruction->dest_kind != IR_DEST_VALUE || !instruction->dest
        || instruction->op == IR_OP_CALL
        || instruction->op == IR_OP_CALLBACK_INVOKE) return false;
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef native = value_type(state->function->owner,
                                    instruction->dest->type, context);
    return native && llvm_type_holds_box(native, box_llvm_type(context));
}

static ir_live_flow_t
box_flow(void *context, const ir_instr_t *user, uint32_t operand)
{
    lower_state_t *state = context;
    if (user->dest_kind == IR_DEST_LOCAL
        || (user->op == IR_OP_STORE && user->n_operands == 2 && operand == 1
            && direct_stack_address(state->function,
                                    ir_operand_value(&user->operands[0]))))
        return IR_LIVE_FLOW_HOLD;
    return flow_carries_box(state, user) ? IR_LIVE_FLOW_CARRY
                                         : IR_LIVE_FLOW_NONE;
}

static bool
box_in_frame(lower_state_t *state, const ir_instr_t *instruction)
{
    if (!instruction || instruction->dest_kind != IR_DEST_VALUE
        || !instruction->dest) return false;
    if (!state->live)
        state->live = ir_func_liveness((ir_func_t *)state->function);
    if (!ir_live_block_on_cycle(state->live, instruction->block))
        return true;
    return ir_live_carried_after(state->live, instruction->dest, box_flow,
                                 state, instruction) == IR_LIVENESS_DEAD;
}

static bool
llvm_type_holds_box(LLVMTypeRef type, LLVMTypeRef box)
{
    if (type == box) return true;
    LLVMTypeKind kind = LLVMGetTypeKind(type);
    if (kind == LLVMArrayTypeKind)
        return llvm_type_holds_box(LLVMGetElementType(type), box);
    if (kind != LLVMStructTypeKind) return false;
    unsigned count = LLVMCountStructElementTypes(type);
    for (unsigned i = 0; i < count; i++)
        if (llvm_type_holds_box(LLVMStructGetTypeAtIndex(type, i), box))
            return true;
    return false;
}

static bool
escape_box_contents(lower_state_t *state, LLVMValueRef slot)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef arguments[] = {slot};
    LLVMTypeRef parameters[] = {pointer};
    return slot && box_runtime_call(state,
        "sys.internal.gc.box_store.escape_box_inner",
        LLVMVoidTypeInContext(context), parameters, arguments, 1);
}

static LLVMValueRef
escape_boxes(lower_state_t *state, LLVMValueRef value)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMTypeRef box = box_llvm_type(context);
    LLVMTypeRef type = value ? LLVMTypeOf(value) : nullptr;
    if (!type || !llvm_type_holds_box(type, box)) return value;
    if (type == box) {
        LLVMValueRef slot = entry_alloca(state, box);
        if (!slot) return nullptr;
        LLVMBuildStore(state->builder, value, slot);
        LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
        LLVMTypeRef parameters[] = {pointer};
        LLVMValueRef arguments[] = {slot};
        if (!box_runtime_call(state, "sys.internal.gc.box_store.escape_box",
                LLVMVoidTypeInContext(context), parameters, arguments, 1))
            return nullptr;
        return LLVMBuildLoad2(state->builder, box, slot, "box.escaped");
    }
    unsigned count = LLVMGetTypeKind(type) == LLVMArrayTypeKind
        ? (unsigned)LLVMGetArrayLength2(type)
        : LLVMCountStructElementTypes(type);
    for (unsigned i = 0; value && i < count; i++) {
        LLVMTypeRef element = LLVMGetTypeKind(type) == LLVMArrayTypeKind
            ? LLVMGetElementType(type) : LLVMStructGetTypeAtIndex(type, i);
        if (!llvm_type_holds_box(element, box)) continue;
        LLVMValueRef part = escape_boxes(state, LLVMBuildExtractValue(
            state->builder, value, i, "escape.part"));
        value = part ? LLVMBuildInsertValue(state->builder, value, part, i,
                                            "escape.whole") : nullptr;
    }
    return value;
}

static LLVMValueRef
same_shape_struct(lower_state_t *state, LLVMValueRef source,
                  LLVMTypeRef target)
{
    LLVMTypeRef from = LLVMTypeOf(source);
    if (from == target) return source;
    unsigned count = LLVMGetTypeKind(from) == LLVMStructTypeKind
        && LLVMGetTypeKind(target) == LLVMStructTypeKind
        ? LLVMCountStructElementTypes(from) : 0;
    if (!count || count != LLVMCountStructElementTypes(target))
        return nullptr;
    LLVMValueRef result = LLVMGetUndef(target);
    for (unsigned i = 0; i < count; i++) {
        LLVMValueRef field = LLVMBuildExtractValue(state->builder, source, i,
                                                   "box.raw.field");
        LLVMTypeRef want = LLVMStructGetTypeAtIndex(target, i);
        LLVMTypeKind have_kind = LLVMGetTypeKind(LLVMTypeOf(field));
        LLVMTypeKind want_kind = LLVMGetTypeKind(want);
        bool to_bits = have_kind == LLVMPointerTypeKind
            && want_kind == LLVMIntegerTypeKind;
        bool to_pointer = have_kind == LLVMIntegerTypeKind
            && want_kind == LLVMPointerTypeKind;
        if (LLVMTypeOf(field) != want && !to_bits && !to_pointer)
            return nullptr;
        if (to_bits)
            field = LLVMBuildPtrToInt(state->builder, field, want,
                                      "box.raw.bits");
        if (to_pointer)
            field = LLVMBuildIntToPtr(state->builder, field, want,
                                      "box.raw.pointer");
        result = LLVMBuildInsertValue(state->builder, result, field, i,
                                      "box.raw.value");
    }
    return result;
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
load_array_extent(lower_state_t *state, LLVMValueRef address,
                  const char *name)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef extent = LLVMBuildLoad2(state->builder,
        LLVMInt64TypeInContext(context), address, name);
    if (extent)
        LLVMSetMetadata(extent,
            LLVMGetMDKindIDInContext(context, "invariant.load", 14),
            LLVMMetadataAsValue(context,
                LLVMMDNodeInContext2(context, nullptr, 0)));
    return extent;
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

static bool
overflow_proven(lower_state_t *state, const ir_instr_t *instruction)
{
    return ir_range_overflow(state->ranges, instruction, nullptr, nullptr)
            == IR_OVERFLOW_NEVER
        || ir_abcd_overflow_safe(ir_range_abcd(state->ranges), instruction);
}

static LLVMValueRef
native_array_slot(lower_state_t *state, LLVMValueRef array_address,
                  LLVMValueRef index, LLVMTypeRef array_type,
                  LLVMTypeRef element_type, bool signed_index,
                  bool check_bounds)
{
    LLVMContextRef context = LLVMGetModuleContext(state->module);
    LLVMValueRef array = LLVMBuildLoad2(state->builder, array_type,
        array_address, "array.value");
    LLVMValueRef data = LLVMBuildExtractValue(state->builder, array, 0,
        "array.data");
    LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder, array, 1,
        "array.descriptor");
    LLVMValueRef length_address = LLVMBuildStructGEP2(state->builder,
        array_descriptor_type(context), descriptor, 5,
        "array.length.address");
    LLVMValueRef length = load_array_extent(state, length_address,
        "array.length");
    LLVMValueRef position = checked_array_index(state, index, length,
        signed_index, check_bounds);
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

static const ir_instr_t *
local_assignment(ir_func_t *function, const ir_local_t *local)
{
    const ir_instr_t *assignment = nullptr;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *in = function->blocks[b]->instrs[i];
            if (in->dest_kind != IR_DEST_LOCAL || in->dest_local != local)
                continue;
            if (assignment) return nullptr;
            assignment = in;
        }
    return assignment;
}

static const ir_instr_t *
retained_source(ir_func_t *function, const ir_value_t *value)
{
    for (uint32_t depth = 0; value && depth < 8; depth++) {
        const ir_instr_t *source = value->kind == IR_VAL_LOCAL && value->local
            ? local_assignment(function, value->local) : value->def;
        if (!source || source->op != IR_OP_COPY || source->n_operands != 1
            || source->operands[0].kind != IR_OPD_VALUE)
            return source;
        value = source->operands[0].value;
    }
    return nullptr;
}

static bool
lower_instruction(lower_state_t *state, ir_instr_t *instruction)
{
    LLVMValueRef result = nullptr;
    state->failure_detail = (core_string_t){0};
    state->current_instruction = instruction;
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
                nullptr, &variant, nullptr) || !variant) break;
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
    case IR_OP_BITSET_UNION:
    case IR_OP_BITSET_DIFFERENCE:
    case IR_OP_BITSET_INTERSECTION:
    case IR_OP_BITSET_XOR:
    case IR_OP_BITSET_COMPLEMENT:
    case IR_OP_BITSET_CONTAINS:
    case IR_OP_BITSET_SET:
    case IR_OP_BITSET_IS_EMPTY: {
        if (count < 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        LLVMValueRef left = lowered_value(state, operands[0].value);
        LLVMValueRef right = count >= 2 && operands[1].kind == IR_OPD_VALUE
            ? lowered_value(state, operands[1].value) : nullptr;
        LLVMValueRef mask = bitset_mask(state, operands[0].value->type);
        if (!left || !mask
            || LLVMGetTypeKind(LLVMTypeOf(left)) != LLVMIntegerTypeKind)
            break;
        LLVMBuilderRef builder = state->builder;
        switch (instruction->op) {
        case IR_OP_BITSET_UNION:
            result = right ? LLVMBuildOr(builder, left, right, "bitset.union")
                           : nullptr;
            break;
        case IR_OP_BITSET_DIFFERENCE:
            result = right ? LLVMBuildAnd(builder, left,
                LLVMBuildNot(builder, right, "bitset.not"),
                "bitset.difference") : nullptr;
            break;
        case IR_OP_BITSET_INTERSECTION:
            result = right ? LLVMBuildAnd(builder, left, right,
                                          "bitset.intersection") : nullptr;
            break;
        case IR_OP_BITSET_XOR:
            result = right ? LLVMBuildXor(builder, left, right, "bitset.xor")
                           : nullptr;
            break;
        case IR_OP_BITSET_COMPLEMENT:
            result = LLVMBuildXor(builder, left, mask, "bitset.complement");
            break;
        case IR_OP_BITSET_CONTAINS:
            result = right ? LLVMBuildICmp(builder, LLVMIntEQ,
                LLVMBuildAnd(builder, left, right, "bitset.common"), right,
                "bitset.contains") : nullptr;
            break;
        case IR_OP_BITSET_SET: {
            LLVMValueRef enabled = count == 3
                && operands[2].kind == IR_OPD_VALUE
                ? lowered_value(state, operands[2].value) : nullptr;
            result = right && enabled ? LLVMBuildSelect(builder, enabled,
                LLVMBuildOr(builder, left, right, "bitset.with"),
                LLVMBuildAnd(builder, left,
                    LLVMBuildNot(builder, right, "bitset.not"),
                    "bitset.without"), "bitset.set") : nullptr;
            break;
        }
        default:
            result = LLVMBuildICmp(builder, LLVMIntEQ, left,
                LLVMConstNull(LLVMTypeOf(left)), "bitset.is_empty");
            break;
        }
        return result && save_result(state, instruction, result);
    }
    case IR_OP_BITSET_VALUE:
    case IR_OP_BITSET_FROM_VALUE: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        ir_module_t *source = state->function->owner;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(source, instruction->dest->type,
                                        LLVMGetModuleContext(state->module));
        if (!value || !target
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(target) != LLVMIntegerTypeKind) break;
        unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(value));
        unsigned target_bits = LLVMGetIntTypeWidth(target);
        if (instruction->op == IR_OP_BITSET_FROM_VALUE
            && ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"))) {
            LLVMValueRef mask = bitset_mask(state, instruction->dest->type);
            if (!mask) break;
            LLVMValueRef wide_mask = source_bits == target_bits ? mask
                : source_bits > target_bits
                    ? LLVMBuildZExt(state->builder, mask, LLVMTypeOf(value),
                                    "bitset.mask")
                    : LLVMBuildTrunc(state->builder, mask, LLVMTypeOf(value),
                                     "bitset.mask");
            LLVMValueRef outside = LLVMBuildAnd(state->builder, value,
                LLVMBuildNot(state->builder, wide_mask, "bitset.invalid"),
                "bitset.outside");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                outside, LLVMConstNull(LLVMTypeOf(value)),
                "bitset.out_of_range"));
        }
        result = target_bits == source_bits ? value
            : target_bits < source_bits
                ? LLVMBuildTrunc(state->builder, value, target, "bitset.value")
                : LLVMBuildZExt(state->builder, value, target, "bitset.value");
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
    case IR_OP_ENUM_FROM_VALUE: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        ir_module_t *source = state->function->owner;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(source, instruction->dest->type,
                                        LLVMGetModuleContext(state->module));
        if (!value || !target
            || LLVMGetTypeKind(LLVMTypeOf(value)) != LLVMIntegerTypeKind
            || LLVMGetTypeKind(target) != LLVMIntegerTypeKind) break;
        unsigned source_bits = LLVMGetIntTypeWidth(LLVMTypeOf(value));
        unsigned target_bits = LLVMGetIntTypeWidth(target);
        bool signed_source = ir_machine_of(source,
            operands[0].value->type).class == IR_CLASS_SINT;
        result = target_bits == source_bits ? value
            : target_bits < source_bits
                ? LLVMBuildTrunc(state->builder, value, target,
                                 "enum.from_value")
                : signed_source
                    ? LLVMBuildSExt(state->builder, value, target,
                                   "enum.from_value")
                    : LLVMBuildZExt(state->builder, value, target,
                                   "enum.from_value");
        return save_result(state, instruction, result);
    }
    case IR_OP_COPY:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE) break;
        if (operands[0].value && ir_machine_of(state->function->owner,
                operands[0].value->type).class == IR_CLASS_VOID)
            return true;
        result = lowered_value(state, operands[0].value);
        if (result && instruction->dest
            && ir_annot_get(&instruction->annots,
                            CORE_STRING_STATIC("n00bb.literal_copy")))
            result = copy_literal_value(state, result,
                                        instruction->dest->type);
        ir_annot_t *promoted_write = ir_annot_get(&instruction->annots,
            CORE_STRING_STATIC("ir.promoted_local_write"));
        th_type *copy_type = instruction->dest_kind == IR_DEST_LOCAL
            && instruction->dest_local ? instruction->dest_local->type
            : promoted_write && promoted_write->kind == IR_ANNOT_BOOL
                && promoted_write->b && instruction->dest
                ? instruction->dest->type : nullptr;
        if (result && copy_type
            && th_kind_of(th_resolve(copy_type)) == TH_SEQUENCE
            && !n00bb_sequence_transfers_storage(operands[0].value))
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
            LLVMValueRef slot = local_slot(state, local);
            LLVMTypeRef type = slot ? value_type(state->function->owner,
                local->type, LLVMGetModuleContext(state->module)) : nullptr;
            if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) break;
            LLVMBuildStore(state->builder, LLVMConstNull(type), slot);
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_RETAIN_REF:
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        if (operands[0].value->kind != IR_VAL_FUNC) {
            th_type *referent = th_ind_referent(operands[0].value->type);
            if (!referent || th_kind_of(th_resolve(referent)) != TH_FUNCTION) {
                const ir_instr_t *source = retained_source(state->function,
                    operands[0].value);
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
        core_string_t name = instruction->op == IR_OP_CLZ
            ? CORE_STRING_STATIC("llvm.ctlz") : CORE_STRING_STATIC("llvm.cttz");
        unsigned id = LLVMLookupIntrinsicID(name.data, name.u8_bytes);
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
        {
            th_type *option_type = instruction->op == IR_OP_OPTION_SOME
                ? instruction->dest->type : operands[0].value->type;
            option_repr_t repr;
            if (!option_repr(state->function->owner, option_type,
                    LLVMGetModuleContext(state->module), &repr)) break;
            result = instruction->op == IR_OP_OPTION_SOME
                ? option_some(state, &repr, option_type, result)
                : option_payload(state, &repr, option_type, result);
        }
        if (!result) break;
        return save_result(state, instruction, result);
    case IR_OP_OPTION_IS_NIL: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        option_repr_t repr;
        if (!value || !option_repr(state->function->owner,
                operands[0].value->type,
                LLVMGetModuleContext(state->module), &repr)) break;
        value = option_presence(state, &repr, value);
        if (!value) break;
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
        option_repr_t repr;
        if (!value || !fallback || !option_repr(state->function->owner,
                operands[0].value->type,
                LLVMGetModuleContext(state->module), &repr)) break;
        if (repr.kind == OPTION_REPR_TAGGED
            ? LLVMStructGetTypeAtIndex(repr.storage, 0) != LLVMTypeOf(fallback)
            : repr.kind != OPTION_REPR_PACKED
                && repr.storage != LLVMTypeOf(fallback)) break;
        LLVMValueRef tag = option_presence(state, &repr, value);
        value = tag ? option_payload(state, &repr, operands[0].value->type,
                                     value) : nullptr;
        if (!value || LLVMTypeOf(value) != LLVMTypeOf(fallback)) break;
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
            result = local_slot(state, binding->local);
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
            || operands[1].kind != IR_OPD_NAME || !operands[1].name.data
            || !instruction->dest) break;
        th_type *record_type = operands[0].value
            ? th_ind_referent(operands[0].value->type) : nullptr;
        LLVMTypeRef native = nullptr;
        record_slot_t member_slot = {0};
        LLVMValueRef address = lowered_value(state, operands[0].value);
        if (!record_type || !address) break;
        if (value_type(state->function->owner, record_type,
                LLVMGetModuleContext(state->module))
            == box_llvm_type(LLVMGetModuleContext(state->module))) {
            state->failure_detail =
                FAILURE_DETAIL("boxed record field was not rewritten");
            break;
        }
        if (!record_member_slot(state, record_type, operands[1].name,
                &native, &member_slot)) {
            core_buffer_t message = {0};
            ir_module_t *source = state->function->owner;
            const ir_final_layout_t *layout = ir_layout_final(source,
                record_type, ir_module_target(source));
            LLVMTypeRef candidate = value_type(source, record_type,
                LLVMGetModuleContext(state->module));
            LLVMTargetDataRef target = LLVMGetModuleDataLayout(state->module);
            uint64_t extent = 0;
            if (layout) ir_layout_final_byte_extent(layout, &extent);
            core_string_t printed = th_print(
                ir_module_types(state->function->owner), record_type);
            core_buffer_printf(&message,
                "LLVM backend cannot address field '%.*s' in %.*s "
                "(layout %llu bytes, native %llu bytes)",
                TEXT_ARGS(operands[1].name), TEXT_ARGS(printed),
                (unsigned long long)extent,
                (unsigned long long)(candidate && target
                    ? LLVMABISizeOfType(target, candidate) : 0));
            backend_report(state->diagnostics, core_buffer_string(&message));
            core_buffer_release(&message);
            return false;
        }
        result = LLVMBuildStructGEP2(state->builder, native, address,
                                     member_slot.element, "field.address");
        if (member_slot.overlay)
            result = overlay_member_address(state, result, &member_slot);
        bool bit_reference = uses_bitptr(state->function->owner,
                                         instruction->dest->type);
        if (member_slot.packed && !bit_reference) {
            state->failure_detail =
                FAILURE_DETAIL("packed record field needs a bit reference");
            break;
        }
        if (bit_reference) {
            result = make_packed_bitptr(state, result,
                LLVMConstInt(LLVMInt64TypeInContext(
                    LLVMGetModuleContext(state->module)),
                    member_slot.shift, false),
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
            && LLVMTypeOf(base) == pointer
            && LLVMTypeOf(value) == native_element
            && th_is_closed(ir_module_types(state->function->owner),
                referent)
            && th_kind_of(th_resolve(element)) != TH_SEQUENCE
            && !packed_array_width(state->function->owner, element)
            && literal_element_layout_matches(state->function->owner,
                element, native_element,
                LLVMGetModuleDataLayout(state->module))) {
            LLVMValueRef slot = native_array_slot(state, base, index,
                native_array, native_element,
                ir_machine_of(state->function->owner,
                    operands[1].value->type).class == IR_CLASS_SINT,
                !ir_range_index_proven(state->ranges, instruction));
            value = slot ? escape_boxes(state, value) : nullptr;
            return value && store_gc_barrier(state, value, element)
                && LLVMBuildStore(state->builder, value, slot);
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
            state->failure_detail =
                FAILURE_DETAIL("boxed array write was not rewritten");
            break;
        }
        if (!base || !index || !value
            || LLVMTypeOf(base) != pointer
            || LLVMGetTypeKind(LLVMTypeOf(index)) != LLVMIntegerTypeKind)
            break;
        if (LLVMTypeOf(value) != box)
            value = box_value_with(state, value, operands[2].value->type,
                                   true);
        if (!value || LLVMTypeOf(value) != box) break;
        LLVMValueRef value_slot = entry_alloca(state, box);
        if (!value_slot) break;
        LLVMBuildStore(state->builder, value, value_slot);
        if (!escape_box_contents(state, value_slot)) break;
        LLVMTypeRef parameters[] = {pointer, i64, pointer};
        LLVMTypeRef signature = LLVMFunctionType(
            LLVMVoidTypeInContext(context), parameters, 3, false);
        const char *helper_name =
            "sys.internal.gc.box_store.array_replace_row";
        LLVMValueRef helper = LLVMGetNamedFunction(state->module,
            helper_name);
        if (!helper) helper = LLVMAddFunction(state->module,
            helper_name, signature);
        if (!helper || LLVMGlobalGetValueType(helper) != signature) break;
        LLVMValueRef helper_index = array_helper_index(state, index,
            operands[1].value->type);
        LLVMValueRef location = helper_index ? location_slot(state)
            : nullptr;
        if (!location) break;
        LLVMValueRef locate_arguments[] = {base, helper_index, location};
        LLVMValueRef located = box_runtime_call(state,
            "sys.internal.box.array_locate", LLVMInt1TypeInContext(context),
            parameters, locate_arguments, 3);
        if (!located) break;
        LLVMBasicBlockRef flat = LLVMAppendBasicBlockInContext(context,
            state->target_function, "array.write.flat");
        LLVMBasicBlockRef rows = LLVMAppendBasicBlockInContext(context,
            state->target_function, "array.write.rows");
        LLVMBasicBlockRef written = LLVMAppendBasicBlockInContext(context,
            state->target_function, "array.write.done");
        LLVMBuildCondBr(state->builder, located, flat, rows);
        LLVMPositionBuilderAtEnd(state->builder, flat);
        if (!write_location(state, location, value_slot)) break;
        LLVMBuildBr(state->builder, written);
        LLVMPositionBuilderAtEnd(state->builder, rows);
        LLVMValueRef arguments[] = {base, helper_index, value_slot};
        LLVMBuildCall2(state->builder, signature, helper, arguments, 3, "");
        LLVMBuildBr(state->builder, written);
        LLVMPositionBuilderAtEnd(state->builder, written);
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
            state->failure_detail =
                FAILURE_DETAIL("boxed array length was not rewritten");
            break;
        }
        if (!array || LLVMTypeOf(array) != array_type) break;
        LLVMValueRef descriptor = LLVMBuildExtractValue(state->builder,
            array, 1, "array.length.descriptor");
        LLVMTypeRef descriptor_type = array_descriptor_type(context);
        LLVMValueRef slot = LLVMBuildStructGEP2(state->builder,
            descriptor_type, descriptor, 5, "array.length.slot");
        LLVMValueRef length = load_array_extent(state, slot, "array.length");
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
            state->failure_detail =
                FAILURE_DETAIL("boxed array reference was not rewritten");
            break;
        }
        if (value_read && base) {
            LLVMTypeRef native_sequence = value_type(state->function->owner,
                operands[0].value->type, LLVMGetModuleContext(state->module));
            if (!native_sequence || LLVMTypeOf(base) != native_sequence)
                break;
            if (native_sequence == box_llvm_type(context)) {
                state->failure_detail =
                    FAILURE_DETAIL("boxed array read was not rewritten");
                break;
            }
            LLVMValueRef storage = entry_alloca(state, native_sequence);
            if (!storage) break;
            LLVMBuildStore(state->builder, base, storage);
            base = storage;
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
            if (native_array == box_llvm_type(context)) {
                state->failure_detail =
                    FAILURE_DETAIL("boxed array element was not rewritten");
                break;
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
            limit = load_array_extent(state, length_address, "array.length");
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
            signed_index, !ir_range_index_proven(state->ranges, instruction));
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
                CORE_STRING_STATIC("n00bb.array.direct_ref"));
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
        if (!value_read && !packed_width && result
            && LLVMGetTypeKind(LLVMTypeOf(result)) == LLVMPointerTypeKind
            && uses_bitptr(state->function->owner, instruction->dest->type)) {
            result = make_packed_bitptr(state, result,
                LLVMConstNull(LLVMInt64TypeInContext(context)),
                th_ind_referent(instruction->dest->type));
            if (!result) break;
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
                || !ir_instr_has_mod(instruction, CORE_STRING_STATIC("nearest"))
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
                     && ir_instr_has_mod(instruction, CORE_STRING_STATIC("nearest")))
                result = LLVMBuildFPTrunc(state->builder, source, target,
                                          "cast.fptrunc");
            else if (to.width == from.width) result = source;
            else break;
            return result && save_result(state, instruction, result);
        }
        if (from.class == IR_CLASS_FLOAT && to_integer) {
            if (instruction->n_mods != 2
                || !ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"))
                || !ir_instr_has_mod(instruction, CORE_STRING_STATIC("to_zero"))
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
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"))
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("wrap")))) break;
        if ((from.class != IR_CLASS_SINT && from.class != IR_CLASS_UINT)
            || (to.class != IR_CLASS_SINT && to.class != IR_CLASS_UINT)) break;
        unsigned from_width = LLVMGetIntTypeWidth(LLVMTypeOf(source));
        unsigned to_width = LLVMGetIntTypeWidth(target);
        bool checked = ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"))
            && !overflow_proven(state, instruction);
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
    case IR_OP_ATOMIC_WRAP: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !operands[0].value || !instruction->dest) break;
        LLVMValueRef source = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!source || !target || LLVMTypeOf(source) != target) break;
        return save_result(state, instruction, source);
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
        th_type *source_type = th_resolve(operands[0].value->type);
        th_type *target_type = th_resolve(instruction->dest->type);
        LLVMTypeRef target_native = value_type(state->function->owner,
            target_type, LLVMGetModuleContext(state->module));
        if (target_native && LLVMTypeOf(source) == target_native
            && ir_type_eq(state->function->owner, source_type, target_type))
            return save_result(state, instruction, source);
        bool source_option = th_builtin_applied(source_type, TH_BUILTIN_OPTION,
                                             1) != nullptr;
        bool target_option = th_builtin_applied(target_type, TH_BUILTIN_OPTION,
                                             1) != nullptr;
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
            LLVMValueRef boxed = box_value_with(state, payload, element->type,
                box_in_frame(state, instruction));
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
        result = th_builtin_kind_of(target_type) == TH_BUILTIN_ANY
                && LLVMTypeOf(source) == target_native
                && !th_is_closed(ir_module_types(state->function->owner),
                    source_type)
            ? unwrap_any_box(state, source, target_type)
            : box_value_with(state, source, operands[0].value->type,
                             box_in_frame(state, instruction));
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
        LLVMTypeKind source_kind = LLVMGetTypeKind(LLVMTypeOf(source));
        LLVMTypeKind target_kind = LLVMGetTypeKind(target);
        LLVMTypeRef box = box_llvm_type(LLVMGetModuleContext(state->module));
        result = LLVMTypeOf(source) == target ? source
            : target == box
            ? box_value_with(state, source, operands[0].value->type,
                             box_in_frame(state, instruction))
            : LLVMTypeOf(source) == box
            ? unbox_value(state, source, instruction->dest->type)
            : source_kind == LLVMPointerTypeKind
                && target_kind == LLVMIntegerTypeKind
            ? LLVMBuildPtrToInt(state->builder, source, target, "address")
            : source_kind == LLVMIntegerTypeKind
                && target_kind == LLVMPointerTypeKind
            ? LLVMBuildIntToPtr(state->builder, source, target, "pointer")
            : LLVMBuildBitCast(state->builder, source, target, "bitcast");
        return result && save_result(state, instruction, result);
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
        if (tuple && LLVMTypeOf(tuple) == box_llvm_type(context)) {
            state->failure_detail =
                FAILURE_DETAIL("boxed tuple element was not rewritten");
            break;
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
            || operands[1].kind != IR_OPD_NAME || !operands[1].name.data
            || !instruction->dest || !operands[0].value) break;
        th_type *record_type = th_resolve(operands[0].value->type);
        LLVMTypeRef native = nullptr;
        record_slot_t member_slot = {0};
        LLVMValueRef value = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (value && LLVMTypeOf(value) == box_llvm_type(context)) {
            state->failure_detail =
                FAILURE_DETAIL("boxed record member was not rewritten");
            break;
        }
        if (!value) {
            state->failure_detail =
                FAILURE_DETAIL("record source has no lowered value");
            break;
        }
        if (!record_member_slot(state, record_type, operands[1].name,
                                &native, &member_slot)) {
            th_type *definition = ir_type_definition(
                state->function->owner, record_type);
            const ir_final_layout_t *layout = ir_layout_final(
                state->function->owner, record_type,
                ir_module_target(state->function->owner));
            if (!state->failure_detail.data) state->failure_detail = !definition
                ? FAILURE_DETAIL("record source has no type definition")
                : !layout ? FAILURE_DETAIL("record source has no finalized layout")
                : FAILURE_DETAIL("record field has no matching layout");
            break;
        }
        if (LLVMTypeOf(value) != native) {
            state->failure_detail =
                FAILURE_DETAIL("record source differs from its layout type");
            break;
        }
        result = record_value_get(state, value, &member_slot,
                                  instruction->dest->type);
        if (!result || LLVMTypeOf(result) != value_type(
                state->function->owner, instruction->dest->type,
                LLVMGetModuleContext(state->module))) {
            state->failure_detail =
                FAILURE_DETAIL("record field differs from its result type");
            break;
        }
        return save_result(state, instruction, result);
    }
    case IR_OP_PHI: {
        if (!instruction->dest) break;
        LLVMTypeRef type = native_call_outcome(state->function->owner,
                instruction->dest)
            ? native_result_type(state->module, state->function->owner,
                native_call_outcome_signature(instruction->dest, 0),
                LLVMGetModuleContext(state->module))
            : value_type(state->function->owner,
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
            if (local && type_index) {
                LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
                LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
                LLVMTypeRef parameters[] = {i32};
                LLVMValueRef bytes = box_runtime_call(state,
                    "sys.internal.box.payload_bytes", i64, parameters,
                    &type_index, 1);
                LLVMValueRef bits = bytes ? runtime_sized_payload(state,
                    instruction, type_index, bytes) : nullptr;
                result = bits ? entry_alloca(state, type) : nullptr;
                if (!result) break;
                LLVMBuildStore(state->builder,
                    assemble_box(state, type_index, bits), result);
                return save_result(state, instruction, result);
            }
            state->failure_detail =
                FAILURE_DETAIL("boxed heap allocation was not rewritten");
            break;
        }
        if (instruction->op == IR_OP_ALLOCA
            || ir_allocation_has_only_local_uses(state->function,
                                              instruction)) {
            result = entry_alloca(state, type);
            if (result && instruction->op == IR_OP_ALLOC)
                LLVMBuildStore(state->builder, LLVMConstNull(type), result);
        } else {
            bool required = false;
            core_string_t storage_class = ir_instr_storage_class(instruction,
                                                                 &required);
            if (storage_class.data
                && !core_string_eq(storage_class, CORE_STRING_STATIC("gc")))
                break;
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
            uint64_t stride_bits = 0;
            bool strided = layout
                && ir_layout_final_stride_bits(layout, &stride_bits)
                && stride_bits % 8 == 0;
            bytes = stride_bits / 8;
            if (!layout || !target || !type_index
                || !layout->alignment_bytes
                || !strided
                || LLVMABISizeOfType(target, type) != bytes
                || LLVMABIAlignmentOfType(target, type)
                    != layout->alignment_bytes) {
                core_buffer_t message = {0};
                core_string_t printed = th_print(
                    ir_module_types(state->function->owner),
                    operands[0].type);
                core_buffer_printf(&message,
                    "allocation layout mismatch for %.*s (type index %u, IR size %llu, LLVM size %llu, IR alignment %u, LLVM alignment %u)",
                    TEXT_ARGS(printed),
                    type_index, (unsigned long long)bytes,
                    (unsigned long long)(target
                        ? LLVMABISizeOfType(target, type) : 0),
                    layout ? layout->alignment_bytes : 0,
                    target ? LLVMABIAlignmentOfType(target, type) : 0);
                bool reported = backend_report(state->diagnostics,
                                               core_buffer_string(&message));
                core_buffer_release(&message);
                return reported;
            }
            result = mutator_allocation(state,
                                        runtime_type_index_value(state,
                                            operands[0].type), bytes,
                                        layout->alignment_bytes,
                                        binding->finalizable);
            if (!result) break;
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
            && !n00bb_sequence_transfers_storage(operands[1].value)) {
            value = copy_sequence_value(state, value,
                                        operands[0].value->global->type);
            if (!value) break;
        }
        value = escape_boxes(state, value);
        if (!value) break;
        LLVMBuildStore(state->builder, value, global);
        return mark_global_initialized(state, operands[0].value->global);
    }
    case IR_OP_LOAD: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest
            || instruction->n_mods > 2
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("atomic"))
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("volatile")))
            || ((ir_instr_has_mod(instruction, CORE_STRING_STATIC("relaxed"))
                 || ir_instr_has_mod(instruction, CORE_STRING_STATIC("acquire")))
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("atomic")))
            || ir_instr_has_mod(instruction, CORE_STRING_STATIC("release")))
            break;
        global_load_guard_t guard = {.state = state, .valid = true};
        ir_value_visit_store_origins(operands[0].value,
            guard_global_load_origin, &guard);
        if (!guard.valid) {
            state->failure_detail =
                FAILURE_DETAIL("load is blocked by a global read guard");
            return false;
        }
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMTypeRef type = value_type(state->function->owner,
            instruction->dest->type, LLVMGetModuleContext(state->module));
        if (!address || !type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) {
            state->failure_detail =
                FAILURE_DETAIL("load has no lowerable address or value type");
            break;
        }
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        if (LLVMTypeOf(address) == box_llvm_type(context)) {
            state->failure_detail =
                FAILURE_DETAIL("boxed reference read was not rewritten");
            break;
        }
        th_type *storage = th_ind_referent(operands[0].value->type);
        if (uses_bitptr(state->function->owner,
                        operands[0].value->type)) {
            if (instruction->n_mods || !storage
                || !packed_value_width(state->function->owner, storage)) break;
            if (LLVMTypeOf(address) != bitptr_llvm_type(
                    LLVMGetModuleContext(state->module))) {
                const ir_instr_t *source = operands[0].value->def;
                core_buffer_t message = {0};
                core_string_t producer = source
                    ? ir_op_spelling(source->op)
                    : CORE_STRING_STATIC("an input value");
                core_buffer_printf(&message,
                    "packed value load has a non-bitptr address from %.*s",
                    TEXT_ARGS(producer));
                bool reported = backend_report(state->diagnostics,
                                               core_buffer_string(&message));
                core_buffer_release(&message);
                return reported;
            }
            result = load_packed_bitptr(state, address, storage);
            bool saved = result && save_result(state, instruction, result);
            if (!saved)
                state->failure_detail =
                    FAILURE_DETAIL("packed reference load cannot root its result");
            return saved;
        }
        bool atomic = ir_instr_has_mod(instruction, CORE_STRING_STATIC("atomic"));
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
                ir_instr_has_mod(instruction, CORE_STRING_STATIC("relaxed"))
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, CORE_STRING_STATIC("acquire"))
                        ? LLVMAtomicOrderingAcquire
                        : LLVMAtomicOrderingSequentiallyConsistent);
        }
        if (ir_instr_has_mod(instruction, CORE_STRING_STATIC("volatile")))
            LLVMSetVolatile(loaded, true);
        result = atomic_bool
            ? LLVMBuildTrunc(state->builder, loaded, type, "atomic.bool")
            : loaded;
        bool saved = save_result(state, instruction, result);
        if (!saved)
            state->failure_detail = FAILURE_DETAIL(
                "native reference load cannot root its result");
        return saved;
    }
    case IR_OP_STORE: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_VALUE
            || instruction->n_mods > 2
            || (instruction->n_mods && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("atomic")))
            || ir_instr_has_mod(instruction, CORE_STRING_STATIC("acquire")))
            break;
        LLVMValueRef address = lowered_value(state, operands[0].value);
        LLVMValueRef value = lowered_value(state, operands[1].value);
        if (!address || !value) break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        th_type *storage = th_ind_referent(operands[0].value->type);
        if (LLVMTypeOf(address) == box_llvm_type(context)) {
            state->failure_detail =
                FAILURE_DETAIL("boxed reference write was not rewritten");
            break;
        }
        if (uses_bitptr(state->function->owner,
                        operands[0].value->type)) {
            if (instruction->n_mods || !storage
                || !packed_value_width(state->function->owner, storage)
                || LLVMTypeOf(address) != bitptr_llvm_type(
                    LLVMGetModuleContext(state->module)))
            {
                core_buffer_t message = {0};
                core_string_t module_name = ir_module_name(
                    state->function->owner);
                core_string_t printed = th_print(
                    ir_module_types(state->function->owner),
                    operands[0].value->type);
                core_string_t producer = operands[0].value->def
                    ? ir_op_spelling(operands[0].value->def->op)
                    : CORE_STRING_STATIC("value");
                core_buffer_printf(&message,
                    "packed value store unsupported in %.*s.%.*s for %.*s (width %llu, address kind %d, producer %.*s)",
                    TEXT_ARGS(module_name),
                    TEXT_ARGS(state->function->name),
                    TEXT_ARGS(printed),
                    (unsigned long long)packed_value_width(
                        state->function->owner, storage),
                    (int)LLVMGetTypeKind(LLVMTypeOf(address)),
                    TEXT_ARGS(producer));
                bool reported = backend_report(state->diagnostics,
                    core_buffer_string(&message));
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
            && !n00bb_sequence_transfers_storage(operands[1].value)) {
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
        if (!direct_stack_address(state->function, operands[0].value)) {
            value = escape_boxes(state, value);
            if (!value) break;
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
                ir_instr_has_mod(instruction, CORE_STRING_STATIC("relaxed"))
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, CORE_STRING_STATIC("release"))
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
            || !operands[1].name.data
            || (!core_string_eq(operands[1].name, CORE_STRING_STATIC("or"))
                && !core_string_eq(operands[1].name, CORE_STRING_STATIC("and"))
                && !core_string_eq(operands[1].name,
                                   CORE_STRING_STATIC("xchg"))
                && !core_string_eq(operands[1].name, CORE_STRING_STATIC("add"))
                && !core_string_eq(operands[1].name,
                                   CORE_STRING_STATIC("sub")))
            || operands[2].kind != IR_OPD_VALUE || !instruction->dest
            || (instruction->n_mods != 0
                && (instruction->n_mods != 1
                    || (!ir_instr_has_mod(instruction, CORE_STRING_STATIC("release"))
                        && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("relaxed"))
                        && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("acquire"))))))
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
                || !core_string_eq(operands[1].name,
                                   CORE_STRING_STATIC("xchg"))))
            || layout->intrinsic_bits != width
            || layout->alignment_bytes < width / 8)
            break;
        LLVMAtomicRMWBinOp operation = core_string_eq(operands[1].name,
                CORE_STRING_STATIC("xchg"))
            ? LLVMAtomicRMWBinOpXchg
            : core_string_eq(operands[1].name, CORE_STRING_STATIC("or"))
                ? LLVMAtomicRMWBinOpOr
                : core_string_eq(operands[1].name, CORE_STRING_STATIC("and"))
                    ? LLVMAtomicRMWBinOpAnd
                : core_string_eq(operands[1].name, CORE_STRING_STATIC("add"))
                    ? LLVMAtomicRMWBinOpAdd : LLVMAtomicRMWBinOpSub;
        result = LLVMBuildAtomicRMW(state->builder, operation,
            address, value,
            ir_instr_has_mod(instruction, CORE_STRING_STATIC("release"))
                ? LLVMAtomicOrderingRelease
                : ir_instr_has_mod(instruction, CORE_STRING_STATIC("relaxed"))
                    ? LLVMAtomicOrderingMonotonic
                    : ir_instr_has_mod(instruction, CORE_STRING_STATIC("acquire"))
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
    case IR_OP_BUFFER_AS_STRING:
    case IR_OP_BUFFER_BORROW_END: {
        bool begin = instruction->op == IR_OP_BUFFER_AS_STRING;
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || begin != (instruction->dest != nullptr)) break;
        bool held = !begin && operands[0].value->type
            && th_option_item(th_resolve(operands[0].value->type));
        const char *name = begin
            ? "builtin.containers.buffer.buffer_borrow_begin"
            : held ? "builtin.containers.buffer.buffer_borrow_end_if_held"
            : "builtin.containers.buffer.buffer_borrow_end";
        LLVMValueRef buffer = lowered_value(state, operands[0].value);
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMTypeRef view = begin ? value_type(state->function->owner,
            instruction->dest->type, context)
            : LLVMVoidTypeInContext(context);
        if (!buffer || !view) break;
        LLVMTypeRef parameter = LLVMTypeOf(buffer);
        LLVMTypeRef expected = LLVMFunctionType(view, &parameter, 1, false);
        LLVMValueRef helper = LLVMGetNamedFunction(state->module, name);
        if (!helper) helper = LLVMAddFunction(state->module, name, expected);
        LLVMTypeRef signature = LLVMGlobalGetValueType(helper);
        if (signature != expected) {
            state->failure_detail =
                FAILURE_DETAIL("buffer borrow helper signature differs");
            break;
        }
        result = LLVMBuildCall2(state->builder, signature, helper, &buffer,
                                1, begin ? "buffer.borrow" : "");
        if (!result) break;
        return !begin || save_result(state, instruction, result);
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
        bool dynamic = !th_is_closed(ir_module_types(state->function->owner),
            source_type) || th_kind_of(source_type) == TH_SUM
            || th_builtin_applied(source_type, TH_BUILTIN_ANY, 0);
        if (dynamic) {
            state->failure_detail =
                FAILURE_DETAIL("boxed typeof was not rewritten");
            break;
        }
        result = runtime_type_index_value(state, source_type);
        return result && save_result(state, instruction, result);
    }
    case IR_OP_TYPE_INDEX: {
        if (count != 1 || operands[0].kind != IR_OPD_TYPE
            || !instruction->dest) break;
        result = ir_annot_get(&instruction->annots,
                CORE_STRING_STATIC("n00bb.type_index.static"))
            ? runtime_type_index_value(state, operands[0].type)
            : runtime_type_value(state, operands[0].type);
        return result && save_result(state, instruction, result);
    }
    case IR_OP_BOX_RAW:
    case IR_OP_BOX_FROM_RAW: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || !instruction->dest) break;
        LLVMContextRef context = LLVMGetModuleContext(state->module);
        LLVMValueRef source = lowered_value(state, operands[0].value);
        LLVMTypeRef target = value_type(state->function->owner,
            instruction->dest->type, context);
        LLVMTypeRef box = box_llvm_type(context);
        if (!source || !target) break;
        if (instruction->op == IR_OP_BOX_RAW) {
            if (LLVMTypeOf(source) != box) break;
            result = same_shape_struct(state, source, target);
        }
        else {
            LLVMValueRef boxed = same_shape_struct(state, source, box);
            result = boxed && target != box
                ? unbox_value(state, boxed, instruction->dest->type) : boxed;
        }
        return result && save_result(state, instruction, result);
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
        if (value && LLVMTypeOf(value) == box_llvm_type(context)) {
            state->failure_detail =
                FAILURE_DETAIL("boxed typecase was not rewritten");
            break;
        }
        LLVMValueRef type_field = operands[0].value && th_is_closed(
                ir_module_types(state->function->owner),
                operands[0].value->type)
            ? runtime_type_index_value(state, operands[0].value->type)
            : nullptr;
        if (!type_field || !th_is_closed(
                ir_module_types(state->function->owner), operands[1].type)) {
            state->failure_detail =
                FAILURE_DETAIL("typecase subject has no runtime type");
            break;
        }
        uint32_t index = runtime_type_index(state, operands[1].type);
        if (!index) break;
        LLVMValueRef actual = type_field;
        actual = LLVMBuildAnd(state->builder, actual,
            LLVMConstInt(LLVMTypeOf(actual), UINT32_C(0x7fffffff), false),
            "type.index.masked");
        result = LLVMBuildICmp(state->builder, LLVMIntEQ, actual,
            runtime_type_index_value(state, operands[1].type),
            "type.matches");
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
        bool tuple = th_builtin_kind_of(aggregate) == TH_BUILTIN_TUPLE;
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
            state->failure_detail =
                FAILURE_DETAIL("boxed array shape check was not rewritten");
            break;
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
    case IR_OP_NEG: {
        if (count != 1 || operands[0].kind != IR_OPD_VALUE
            || instruction->n_mods > 1
            || (instruction->n_mods
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap")))) break;
        LLVMValueRef value = lowered_value(state, operands[0].value);
        if (!value) break;
        LLVMTypeRef type = LLVMTypeOf(value);
        bool checked = false;
        if (floating_type(type)) {
            if (instruction->n_mods) break;
            result = LLVMBuildFNeg(state->builder, value, "negated");
        } else if (LLVMGetTypeKind(type) == LLVMIntegerTypeKind) {
            checked = instruction->n_mods && !overflow_proven(state,
                                                              instruction);
            if (!checked) {
                result = LLVMBuildNeg(state->builder, value, "negated");
            } else {
                ir_machine_t machine = ir_machine_of(state->function->owner,
                    operands[0].value->type);
                core_string_t name = machine.class == IR_CLASS_SINT
                    ? CORE_STRING_STATIC("llvm.ssub.with.overflow")
                    : machine.class == IR_CLASS_UINT
                        ? CORE_STRING_STATIC("llvm.usub.with.overflow")
                        : (core_string_t){0};
                if (!name.data) break;
                unsigned id = LLVMLookupIntrinsicID(name.data, name.u8_bytes);
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
            operands[0].value->type, checked);
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
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"))
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("wrap")))) break;
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
        bool safe_sub = overflow_proven(state, instruction)
            || (instruction->op == IR_OP_SUB
                && ir_range_unsigned_sub_safe(state->ranges, instruction));
        if (ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap")) && !safe_sub) {
            ir_machine_t machine = ir_machine_of(state->function->owner,
                operands[0].value->type);
            core_string_t intrinsic_name = {0};
            if (machine.class == IR_CLASS_SINT)
                intrinsic_name = instruction->op == IR_OP_ADD
                    ? CORE_STRING_STATIC("llvm.sadd.with.overflow")
                    : instruction->op == IR_OP_SUB
                        ? CORE_STRING_STATIC("llvm.ssub.with.overflow")
                        : CORE_STRING_STATIC("llvm.smul.with.overflow");
            else if (machine.class == IR_CLASS_UINT)
                intrinsic_name = instruction->op == IR_OP_ADD
                    ? CORE_STRING_STATIC("llvm.uadd.with.overflow")
                    : instruction->op == IR_OP_SUB
                        ? CORE_STRING_STATIC("llvm.usub.with.overflow")
                        : CORE_STRING_STATIC("llvm.umul.with.overflow");
            if (!intrinsic_name.data) break;
            unsigned id = LLVMLookupIntrinsicID(intrinsic_name.data,
                intrinsic_name.u8_bytes);
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
            ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap")) && !safe_sub);
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
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap")))) break;
        ir_machine_t machine = ir_machine_of(state->function->owner,
                                             operands[0].value->type);
        if (machine.class != IR_CLASS_SINT
            && machine.class != IR_CLASS_UINT) break;
        LLVMValueRef zero = LLVMConstNull(type);
        bool nonzero = ir_range_site_proven(state->ranges, instruction,
                                            IR_SITE_DIVISOR);
        bool proven = overflow_proven(state, instruction);
        if (!nonzero)
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntEQ,
                right, zero, "div.zero"));
        if (machine.class == IR_CLASS_SINT && !proven) {
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
            operands[0].value->type, instruction->n_mods != 0 && !proven);
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
        bool trapping = ir_instr_has_mod(instruction, CORE_STRING_STATIC("trap"));
        if (count != 2 || instruction->n_mods > 1
            || (instruction->n_mods && !trapping
                && !ir_instr_has_mod(instruction, CORE_STRING_STATIC("wrap")))
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
        bool count_proven = ir_range_site_proven(state->ranges, instruction,
                                                 IR_SITE_SHIFT_AMOUNT);
        if (instruction->n_mods && !count_proven) {
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
        bool overflow_checked = instruction->op == IR_OP_SHL && trapping
            && !overflow_proven(state, instruction);
        if (overflow_checked) {
            LLVMValueRef restored = value_machine.class == IR_CLASS_SINT
                ? LLVMBuildAShr(state->builder, result, amount,
                                "shift.restored")
                : LLVMBuildLShr(state->builder, result, amount,
                                "shift.restored");
            emit_trap_if(state, LLVMBuildICmp(state->builder, LLVMIntNE,
                restored, left, "shift.overflow"));
        }
        result = semantic_integer_result(state, result,
            operands[0].value->type, overflow_checked);
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
        } else if (boxed_reference) {
            state->failure_detail =
                FAILURE_DETAIL("boxed reference compare was not rewritten");
            break;
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
                          &named_variant, nullptr)
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
            LLVMConstInt(dest, th_arm_tag(CORE_STRING_STATIC("ok")), false),
            LLVMConstInt(dest, th_arm_tag(CORE_STRING_STATIC("error")), false),
            "variant.tag");
        return save_result(state, instruction, result);
    }
    case IR_OP_VARIANT_PAYLOAD: {
        if (count != 2 || operands[0].kind != IR_OPD_VALUE
            || operands[1].kind != IR_OPD_NAME
            || !operands[0].value
            || !instruction->dest) break;
        ir_module_t *owner = state->function->owner;
        if (ir_machine_of(owner, instruction->dest->type).class
            == IR_CLASS_VOID) return true;
        const ir_final_layout_t *layout = nullptr;
        uint64_t stride = 0;
        bool named_variant = false;
        if (th_named_info(th_resolve(operands[0].value->type), nullptr,
                          &named_variant, nullptr)
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
                ? th_record_lookup(definition, CORE_STRING_STATIC("ok"))
                : nullptr;
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
            ? runtime_function_index_value(state, handler->owner) : nullptr;
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
            runtime_type_index_value(state, callback_type), 1,
            "callback.type");
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
                        && setup_target->params[at]->name.data
                        && core_string_eq(setup_target->params[at]->name,
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
            state->failure_detail = FAILURE_DETAIL(
                "callback value is not a native callback record");
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
            state->failure_detail =
                FAILURE_DETAIL("callback handler signature is not native");
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
            append_function_symbol(&name, binding->handler);
            LLVMValueRef target = name.data
                ? named_function(state->module, core_buffer_string(&name))
                : nullptr;
            if (!target && name.data)
                target = add_function(state->module,
                    core_buffer_string(&name), concrete);
            core_buffer_release(&name);
            if (!target || LLVMGlobalGetValueType(target) != concrete) {
                state->failure_detail =
                    FAILURE_DETAIL("callback direct target has a conflicting native signature");
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
                    runtime_function_index_value(state, binding->handler),
                    "callback.handler.match"),
                LLVMBuildICmp(state->builder, LLVMIntEQ,
                    callback_type_index,
                    runtime_type_index_value(state, binding->callback_type),
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
                    FAILURE_DETAIL("could not allocate callback arguments");
                return unsupported(state, instruction);
            }
            bool adapted = true;
            for (uint32_t j = 1; adapted && j < count; j++) {
                const th_param *formal = th_func_param(
                    binding->handler_signature, j - 1);
                LLVMValueRef value = operands[j].kind == IR_OPD_VALUE
                    ? lowered_value(state, operands[j].value) : nullptr;
                if (value && formal)
                    value = adapt_call_argument(state, value,
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
                    FAILURE_DETAIL("callback direct call arguments cannot be adapted");
                return unsupported(state, instruction);
            }
            if (instruction->dest) {
                called = adapt_call_result(state, called,
                    binding->handler_signature, handler_type);
                if (!called || LLVMTypeOf(called)
                    != LLVMGetReturnType(signature)) {
                    state->failure_detail =
                        FAILURE_DETAIL("callback direct result cannot be adapted");
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
                && (LLVMTypeOf(arguments[ordinal]) != expected[ordinal]
                    || crosses_any_boundary(state, operands[i].value->type,
                        formal->type)))
                arguments[ordinal] = adapt_call_argument(state,
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
            output = escape_boxes(state, output);
            valid = output && store_gc_barrier(state, output, state_type)
                && LLVMBuildStore(state->builder, output, environment);
            if (done) {
                LLVMBuildBr(state->builder, done);
                LLVMPositionBuilderAtEnd(state->builder, done);
            }
        }
        free(arguments);
        free(expected);
        if (!valid || !result) {
            state->failure_detail = FAILURE_DETAIL(
                "callback arguments cannot match native handler");
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
            ? n00bb_cached_body(operands[0].value->owner) : nullptr;
        ir_annot_t *native_call = target
            ? ir_annot_get(&target->annots,
                           CORE_STRING_STATIC("n00bb.abi.native")) : nullptr;
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s.data
            && core_string_eq(native_call->s,
                              CORE_STRING_STATIC("__n00bb_array_clear"))) {
            if (count != 3 || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i32 = LLVMInt32TypeInContext(context);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMValueRef address = lowered_value(state, operands[1].value);
            LLVMValueRef index = lowered_value(state, operands[2].value);
            th_type *cleared = th_ind_referent(operands[1].value->type);
            th_type *element = cleared
                && th_kind_of(th_resolve(cleared)) == TH_SEQUENCE
                ? th_seq_elem(th_resolve(cleared)) : nullptr;
            ir_module_t *owner = state->function->owner;
            LLVMTypeRef native_array = element
                ? value_type(owner, cleared, context) : nullptr;
            LLVMTypeRef native_element = element
                ? value_type(owner, element, context) : nullptr;
            if (address && LLVMTypeOf(address) == pointer && index
                && native_array && native_array != box && native_element
                && native_element != box
                && th_is_closed(ir_module_types(owner), cleared)
                && th_kind_of(th_resolve(element)) != TH_SEQUENCE
                && !packed_array_width(owner, element)
                && literal_element_layout_matches(owner, element,
                    native_element, LLVMGetModuleDataLayout(state->module))) {
                LLVMValueRef slot = native_array_slot(state, address, index,
                    native_array, native_element,
                    ir_machine_of(owner, operands[2].value->type).class
                        == IR_CLASS_SINT, true);
                if (!slot || !LLVMBuildStore(state->builder,
                        LLVMConstNull(native_element), slot)) break;
                return true;
            }
            LLVMValueRef array_type_index = nullptr;
            if (address && LLVMTypeOf(address) == box) {
                state->failure_detail =
                    FAILURE_DETAIL("boxed array clear was not rewritten");
                break;
            }
            if (address) {
                th_type *referent = th_ind_referent(
                    operands[1].value->type);
                array_type_index = referent
                    ? runtime_type_value(state, referent) : nullptr;
            }
            if (!address || LLVMTypeOf(address) != pointer
                || !array_type_index || LLVMTypeOf(array_type_index) != i32
                || !index || LLVMTypeOf(index) != i64) break;
            LLVMTypeRef parameters[] = {pointer, i32, i64};
            LLVMValueRef args[] = {address, array_type_index, index};
            return box_runtime_call(state, "sys.internal.box.array_clear_slot",
                LLVMVoidTypeInContext(context), parameters, args, 3)
                != nullptr;
        }
        if (native_call && native_call->kind == IR_ANNOT_STR
            && native_call->s.data
            && core_string_eq(native_call->s,
                              CORE_STRING_STATIC("__n00bb_array_prefix"))) {
            if (count != 3 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE
                || operands[2].kind != IR_OPD_VALUE) break;
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
            LLVMTypeRef i64 = LLVMInt64TypeInContext(context);
            LLVMValueRef source = lowered_value(state, operands[1].value);
            LLVMValueRef length = lowered_value(state, operands[2].value);
            if (!source || !length || LLVMTypeOf(length) != i64) break;
            if (LLVMTypeOf(source) == box) {
                state->failure_detail =
                    FAILURE_DETAIL("boxed array prefix was not rewritten");
                break;
            }
            source = box_value_with(state, source, operands[1].value->type,
                                    true);
            LLVMValueRef source_slot = source ? entry_alloca(state, box)
                                                : nullptr;
            LLVMValueRef result_slot = source_slot
                ? entry_alloca(state, box) : nullptr;
            if (!result_slot) break;
            LLVMBuildStore(state->builder, source, source_slot);
            LLVMTypeRef parameters[] = {pointer, i64, pointer};
            LLVMValueRef args[] = {source_slot, length, result_slot};
            if (!box_runtime_call(state,
                    "sys.internal.gc.box_store.array_prefix",
                    LLVMVoidTypeInContext(context), parameters, args, 3))
                break;
            result = LLVMBuildLoad2(state->builder, box, result_slot,
                                    "array.prefix.box");
            result = adapt_call_value_with(state, result,
                operands[1].value->type, instruction->dest->type,
                box_in_frame(state, instruction));
            return result && save_result(state, instruction, result);
        }
        ir_annot_t *native = target
            ? ir_annot_get(&target->annots,
                           CORE_STRING_STATIC("n00bb.abi.native")) : nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s.data
            && core_string_eq(native->s,
                CORE_STRING_STATIC("n00b_builtin_abi_ref_address"))) {
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
        if (native && native->kind == IR_ANNOT_STR && native->s.data
            && (core_string_eq(native->s, CORE_STRING_STATIC("n00bb_box_of"))
                || core_string_eq(native->s,
                    CORE_STRING_STATIC("n00bb_result_box"))
                || core_string_eq(native->s,
                    CORE_STRING_STATIC("n00bb_box_as")))) {
            LLVMContextRef context = LLVMGetModuleContext(state->module);
            LLVMTypeRef box = box_llvm_type(context);
            if (!instruction->dest) break;
            if (core_string_eq(native->s,
                    CORE_STRING_STATIC("n00bb_result_box"))) {
                if (count != 1 || !state->result_box) break;
                return save_result(state, instruction, state->result_box);
            }
            LLVMValueRef operand = count == 2
                && operands[1].kind == IR_OPD_VALUE
                ? lowered_value(state, operands[1].value) : nullptr;
            if (!operand) break;
            if (core_string_eq(native->s, CORE_STRING_STATIC("n00bb_box_of"))) {
                if (LLVMTypeOf(operand) != box)
                    operand = box_value_with(state, operand,
                        operands[1].value->type, true);
                LLVMValueRef slot = operand && LLVMTypeOf(operand) == box
                    ? entry_alloca(state, box) : nullptr;
                if (!slot) break;
                LLVMBuildStore(state->builder, operand, slot);
                return save_result(state, instruction, slot);
            }
            if (LLVMGetTypeKind(LLVMTypeOf(operand)) != LLVMPointerTypeKind)
                break;
            result = LLVMBuildLoad2(state->builder, box, operand, "box.as");
            return save_result(state, instruction, result);
        }
        if (native && native->kind == IR_ANNOT_STR && native->s.data
            && (core_string_eq(native->s,
                    CORE_STRING_STATIC("n00bb_load_le_u64"))
                || core_string_eq(native->s,
                    CORE_STRING_STATIC("n00bb_load_le_u32")))) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            LLVMValueRef address = lowered_value(state, operands[1].value);
            LLVMTypeRef integer = value_type(state->function->owner,
                instruction->dest->type, LLVMGetModuleContext(state->module));
            if (!address || !integer
                || LLVMGetTypeKind(LLVMTypeOf(address)) != LLVMPointerTypeKind
                || LLVMGetTypeKind(integer) != LLVMIntegerTypeKind) break;
            result = LLVMBuildLoad2(state->builder, integer, address,
                                    "load.le");
            LLVMSetAlignment(result, 1);
            return save_result(state, instruction, result);
        }
        if (native && native->kind == IR_ANNOT_STR && native->s.data
            && core_string_eq(native->s, CORE_STRING_STATIC("n00bb_bitptr"))) {
            if (count != 2 || !instruction->dest
                || operands[1].kind != IR_OPD_VALUE) break;
            result = reference_bitptr(state,
                lowered_value(state, operands[1].value),
                operands[1].value->type, instruction->dest->type);
            return result && save_result(state, instruction, result);
        }
        bool mutator_call = !native && target
            && inline_list_contains(state->mutator_functions, target);
        LLVMValueRef callee = nullptr;
        if (native && native->kind == IR_ANNOT_STR && native->s.data) {
            LLVMTypeRef signature = function_type(state->module, target);
            if (!signature) break;
            callee = named_function(state->module, native->s);
            if (!callee)
                callee = add_function(state->module, native->s, signature);
            if (!callee || LLVMGlobalGetValueType(callee) != signature) break;
        } else if (mutator_call) {
            callee = mutator_function(state->module, target);
            if (!callee) {
                LLVMTypeRef type = mutator_function_type(state->module,
                                                         target);
                core_buffer_t name = {0};
                mutator_symbol(&name, target);
                if (type) callee = add_function(state->module,
                    core_buffer_string(&name), type);
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
        unsigned result_base = !native && formal_signature
            && returns_through_box(target ? target->owner
                                          : state->function->owner,
                formal_signature, LLVMGetModuleContext(state->module))
            ? 1 : 0;
        if (!callee || !signature
            || LLVMGetTypeKind(signature) != LLVMFunctionTypeKind
            || !formal_signature
            || th_func_nparams(formal_signature) != count - 1
            || native_count != count - 1 - optional_count
                + (optional_count != 0) + (mutator_call ? 1 : 0)
                + result_base
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
        size_t native_index = result_base, optional_index = 0;
        uint64_t omissions = 0;
        if (valid && result_base) {
            arguments[0] = prepare_result_box(state, instruction,
                                              formal_signature);
            valid = arguments[0] != nullptr;
        }
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
                        th_ctx *types = ir_module_types(
                            state->function->owner);
                        core_string_t referent_name = th_print(types,
                                                               referent);
                        core_string_t target_name = target ? target->name
                            : CORE_STRING_STATIC("indirect call");
                        core_string_t expected_name = th_print(types,
                                                               expected);
                        core_buffer_printf(&message,
                            "fresh construction target has type %.*s but '%.*s' requires %.*s",
                            TEXT_ARGS(referent_name), TEXT_ARGS(target_name),
                            TEXT_ARGS(expected_name));
                        backend_report(state->diagnostics,
                                       core_buffer_string(&message));
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
                        core_string_t referent_name = th_print(
                            ir_module_types(state->function->owner),
                            referent);
                        core_buffer_printf(&message,
                            "fresh construction type has no native representation: %.*s",
                            TEXT_ARGS(referent_name));
                        backend_report(state->diagnostics,
                                       core_buffer_string(&message));
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
                        LLVMValueRef reference = entry_alloca(state, type);
                        LLVMValueRef value_slot = entry_alloca(state, type);
                        if (!reference || !value_slot)
                            storage = nullptr;
                        else {
                            LLVMBuildStore(state->builder, storage,
                                           reference);
                            LLVMValueRef args[] = {reference, value_slot};
                            box_runtime_call(state,
                                "sys.internal.box.reference_view",
                                LLVMVoidTypeInContext(context), params, args,
                                2);
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
                    if (input && (LLVMTypeOf(input) != type
                        || crosses_any_boundary(state,
                            operands[i].value->type, formal->type)))
                        input = adapt_call_argument(state, input,
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
                    if (value && (LLVMTypeOf(value) != expected
                        || crosses_any_boundary(state,
                            operands[i].value->type, formal->type)))
                        value = adapt_call_argument(state, value,
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
                    && (LLVMTypeOf(arguments[native_index]) != expected
                        || crosses_any_boundary(state,
                            operands[i].value->type, formal->type)))
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
                        : adapt_call_argument(state,
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
                LLVMValueRef reference = LLVMBuildLoad2(state->builder,
                    box_llvm_type(LLVMGetModuleContext(state->module)),
                    fresh_original[i], "fresh.reference");
                valid = write_location(state,
                    reference_location(state, reference), fresh_boxed[i]);
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
                    FAILURE_DETAIL("callee result cannot be adapted to the call-site type");
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
            result = adapt_call_value_with(state, result, output->type,
                instruction->dest->type, box_in_frame(state, instruction));
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
                state->parameter_base + (unsigned)i);
            value = escape_boxes(state, value);
            if (!value || !(formal->mode == TH_INOUT
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
                && !n00bb_return_transfers_storage(state->function,
                                                    operands[0].value)) {
                result = copy_sequence_value(state, result, success);
                if (!result) break;
            }
            result = state->result_box ? fill_result_box(state, result)
                : escape_boxes(state, result);
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
        if (result && th_kind_of(th_resolve(
                th_func_error(state->function->type))) == TH_SEQUENCE
            && !n00bb_return_transfers_storage(state->function,
                                                operands[0].value))
            result = copy_sequence_value(state, result,
                th_func_error(state->function->type));
        result = escape_boxes(state, result);
        return result && emit_outcome_return(state, result, true);
    case IR_OP_TRAP:
        if (count != 0) break;
        {
            core_string_t path = instruction->span.file >= 0
                ? ir_file_path(state->function->owner, instruction->span.file)
                : (core_string_t){0};
            core_buffer_t reason = {0};
            if (path.data)
                core_buffer_printf(&reason, "check failed at %.*s:%d:%d",
                    TEXT_ARGS(path), instruction->span.start_line,
                    instruction->span.start_col);
            else core_buffer_puts(&reason, "check failed");
            emit_trap_reason(state, core_buffer_string(&reason));
            core_buffer_release(&reason);
        }
        return true;
    default:
        break;
    }
    return unsupported(state, instruction);
}

static bool
returns_through_box(ir_module_t *module, th_type *signature,
                    LLVMContextRef context)
{
    th_type *success = signature ? th_func_ret(signature) : nullptr;
    return success && !th_is_closed(ir_module_types(module), success)
        && value_type(module, success, context) == box_llvm_type(context);
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
    unsigned base = !native_binding
        && returns_through_box(owner, signature, context) ? 1 : 0;
    LLVMTypeRef *parameters = count + base
        ? calloc(count + base, sizeof(*parameters)) : nullptr;
    if (count + base && !parameters) return nullptr;
    if (base) parameters[0] = LLVMPointerTypeInContext(context, 0);
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
        parameters[base + i] = th_mode_produces_output(parameter->mode)
            ? LLVMPointerTypeInContext(context, 0) : value;
    }
    LLVMTypeRef native_signature = LLVMFunctionType(result, parameters,
        (unsigned)(count + base), false);
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
            ir_annot_get(&function->annots,
                         CORE_STRING_STATIC("n00bb.abi.native")) != nullptr);
    LLVMTypeRef record = optional_record_type(target, function);
    LLVMContextRef context = LLVMGetModuleContext(target);
    LLVMTypeRef result = native_result_type(target, function->owner,
                                            function->type, context);
    if (!record || !result || count > UINT32_MAX || th_func_nkw(function->type)
        || th_func_rest(function->type)) return nullptr;
    LLVMTypeRef *parameters = calloc(count - optional_count + 2,
                                     sizeof(*parameters));
    if (!parameters) return nullptr;
    size_t native_index = 0;
    if (returns_through_box(function->owner, function->type, context))
        parameters[native_index++] = LLVMPointerTypeInContext(context, 0);
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

static void
mutator_symbol(core_buffer_t *name, ir_func_t *function)
{
    append_function_symbol(name, function);
    core_buffer_puts(name, "$gc_mutator");
}

static LLVMValueRef
mutator_function(LLVMModuleRef target, ir_func_t *function)
{
    core_buffer_t name = {0};
    mutator_symbol(&name, function);
    LLVMValueRef result = named_function(target, core_buffer_string(&name));
    core_buffer_release(&name);
    return result;
}

static bool
lower_mutator_wrapper(LLVMModuleRef target, ir_func_t *function,
                      core_diagnostics_t *diagnostics)
{
    core_buffer_t name = {0};
    append_function_symbol(&name, function);
    LLVMValueRef wrapper = named_function(target, core_buffer_string(&name));
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

static bool
frame_root_slot(void *opaque, n00bb_frame_root_kind_t kind,
                size_t index, ir_value_t *value, ir_local_t *local,
                ir_instr_t *instruction)
{
    (void)local;
    lower_state_t *state = opaque;
    if (frame_root_unreachable_narrow(state, kind, value, instruction)
        || !frame_root_needs_slot(state, kind, value, local)) return true;
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
collect_conservative_spills(ir_func_t *function,
                            core_indexed_table_t *spills)
{
    size_t total = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            const ir_instr_t *instruction = function->blocks[b]->instrs[i];
            total += instruction->dest_kind == IR_DEST_VALUE
                && instruction->dest;
        }
    ir_value_t **values = core_alloc_array(ir_value_t *, total ? total : 1);
    bool *across = core_alloc_array(bool, total ? total : 1);
    size_t next = 0;
    for (uint32_t b = 0; b < function->n_blocks; b++)
        for (uint32_t i = 0; i < function->blocks[b]->n_instrs; i++) {
            ir_instr_t *instruction = function->blocks[b]->instrs[i];
            if (instruction->dest_kind == IR_DEST_VALUE && instruction->dest)
                values[next++] = instruction->dest;
        }
    if (!ir_live_across_points(ir_func_liveness(function), values, total,
                               n00bb_frame_call_point, nullptr, across))
        for (size_t v = 0; v < total; v++) across[v] = true;
    size_t pending = 0;
    for (size_t v = 0; v < total; v++)
        if (across[v]) values[pending++] = values[v];
    bool ok = true;
    while (ok && pending) {
        ir_value_t *value = values[--pending];
        bool added = false;
        ok = core_indexed_table_intern(spills, value, nullptr, &added)
            != nullptr;
        if (!ok || !added || !value->def || value->def->op != IR_OP_PHI)
            continue;
        const ir_instr_t *phi = value->def;
        for (uint32_t o = 0; o < phi->n_operands; o++) {
            ir_value_t *arm = phi->operands[o].kind == IR_OPD_PHI_ARM
                ? phi->operands[o].phi.value : nullptr;
            if (!arm || arm->kind != IR_VAL_INSTR
                || core_indexed_table_find(spills, arm)) continue;
            if (pending == total) {
                values = core_realloc(values, (total * 2 + 1)
                                                  * sizeof(*values));
                total = total * 2 + 1;
            }
            values[pending++] = arm;
        }
    }
    core_free(values);
    core_free(across);
    return ok;
}

static bool
mark_conservative_frame(lower_state_t *state)
{
    state->conservative_frame = true;
    state->used_values = core_indexed_table_new(0, UINT32_MAX,
        pointer_key_hash, pointer_key_equal);
    if (!state->used_values
        || !collect_conservative_spills(state->function, state->used_values))
        return false;
    for (uint32_t i = 0; i < state->function->n_params; i++) {
        ir_value_t *parameter = state->function->params[i];
        if (parameter && !spill_conservative_root(state, parameter->type,
                lowered_value(state, parameter))) return false;
    }
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
summary_has(const ir_func_t *function, uint32_t flag, bool otherwise)
{
    ir_summary_t *summary = ir_summary_of(ir_analysis_summaries(), function);
    return summary ? (ir_summary_flags(summary) & flag) != 0 : otherwise;
}

static bool
prepare_frame_map(lower_state_t *state)
{
    bool collects = state->with_mutator
        || summary_has(state->function, IR_SUMMARY_MAY_COLLECT, true);
    if (!collects) return true;
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
        pointer_key_hash, pointer_key_equal);
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
    append_function_symbol(&name, state->function);
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
            unsigned position = state->parameter_base
                + (unsigned)native_index;
            if (!formal || position >= LLVMCountParams(state->target_function))
                return false;
            LLVMValueRef native = LLVMGetParam(state->target_function,
                                               position);
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

static void
append_used_global(LLVMModuleRef module, LLVMValueRef global)
{
    LLVMContextRef context = LLVMGetModuleContext(module);
    LLVMTypeRef pointer = LLVMPointerTypeInContext(context, 0);
    LLVMValueRef existing = LLVMGetNamedGlobal(module, "llvm.used");
    unsigned count = existing
        ? LLVMGetArrayLength2(LLVMGlobalGetValueType(existing)) : 0;
    LLVMValueRef *items = core_alloc_array(LLVMValueRef, count + 1);
    LLVMValueRef initializer = existing ? LLVMGetInitializer(existing)
                                        : nullptr;
    for (unsigned i = 0; i < count; i++)
        items[i] = LLVMGetAggregateElement(initializer, i);
    items[count] = global;
    if (existing) LLVMDeleteGlobal(existing);
    LLVMValueRef value = LLVMConstArray2(pointer, items, count + 1);
    LLVMValueRef used = LLVMAddGlobal(module, LLVMTypeOf(value), "llvm.used");
    LLVMSetInitializer(used, value);
    LLVMSetLinkage(used, LLVMAppendingLinkage);
    LLVMSetSection(used, "llvm.metadata");
    core_free(items);
}

static bool
lower_function(LLVMModuleRef target, const ir_module_t *source,
               ir_func_t *function,
               bool with_mutator,
               bool emit_frame_maps,
               const core_list_t(inline_function_ptr_t) *mutator_functions,
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
               index_slots_t *index_slots,
               core_diagnostics_t *diagnostics)
{
    LLVMContextRef context = LLVMGetModuleContext(target);
    core_buffer_t symbol = {0};
    append_function_symbol(&symbol, function);
    LLVMValueRef target_function = with_mutator
        ? mutator_function(target, function)
        : named_function(target, core_buffer_string(&symbol));
    core_buffer_release(&symbol);
    size_t input_count = 0;
    size_t optional_count = 0;
    for (size_t i = 0; i < th_func_nparams(function->type); i++) {
        const th_param *formal = th_func_param(function->type, i);
        if (formal && th_mode_accepts_input(formal->mode)) input_count++;
        if (ir_func_parameter_is_optional(function, i)) optional_count++;
    }
    unsigned parameter_base = !ir_annot_get(&function->annots,
            CORE_STRING_STATIC("n00bb.abi.native"))
        && returns_through_box(function->owner, function->type, context)
        ? 1 : 0;
    size_t native_count = th_func_nparams(function->type) - optional_count
        + (optional_count != 0) + parameter_base;
    if (!target_function || !function->n_blocks
        || function->n_params != input_count
        || native_count + (with_mutator ? 1 : 0)
            != LLVMCountParams(target_function)) {
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "LLVM backend cannot lower function '%.*s': its signature or body is unavailable",
            TEXT_ARGS(function->name));
        if (diagnostics)
            core_diagnostics_report_at(diagnostics, nullptr,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-B0006"),
                core_buffer_string(&message), (core_diag_location_t){
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
        .parameter_base = parameter_base,
        .result_box = parameter_base ? LLVMGetParam(target_function, 0)
                                     : nullptr,
        .diagnostics = diagnostics,
        .index_slots = index_slots,
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
    state.analysis_scope = ir_analysis_scope_begin();
    (void)ir_abcd_fold(function);
    (void)ir_abcd_hoist(function);
    state.ranges = ir_func_ranges(function);
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
    state.value_index = core_indexed_table_new(0, UINT32_MAX,
        pointer_key_hash, pointer_key_equal);
    state.builder = LLVMCreateBuilderInContext(context);
    LLVMMetadataRef subprogram = active_debug && state.builder
        ? debug_subprogram(active_debug, target_function, function) : nullptr;
    int32_t debug_file_id = subprogram ? function_file(function) : -1;
    debug_location(state.builder, subprogram, function, debug_file_id,
                   function->span);
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
        for (uint32_t i = 0; i < function->n_blocks; i++) {
            state.blocks[i] = LLVMAppendBasicBlockInContext(context,
                target_function, "");
            set_value_name(LLVMBasicBlockAsValue(state.blocks[i]),
                           function->blocks[i]->name);
        }
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
            state.locals[i] = LLVMBuildAlloca(state.builder, type, "");
            set_value_name(state.locals[i], local->name);
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
            debug_location(state.builder, subprogram, function,
                           debug_file_id, instruction->span);
            ok = lower_instruction(&state, instruction);
            if (ok && state.conservative_frame
                && instruction->op == IR_OP_PHI
                && (j + 1 == block->n_instrs
                    || block->instrs[j + 1]->op != IR_OP_PHI))
                ok = spill_conservative_phis(&state, block, j);
            if (ok && state.frame_roots && instruction->op == IR_OP_PHI
                && (j + 1 == block->n_instrs
                    || block->instrs[j + 1]->op != IR_OP_PHI))
                ok = store_phi_frame_roots(&state, block, j);
            if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics))) {
                core_buffer_t message = {0};
                core_string_t spelling = ir_op_spelling(instruction->op);
                core_buffer_printf(&message,
                    "could not lower '%.*s' in function '%.*s'",
                    TEXT_ARGS(spelling), TEXT_ARGS(function->name));
                if (state.failure_detail.data)
                    core_buffer_printf(&message, ": %.*s",
                        TEXT_ARGS(state.failure_detail));
                backend_report(diagnostics, core_buffer_string(&message));
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
    ir_analysis_scope_end(state.analysis_scope);
    core_indexed_table_free(state.frame_roots);
    core_indexed_table_free(state.used_values);
    core_indexed_table_free(state.value_index);
    free(state.frame_slots);
    if (!ok && (!diagnostics || !core_diagnostics_failed(diagnostics))) {
        core_buffer_t message = {0};
        core_string_t module_name = ir_module_name(function->owner);
        core_buffer_printf(&message,
            "could not lower LLVM function '%.*s.%.*s'",
            TEXT_ARGS(module_name), TEXT_ARGS(function->name));
        backend_report(diagnostics, core_buffer_string(&message));
        core_buffer_release(&message);
    }
    return ok;
}

static bool
inline_error(core_diagnostics_t *diagnostics, const ir_func_t *function,
             core_string_t detail)
{
    if (!diagnostics) return false;
    core_buffer_t message = {0};
    core_string_t module_name = ir_module_name(function->owner);
    core_buffer_printf(&message, "inline function '%.*s.%.*s' %.*s",
        TEXT_ARGS(module_name), TEXT_ARGS(function->name),
        TEXT_ARGS(detail));
    core_diagnostics_report_at(diagnostics, nullptr, CORE_DIAG_FATAL,
        CORE_STRING_STATIC("N00BB-B0006"),
        core_buffer_string(&message), (core_diag_location_t){
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
                    CORE_STRING_STATIC("contains an indirect call whose recursion cannot be checked"));
            if (!callee) continue;
            if (callee == start)
                return inline_error(diagnostics, start,
                    CORE_STRING_STATIC("participates in direct or indirect recursion"));
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
                    ? n00bb_cached_body(used->value->owner) : nullptr;
                if (value && ir_func_is_always_inline(value)
                    && (instruction->op != IR_OP_CALL || operand != 0))
                    return inline_error(diagnostics, value,
                        CORE_STRING_STATIC("cannot be used as a function value"));
            }
            ir_func_t *callee = direct_call_target(instruction);
            if (!callee || !ir_func_is_always_inline(callee)
                || inline_list_contains(collected, callee)) continue;
            if (!callee->n_blocks)
                return inline_error(diagnostics, callee,
                    CORE_STRING_STATIC("has no body available at this call site"));
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

static void
add_mutator_candidate(ir_func_t *function,
                      core_list_t(inline_function_ptr_t) *mutators)
{
    if (function->n_blocks && !inline_list_contains(mutators, function)
        && summary_has(function, IR_SUMMARY_ALLOCATES_DIRECT, false))
        core_list_push(*mutators, function);
}

static void
collect_mutator_candidates(ir_func_t *function,
                           core_list_t(inline_function_ptr_t) *mutators)
{
    add_mutator_candidate(function, mutators);
    for (uint32_t b = 0; b < function->n_blocks; b++) {
        ir_block_t *block = function->blocks[b];
        for (uint32_t i = 0; i < block->n_instrs; i++) {
            ir_func_t *callee = direct_call_target(block->instrs[i]);
            if (callee) add_mutator_candidate(callee, mutators);
        }
    }
}

static bool
is_mutator_allocator(const ir_func_t *function)
{
    return n00bb_axiom_function_has(function, N00BB_AXIOM_MUTATOR_ALLOCATOR);
}

static bool
is_mutator_getter(const ir_func_t *function)
{
    return n00bb_axiom_function_has(function, N00BB_AXIOM_MUTATOR_GETTER);
}

uint32_t
n00bb_llvm_mutator_flags(const ir_func_t *function, uint32_t flags)
{
    return function->n_blocks && is_mutator_allocator(function)
        ? flags | IR_SUMMARY_ALLOCATES_DIRECT : flags;
}

static void
mark_always_inline(LLVMContextRef context, LLVMValueRef function)
{
    unsigned kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
    LLVMAttributeRef attribute = LLVMCreateEnumAttribute(context, kind, 0);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, attribute);
}

static bool
function_body_emitted(const ir_func_t *function,
                      ir_func_t *const *reachable, size_t reachable_count,
                      bool reachable_bound)
{
    if (!reachable_bound) return true;
    for (size_t j = 0; j < reachable_count; j++)
        if (reachable[j] == function) return true;
    return false;
}

static bool
lower_module(LLVMModuleRef target, const ir_module_t *source,
             bool emit_frame_maps,
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
             const n00bb_llvm_index_table_t *index_table,
             core_diagnostics_t *diagnostics)
{
    for (size_t i = 0; i < ir_global_count(source); i++) {
        ir_global_t *global = ir_global_at((ir_module_t *)source, i);
        LLVMTypeRef type = value_type((ir_module_t *)source, global->type,
                                     LLVMGetModuleContext(target));
        if (!type || LLVMGetTypeKind(type) == LLVMVoidTypeKind) {
            core_buffer_t message = {0};
            core_string_t printed = th_print(
                ir_module_types((ir_module_t *)source), global->type);
            core_buffer_printf(&message,
                "LLVM backend cannot lower global '%.*s' of type %.*s",
                TEXT_ARGS(global->name), TEXT_ARGS(printed));
            bool reported = backend_report(diagnostics,
                                           core_buffer_string(&message));
            core_buffer_release(&message);
            return reported;
        }
        bool variant = false;
        if (th_named_info(th_resolve(global->type), nullptr,
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
                    (const uint8_t *)init->string.data, init->string.u8_bytes,
                    default_string_encoding(init->string.data,
                                            init->string.u8_bytes));
            else if (init->kind == IR_GLOBAL_INIT_RECORD
                     || init->kind == IR_GLOBAL_INIT_SEQUENCE) {
                lower_state_t cache = {.module = target};
                value = static_init_value(&cache, (ir_module_t *)source,
                    global->type, init, global->mutability == IR_MUT_CONST);
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
            core_string_t module_name = ir_module_name(source);
            core_string_t printed = th_print(
                ir_module_types((ir_module_t *)source), function->type);
            core_buffer_printf(&message,
                "LLVM backend cannot lower signature for '%.*s.%.*s': %.*s",
                TEXT_ARGS(module_name), TEXT_ARGS(function->name),
                TEXT_ARGS(printed));
            backend_report(diagnostics, core_buffer_string(&message));
            core_buffer_release(&message);
            return false;
        }
        core_buffer_t symbol = {0};
        core_buffer_append_string(&symbol, ir_module_name(source));
        core_buffer_putc(&symbol, '.');
        core_buffer_append_string(&symbol, function->name);
        LLVMValueRef declared = add_function(target,
            core_buffer_string(&symbol), signature);
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
        bool emit_body = function_body_emitted(function,
            reachable, reachable_count, reachable_bound);
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
        bool emit_body = function_body_emitted(function,
            reachable, reachable_count, reachable_bound);
        if (emit_body && function->n_blocks
            && summary_has(function, IR_SUMMARY_ALLOCATES_DIRECT, false))
            needs_allocation_callee = true;
    }
    for (size_t i = 0; i < inline_callees.len; i++)
        if (summary_has(inline_callees.data[i], IR_SUMMARY_ALLOCATES_DIRECT,
                        false))
            needs_allocation_callee = true;
    if (inline_ok && needs_allocation_callee && reachable_bound) {
        for (size_t i = 0; i < reachable_count; i++) {
            ir_func_t *function = reachable[i];
            if (!is_mutator_allocator(function)
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
        append_function_symbol(&symbol, function);
        LLVMValueRef declared = named_function(target,
                                               core_buffer_string(&symbol));
        if (!declared) {
            LLVMTypeRef signature = function_type(target, function);
            declared = signature
                ? add_function(target, core_buffer_string(&symbol), signature)
                : nullptr;
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
    core_list_t(inline_function_ptr_t) mutator_functions =
        core_list_new(inline_function_ptr_t);
    for (size_t i = 0; i < ir_func_count(source); i++)
        collect_mutator_candidates(ir_func_at((ir_module_t *)source, i),
                                   &mutator_functions);
    for (size_t i = 0; i < inline_callees.len; i++)
        collect_mutator_candidates(inline_callees.data[i], &mutator_functions);
    for (size_t i = 0; i < mutator_functions.len; i++) {
        if (!is_mutator_getter(mutator_functions.data[i])) continue;
        core_list_free(mutator_functions);
        core_list_free(inline_callees);
        return backend_error(diagnostics,
            "the native mutator getter reaches code that needs a mutator");
    }
    for (size_t i = 0; i < mutator_functions.len; i++) {
        ir_func_t *function = mutator_functions.data[i];
        LLVMTypeRef signature = mutator_function_type(target, function);
        core_buffer_t name = {0};
        mutator_symbol(&name, function);
        LLVMValueRef declared = signature
            ? named_function(target, core_buffer_string(&name)) : nullptr;
        if (!declared && signature)
            declared = add_function(target, core_buffer_string(&name),
                                    signature);
        core_buffer_release(&name);
        if (declared) {
            core_buffer_t source_name = {0};
            append_function_symbol(&source_name, function);
            LLVMAddAttributeAtIndex(declared, LLVMAttributeFunctionIndex,
                LLVMCreateStringAttribute(LLVMGetModuleContext(target),
                    code_name_attribute,
                    (unsigned)(sizeof(code_name_attribute) - 1),
                    source_name.data, (unsigned)source_name.byte_len));
            core_buffer_release(&source_name);
        }
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
    index_slots_t index_slots = {.binding = index_table};
    for (size_t i = 0; i < ir_func_count(source); i++) {
        ir_func_t *function = ir_func_at((ir_module_t *)source, i);
        bool emit_body = function_body_emitted(function,
            reachable, reachable_count, reachable_bound);
        bool selected = inline_list_contains(&mutator_functions, function);
        if (function->n_blocks && emit_body
            && ((selected && !lower_mutator_wrapper(target, function,
                                                     diagnostics))
                || !lower_function(target, source, function, selected,
                               emit_frame_maps,
                               &mutator_functions,
                               type_indices,
                               type_index_count, function_indices,
                               function_index_count, global_bindings,
                               global_binding_count, storage_origin,
                               callback_bindings,
                               &layout_matches,
                               &member_matches,
                               &index_slots,
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
                            &mutator_functions,
                            type_indices,
                            type_index_count, function_indices,
                            function_index_count, global_bindings,
                            global_binding_count, storage_origin,
                            callback_bindings,
                            &layout_matches, &member_matches,
                            &index_slots,
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
    core_string_t module_name = ir_module_name(source);
    size_t cursor = 0;
    const n00bb_runtime_native_root_t *root;
    while ((root = n00bb_native_root_in_module(module_name, &cursor))) {
        LLVMValueRef function = LLVMGetNamedFunction(target,
                                                     root->qualified_name);
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

static bool native_target_ready(void);

static bool
set_native_layout(LLVMModuleRef module, core_string_t cpu,
                  core_string_t features)
{
    if (!native_target_ready()) return false;
    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = nullptr;
    char *message = nullptr;
    bool ok = triple && !LLVMGetTargetFromTriple(triple, &target, &message);
    char *cpu_text = ok ? llvm_text(cpu) : nullptr;
    char *feature_text = ok ? llvm_text(features) : nullptr;
    LLVMTargetMachineRef machine = ok ? LLVMCreateTargetMachine(target,
        triple, cpu_text, feature_text, LLVMCodeGenLevelDefault,
        LLVMRelocDefault, LLVMCodeModelDefault) : nullptr;
    core_free(cpu_text);
    core_free(feature_text);
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
n00bb_llvm_backend_prepare(const ir_module_t *ir, bool debug_enabled,
                           bool native_cpu, core_diagnostics_t *diagnostics)
{
    if (!ir || !ir_module_name(ir).data || !ir_module_target(ir).data) {
        backend_error(diagnostics, "LLVM backend needs a targeted module");
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
        backend->cpu_name = take_llvm_message(LLVMGetHostCPUName());
        backend->cpu_features = take_llvm_message(LLVMGetHostCPUFeatures());
        if (!backend->cpu_name.data || !backend->cpu_name.u8_bytes
            || !backend->cpu_features.data) goto fail;
    }
    backend->source = ir;
    backend->context = LLVMContextCreate();
    if (!backend->context) goto fail;
    backend->module = LLVMModuleCreateWithNameInContext("", backend->context);
    if (!backend->module) goto fail;
    core_string_t module_name = ir_module_name(ir);
    if (module_name.data) {
        LLVMSetModuleIdentifier(backend->module, module_name.data,
                                module_name.u8_bytes);
        LLVMSetSourceFileName(backend->module, module_name.data,
                              module_name.u8_bytes);
    }
    char *target_name = llvm_text(ir_module_target(ir));
    LLVMSetTarget(backend->module, target_name);
    core_free(target_name);
    if (!set_native_layout(backend->module,
            native_cpu ? backend->cpu_name : CORE_STRING_STATIC("generic"),
            native_cpu ? backend->cpu_features : CORE_STRING_STATIC("")))
        goto fail;
    return backend;

fail:
    n00bb_llvm_backend_free(backend);
    if (!diagnostics || !core_diagnostics_failed(diagnostics))
        backend_error(diagnostics, "could not initialize the LLVM module");
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
    if (backend->module) LLVMDisposeModule(backend->module);
    if (backend->context) LLVMContextDispose(backend->context);
    core_free(backend->cpu_name.data);
    core_free(backend->cpu_features.data);
    core_free(backend);
}

bool
n00bb_llvm_backend_bind_type_indices(
    n00bb_llvm_backend_t *backend, const n00bb_llvm_type_index_t *types,
    size_t count)
{
    if (!backend || backend->lowered || (count && !types))
        return false;
    backend->type_indices = types;
    backend->type_index_count = count;
    return true;
}

bool
n00bb_llvm_backend_bind_index_table(n00bb_llvm_backend_t *backend,
                                    const n00bb_llvm_index_table_t *table)
{
    if (!backend || backend->lowered || !table || !table->symbol.data)
        return false;
    backend->index_table = table;
    return true;
}

bool
n00bb_llvm_backend_bind_function_indices(
    n00bb_llvm_backend_t *backend,
    const n00bb_llvm_function_binding_t *functions, size_t count)
{
    if (!backend || backend->lowered || (count && !functions))
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
    if (!backend || (count && !globals) || backend->lowered)
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
    if (!backend || (count && !finalizers) || backend->lowered)
        return false;
    backend->finalizers = finalizers;
    backend->finalizer_count = count;
    return true;
}

struct n00bb_llvm_callback_bindings_t {
    core_list_t(callback_binding_t) list;
};

void
n00bb_llvm_callback_bindings_free(n00bb_llvm_callback_bindings_t *bindings)
{
    if (!bindings) return;
    core_list_free(bindings->list);
    core_free(bindings);
}

bool
n00bb_llvm_backend_bind_reachable_functions(
    n00bb_llvm_backend_t *backend, ir_func_t *const *functions, size_t count,
    const n00bb_llvm_callback_bindings_t *bindings)
{
    if (!backend || (count && !functions) || !bindings || backend->lowered)
        return false;
    backend->reachable = functions;
    backend->reachable_count = count;
    backend->reachable_bound = true;
    backend->callback_bindings = bindings;
    return true;
}

n00bb_llvm_callback_bindings_t *
n00bb_llvm_callback_bindings_new(ir_func_t *const *functions, size_t count)
{
    if (count && !functions) return nullptr;
    n00bb_llvm_callback_bindings_t *bindings =
        core_alloc(n00bb_llvm_callback_bindings_t);
    bindings->list = core_list_new(callback_binding_t);
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
                core_list_push(bindings->list,
                    ((callback_binding_t){handler->owner,
                        creation->dest->type, signature}));
            }
        }
    }
    return bindings;
}

bool
n00bb_llvm_backend_bind_storage_origin(
    n00bb_llvm_backend_t *backend, ir_storage_origin_t *origin)
{
    if (!backend || backend->lowered) return false;
    backend->storage_origin = origin;
    return true;
}

bool
n00bb_llvm_backend_enable_frame_maps(n00bb_llvm_backend_t *backend)
{
    if (!backend || backend->lowered) return false;
    backend->emit_frame_maps = true;
    return true;
}

void
n00bb_llvm_backend_bind_native_cache(n00bb_llvm_backend_t *backend,
    n00bb_native_cache_t *cache)
{
    if (backend) backend->native_cache = cache;
}

void
n00bb_llvm_backend_bind_summaries(n00bb_llvm_backend_t *backend,
                                  ir_summaries_t *summaries)
{
    if (backend) backend->summaries = summaries;
}

static bool
lower_backend(n00bb_llvm_backend_t *backend, core_diagnostics_t *diagnostics)
{
    if (backend->lowered) return true;
    core_dict_t *previous_native_types = active_native_types;
    active_native_types = &backend->native_types;
    debug_info_t debug = {0};
    bool with_debug = backend->debug_enabled;
    if (with_debug && !debug_info_begin(&debug, backend->module,
                                        backend->source)) {
        debug_info_end(&debug);
        active_native_types = previous_native_types;
        return backend_error(diagnostics,
            "LLVM backend could not create debug information");
    }
    debug_info_t *previous_debug = active_debug;
    active_debug = with_debug ? &debug : nullptr;
    ir_summaries_t *previous_summaries = backend->summaries
        ? ir_analysis_bind_summaries(backend->summaries)
        : ir_analysis_summaries();
    bool module_lowered = lower_module(backend->module, backend->source,
        backend->emit_frame_maps, backend->type_indices,
        backend->type_index_count, backend->function_indices,
        backend->function_index_count, backend->global_bindings,
        backend->global_binding_count, backend->storage_origin,
        backend->reachable, backend->reachable_count,
        backend->reachable_bound,
        backend->callback_bindings ? &backend->callback_bindings->list
                                   : nullptr,
        backend->index_table, diagnostics);
    ir_analysis_bind_summaries(previous_summaries);
    active_debug = previous_debug;
    if (with_debug) debug_info_end(&debug);
    active_native_types = previous_native_types;
    if (!module_lowered) return false;
    LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
    LLVMErrorRef error = LLVMRunPasses(backend->module, "always-inline",
                                       nullptr, options);
    LLVMDisposePassBuilderOptions(options);
    if (error) {
        char *message = LLVMGetErrorMessage(error);
        backend_llvm_report(diagnostics, message,
            CORE_STRING_STATIC("LLVM always-inline pass failed"));
        LLVMDisposeErrorMessage(message);
        return false;
    }

    backend->lowered = true;
    return true;
}

bool
n00bb_llvm_backend_write_bitcode(n00bb_llvm_backend_t *backend,
                                  core_buffer_t *out,
                                  core_diagnostics_t *diagnostics)
{
    if (!backend || !out)
        return backend_error(diagnostics, "invalid LLVM bitcode probe arguments");
    if (!lower_backend(backend, diagnostics)) return false;
    LLVMModuleRef module = backend->module;
    char *error = nullptr;
    if (LLVMVerifyModule(module, LLVMReturnStatusAction, &error)) {
        backend_llvm_report(diagnostics, error,
            CORE_STRING_STATIC("LLVM module verification failed"));
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
    LLVMModuleRef module = backend->module;
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
        append_function_symbol(&name, function);
        LLVMValueRef target = named_function(module,
                                             core_buffer_string(&name));
        if (!target) target = add_function(module, core_buffer_string(&name),
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
    LLVMModuleRef module = backend->module;
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

static void
keep_entry_frames(LLVMModuleRef module)
{
    LLVMValueRef entry = LLVMGetNamedFunction(module, "main");
    if (!entry) return;
    unsigned kind = LLVMGetEnumAttributeKindForName("noinline",
                                                    sizeof("noinline") - 1);
    LLVMContextRef context = LLVMGetModuleContext(module);
    for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(entry); block;
         block = LLVMGetNextBasicBlock(block))
        for (LLVMValueRef instruction = LLVMGetFirstInstruction(block);
             instruction; instruction = LLVMGetNextInstruction(instruction)) {
            LLVMValueRef callee = LLVMIsACallInst(instruction)
                ? LLVMGetCalledValue(instruction) : nullptr;
            if (!callee || !LLVMIsAFunction(callee)
                || LLVMGetIntrinsicID(callee)) continue;
            LLVMAddAttributeAtIndex(callee, LLVMAttributeFunctionIndex,
                LLVMCreateEnumAttribute(context, kind, 0));
        }
}

static bool
emit_entry_initializers(LLVMModuleRef module, LLVMContextRef context,
                        LLVMBuilderRef builder, const core_string_t *names,
                        size_t count, core_diagnostics_t *diagnostics)
{
    LLVMTypeRef initializer_type = LLVMFunctionType(
        LLVMVoidTypeInContext(context), nullptr, 0, false);
    for (size_t i = 0; i < count; i++) {
        LLVMValueRef initializer = named_function(module, names[i]);
        if (!initializer)
            initializer = add_function(module, names[i], initializer_type);
        if (LLVMCountParams(initializer)
            || LLVMGetReturnType(LLVMGlobalGetValueType(initializer))
                != LLVMVoidTypeInContext(context))
            return backend_error(diagnostics,
                "module initializer signature is invalid");
        LLVMBuildCall2(builder, LLVMGlobalGetValueType(initializer),
                       initializer, nullptr, 0, "");
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
    core_string_t failed_step = CORE_STRING_STATIC("unknown step");
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
            if (!ok) failed_step =
                CORE_STRING_STATIC("discarding conservative roots");
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
            if (!ok) failed_step =
                CORE_STRING_STATIC("discarding transformed roots");
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
            failed_step = CORE_STRING_STATIC("finding entry root slots");
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
            failed_step = CORE_STRING_STATIC("initializing entry root slots");
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
        LLVMValueRef descriptor = add_global(module,
            LLVMInt8TypeInContext(context), core_buffer_string(&symbol));
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
        "could not emit native frame sentinels in %.*s (%.*s)",
        failed_function ? (int)failed_function_length
                        : (int)sizeof("unknown function") - 1,
        failed_function ? failed_function : "unknown function",
        TEXT_ARGS(failed_step));
    bool reported = backend_report(diagnostics, core_buffer_string(&message));
    core_buffer_release(&message);
    return reported;
}

static LLVMValueRef
runtime_function_address_table(n00bb_llvm_backend_t *backend,
                               uint32_t *count_out)
{
    LLVMModuleRef module = backend->module;
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
        ir_func_t *body = n00bb_cached_body(function);
        if (body == function
            ? !function_body_emitted(function, backend->reachable,
                  backend->reachable_count, backend->reachable_bound)
            : !th_is_closed(ir_module_types(body->owner), body->type))
            continue;
        LLVMTypeRef signature = function_type(module, body);
        core_buffer_t name = {0};
        append_function_symbol(&name, function);
        LLVMValueRef target = signature && name.data
            ? named_function(module, core_buffer_string(&name)) : nullptr;
        if (!target && signature && name.data)
            target = add_function(module, core_buffer_string(&name),
                                  signature);
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
    LLVMModuleRef module, core_buffer_t *out,
    core_diagnostics_t *diagnostics);

static pthread_once_t native_target_once = PTHREAD_ONCE_INIT;
static bool native_target_available;

static void
initialize_native_target(void)
{
    native_target_available = !LLVMInitializeNativeTarget()
        && !LLVMInitializeNativeAsmPrinter()
        && !LLVMInitializeNativeAsmParser();
}

static bool
native_target_ready(void)
{
    pthread_once(&native_target_once, initialize_native_target);
    return native_target_available;
}

static bool
prepare_image(n00bb_llvm_backend_t *backend,
              const core_string_t *entry_initializers,
              size_t initializer_count, size_t metadata_bytes,
              core_diagnostics_t *diagnostics)
{
    if (!backend || (initializer_count && !entry_initializers)
        || (initializer_count && !metadata_bytes))
        return backend_error(diagnostics, "invalid LLVM object arguments");
    if (!native_target_ready())
        return backend_error(diagnostics, "LLVM native target is unavailable");
    if (!lower_backend(backend, diagnostics)) return false;
    LLVMModuleRef module = backend->module;
    LLVMContextRef context = backend->context;
    uint32_t function_address_count = 0;
    LLVMValueRef function_addresses = initializer_count
        ? runtime_function_address_table(backend, &function_address_count)
        : nullptr;
    if (initializer_count && !function_addresses)
        return backend_error(diagnostics,
            "could not emit indexed runtime function addresses");
    if (initializer_count
        && !emit_tls_address_resolver(backend, diagnostics)) return false;
    if (initializer_count
        && !emit_finalizer_bridge(backend, diagnostics)) return false;
    if (initializer_count) {
        LLVMTypeRef result = LLVMInt32TypeInContext(context);
        LLVMTypeRef entry_params[] = {
            result, LLVMPointerTypeInContext(context, 0)
        };
        LLVMTypeRef signature = LLVMFunctionType(result, entry_params, 2,
                                                 false);
        LLVMValueRef entry = LLVMAddFunction(module, "main", signature);
        LLVMBasicBlockRef block = LLVMAppendBasicBlockInContext(context, entry,
            "entry");
        LLVMBuilderRef builder = LLVMCreateBuilderInContext(context);
        LLVMPositionBuilderAtEnd(builder, block);
        LLVMTypeRef save_args_type = LLVMFunctionType(
            LLVMVoidTypeInContext(context), entry_params, 2, false);
        LLVMValueRef save_args = LLVMGetNamedFunction(module,
                                                      "n00b_os_save_args");
        if (!save_args)
            save_args = LLVMAddFunction(module, "n00b_os_save_args",
                                        save_args_type);
        LLVMValueRef saved_args[] = {
            LLVMGetParam(entry, 0), LLVMGetParam(entry, 1)
        };
        LLVMBuildCall2(builder, save_args_type, save_args, saved_args, 2, "");
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
        if (!emit_entry_initializers(module, context, builder,
                entry_initializers, initializer_count, diagnostics)) {
            LLVMDisposeBuilder(builder);
            return false;
        }
        LLVMBuildRet(builder, LLVMConstInt(result, 0, false));
        LLVMDisposeBuilder(builder);
        keep_entry_frames(module);
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
        core_buffer_t detail = {0};
        core_string_t verifier = core_string_from_cstr(message);
        core_buffer_append_string(&detail, verifier.data ? verifier
            : CORE_STRING_STATIC("invalid LLVM module"));
        core_free(verifier.data);
        for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
             function = LLVMGetNextFunction(function))
            if (LLVMCountBasicBlocks(function)
                && LLVMVerifyFunction(function, LLVMReturnStatusAction)) {
                size_t length = 0;
                const char *name = LLVMGetValueName2(function, &length);
                core_buffer_printf(&detail, "in function %.*s\n",
                                   (int)length, name);
            }
        backend_report(diagnostics, core_buffer_string(&detail));
        core_buffer_release(&detail);
        if (message) LLVMDisposeMessage(message);
        return false;
    }
    if (message) LLVMDisposeMessage(message);
    backend->prepared = true;
    return true;
}

static bool
emit_image(n00bb_llvm_backend_t *backend,
           const core_string_t *entry_initializers, size_t initializer_count,
           size_t metadata_bytes, bool bitcode, core_buffer_t *out,
           core_diagnostics_t *diagnostics)
{
    if (!out) return backend_error(diagnostics,
                                   "invalid LLVM object arguments");
    if (!prepare_image(backend, entry_initializers, initializer_count,
                       metadata_bytes, diagnostics)) return false;
    LLVMModuleRef module = backend->module;
    if (bitcode) {
        LLVMMemoryBufferRef serialized = LLVMWriteBitcodeToMemoryBuffer(module);
        if (!serialized)
            return backend_error(diagnostics, "could not serialize linkable LLVM bitcode");
        core_buffer_append(out, LLVMGetBufferStart(serialized),
                           LLVMGetBufferSize(serialized));
        LLVMDisposeMemoryBuffer(serialized);
        return true;
    }
    return emit_native_module(backend, module, out, diagnostics);
}

static bool
emit_code_map(LLVMModuleRef module, core_diagnostics_t *diagnostics)
{
    core_string_t triple = core_string_from_cstr(LLVMGetTarget(module));
    bool mach_o = triple.data
        && (core_string_find(triple, CORE_STRING_STATIC("apple")) >= 0
            || core_string_find(triple, CORE_STRING_STATIC("darwin")) >= 0);
    bool elf = triple.data
        && (core_string_find(triple, CORE_STRING_STATIC("linux")) >= 0
            || core_string_find(triple, CORE_STRING_STATIC("elf")) >= 0
            || core_string_find(triple, CORE_STRING_STATIC("bsd")) >= 0);
    core_free(triple.data);
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
        LLVMAttributeRef code_name = LLVMGetStringAttributeAtIndex(function,
            LLVMAttributeFunctionIndex, code_name_attribute,
            (unsigned)(sizeof(code_name_attribute) - 1));
        if (code_name) {
            unsigned code_name_length = 0;
            name = LLVMGetStringAttributeValue(code_name, &code_name_length);
            length = code_name_length;
        }
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
        append_used_global(module, table);
        char *message = nullptr;
        ok = !LLVMVerifyModule(module, LLVMReturnStatusAction, &message);
        if (!ok) backend_llvm_report(diagnostics, message,
            CORE_STRING_STATIC("invalid LLVM code map"));
        if (message) LLVMDisposeMessage(message);
    }
    core_list_free(rows);
    return ok;
}

static double
profile_now(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static bool
emit_native_module(n00bb_llvm_backend_t *backend, LLVMModuleRef module,
                   core_buffer_t *out, core_diagnostics_t *diagnostics)
{
    double profile_started = getenv("N00BB_PROFILE_COMPILE")
        ? profile_now() : -1;
    char *message = nullptr;    if (!native_target_ready())
        return backend_error(diagnostics, "LLVM native target is unavailable");
    char *triple = LLVMGetDefaultTargetTriple();
    core_uint128_t cache_key = 0;
    if (backend->native_cache && triple) {
        LLVMMemoryBufferRef preopt = LLVMWriteBitcodeToMemoryBuffer(module);
        if (preopt) {
            core_string_t triple_text = core_string_from_cstr(triple);
            cache_key = n00bb_native_cache_key(backend->native_cache,
                LLVMGetBufferStart(preopt), LLVMGetBufferSize(preopt),
                triple_text, backend->cpu_name, backend->cpu_features,
                backend->debug_enabled, backend->emit_frame_maps);
            core_free(triple_text.data);
            LLVMDisposeMemoryBuffer(preopt);
            if (n00bb_native_cache_read(backend->native_cache, cache_key,
                    out)) {
                if (getenv("N00BB_PROFILE_COMPILE")) {
                    core_string_t module_name =
                        ir_module_name(backend->source);
                    fprintf(stderr, "PROFILE llvm-cache-hit %.*s\n",
                        TEXT_ARGS(module_name));
                }
                LLVMDisposeMessage(triple);
                return true;
            }
        }
    }
    LLVMTargetRef target = nullptr;
    bool ok = triple && !LLVMGetTargetFromTriple(triple, &target, &message);
    if (!ok) {
        backend_llvm_report(diagnostics, message,
            CORE_STRING_STATIC("LLVM target is unavailable"));
        if (message) LLVMDisposeMessage(message);
        if (triple) LLVMDisposeMessage(triple);
        return false;
    }
    if (message) LLVMDisposeMessage(message);
    message = nullptr;
    char *cpu_text = llvm_text(backend->cpu_name.data ? backend->cpu_name
                                   : CORE_STRING_STATIC("generic"));
    char *feature_text = llvm_text(backend->cpu_features);
    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(target, triple,
        cpu_text, feature_text,
        backend->debug_enabled ? LLVMCodeGenLevelDefault
                               : LLVMCodeGenLevelAggressive,
        LLVMRelocPIC,
        LLVMCodeModelDefault);
    core_free(cpu_text);
    core_free(feature_text);
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
        backend_llvm_report(diagnostics, pass_message,
            CORE_STRING_STATIC("LLVM optimization failed"));
        LLVMDisposeErrorMessage(pass_message);
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (backend->emit_frame_maps
        && !emit_frame_sentinels(module, diagnostics)) {
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (!emit_code_map(module, diagnostics)) {
        LLVMDisposeTargetMachine(machine);
        return false;
    }
    if (backend->emit_frame_maps
        && LLVMVerifyModule(module, LLVMReturnStatusAction, &message)) {
        backend_llvm_report(diagnostics, message,
            CORE_STRING_STATIC("invalid LLVM frame map module"));
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
    else backend_llvm_report(diagnostics, message,
        CORE_STRING_STATIC("LLVM object emission failed"));
    if (ok && profile_started >= 0) {
        size_t functions = 0, instructions = 0, largest = 0;
        LLVMValueRef largest_function = nullptr;
        for (LLVMValueRef function = LLVMGetFirstFunction(module); function;
             function = LLVMGetNextFunction(function)) {
            if (LLVMIsDeclaration(function)) continue;
            functions++;
            size_t count = 0;
            for (LLVMBasicBlockRef block = LLVMGetFirstBasicBlock(function);
                 block; block = LLVMGetNextBasicBlock(block))
                for (LLVMValueRef instruction =
                         LLVMGetFirstInstruction(block);
                     instruction;
                     instruction = LLVMGetNextInstruction(instruction))
                    count++;
            instructions += count;
            if (count > largest) {
                largest = count;
                largest_function = function;
            }
        }
        size_t name_length = 0;
        const char *largest_name = largest_function
            ? LLVMGetValueName2(largest_function, &name_length) : "";
        core_string_t module_name = ir_module_name(backend->source);
        fprintf(stderr, "PROFILE llvm-compile %.3f %.*s functions=%zu "
            "instructions=%zu object=%zu largest=%zu %.*s\n",
            profile_now() - profile_started, TEXT_ARGS(module_name),
            functions, instructions,
            object ? (size_t)LLVMGetBufferSize(object) : 0, largest,
            (int)name_length, largest_name);
    }
    if (object) LLVMDisposeMemoryBuffer(object);
    if (message) LLVMDisposeMessage(message);
    LLVMDisposeTargetMachine(machine);
    return ok;
}

bool
n00bb_llvm_backend_emit_object(n00bb_llvm_backend_t *backend,
                               const core_string_t *entry_initializers,
                               size_t initializer_count,
                               size_t metadata_bytes,
                               core_buffer_t *out,
                               core_diagnostics_t *diagnostics)
{
    return emit_image(backend, entry_initializers, initializer_count,
        metadata_bytes, false, out, diagnostics);
}

bool
n00bb_llvm_backend_prepare_object(n00bb_llvm_backend_t *backend,
                                  const core_string_t *entry_initializers,
                                  size_t initializer_count,
                                  size_t metadata_bytes,
                                  core_diagnostics_t *diagnostics)
{
    return prepare_image(backend, entry_initializers, initializer_count,
                         metadata_bytes, diagnostics);
}

bool
n00bb_llvm_backend_finish_object(n00bb_llvm_backend_t *backend,
                                 core_buffer_t *out,
                                 core_diagnostics_t *diagnostics)
{
    if (!backend || !backend->prepared || !out)
        return backend_error(diagnostics, "invalid LLVM object arguments");
    return emit_native_module(backend, backend->module, out, diagnostics);
}

bool
n00bb_llvm_backend_emit_linkable_bitcode(n00bb_llvm_backend_t *backend,
    const core_string_t *entry_initializers, size_t initializer_count,
    size_t metadata_bytes, core_buffer_t *out,
    core_diagnostics_t *diagnostics)
{
    return emit_image(backend, entry_initializers, initializer_count,
        metadata_bytes, true, out, diagnostics);
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
        core_string_t linked_layout = linked
            ? core_string_from_cstr(LLVMGetDataLayoutStr(linked))
            : (core_string_t){0};
        core_string_t next_layout = linked
            ? core_string_from_cstr(LLVMGetDataLayoutStr(next))
            : (core_string_t){0};
        bool layouts_differ = linked
            && !core_string_eq(linked_layout, next_layout);
        core_free(linked_layout.data);
        core_free(next_layout.data);
        if (layouts_differ) {
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
        keep_entry_frames(linked);
        char *message = nullptr;
        if (LLVMVerifyModule(linked, LLVMReturnStatusAction, &message))
            ok = backend_llvm_report(diagnostics, message,
                CORE_STRING_STATIC("invalid linked LTO module"));
        if (message) LLVMDisposeMessage(message);
    }
    if (ok) {
        LLVMPassBuilderOptionsRef options = LLVMCreatePassBuilderOptions();
        LLVMErrorRef error = LLVMRunPasses(linked,
            "function(simplifycfg),always-inline", nullptr, options);
        LLVMDisposePassBuilderOptions(options);
        if (error) {
            char *message = LLVMGetErrorMessage(error);
            ok = backend_llvm_report(diagnostics, message, CORE_STRING_STATIC(
                "could not simplify and inline linked LLVM functions"));
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
                        ok = backend_report(diagnostics,
                                            core_buffer_string(&message));
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
            backend.cpu_name = take_llvm_message(LLVMGetHostCPUName());
            backend.cpu_features = take_llvm_message(LLVMGetHostCPUFeatures());
            ok = backend.cpu_name.data && backend.cpu_features.data;
        }
        if (ok) ok = emit_native_module(&backend, linked, out,
                                        diagnostics);
        else backend_error(diagnostics, "could not determine LTO target CPU");
        core_free(backend.cpu_name.data);
        core_free(backend.cpu_features.data);
    }
    if (linked) LLVMDisposeModule(linked);
    LLVMContextDispose(context);
    return ok;
}

