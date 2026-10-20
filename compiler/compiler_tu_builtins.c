#include "compiler_tu.h"
#include "enum_value.h"
#include "ir/summary.h"
#include "ir/typelang.h"

bool
n00bb_builtin_reference_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && symbol->scope && !symbol->scope->parent && symbol->scope->ns
        && core_string_eq(symbol->scope->ns->ns_name,
                          CORE_STRING_STATIC("builtin"))
        && (core_string_eq(symbol->name, CORE_STRING_STATIC("ref_read"))
            || core_string_eq(symbol->name,
                              CORE_STRING_STATIC("ref_write")));
}

typedef struct {
    core_string_t name;
    core_string_t native;
    uint32_t      effects;
    uint8_t       reads;
    uint8_t       writes;
} builtin_native_name_t;

#define NATIVE(name, native, effects, reads, writes)                       \
    {CORE_STRING_STATIC(name), CORE_STRING_STATIC(native), effects, reads, \
     writes}
#define NATIVE_IO (IR_SUMMARY_MAY_IO)
#define NATIVE_BLOCKING_IO (IR_SUMMARY_MAY_IO | IR_SUMMARY_MAY_BLOCK)
#define NATIVE_CALLS (IR_SUMMARY_CALLS_UNKNOWN | IR_SUMMARY_MAY_TRAP       \
                      | IR_SUMMARY_MAY_ALLOCATE | IR_SUMMARY_MAY_BLOCK     \
                      | IR_SUMMARY_MAY_IO)

static const builtin_native_name_t builtin_native_names[] = {
    NATIVE("__bitptr", "n00bb_bitptr", 0, 0, 0),
    NATIVE("__os_page_size", "n00b_os_page_size", 0, 0, 0),
    NATIVE("__gc_enter_blocking",
           "sys.internal.gc.alloc.enter_blocking_runtime", 0, 0, 0),
    NATIVE("__gc_leave_blocking",
           "sys.internal.gc.alloc.leave_blocking_runtime", 0, 0, 0),
    NATIVE("__os_env_count", "n00b_os_env_count", 0, 0, 0),
    NATIVE("__os_env_at", "n00b_os_env_at", 0, 0, 0),
    NATIVE("__os_arg_count", "n00b_os_arg_count", 0, 0, 0),
    NATIVE("__os_arg_at", "n00b_os_arg_at", 0, 0, 0),
    NATIVE("__os_fd_is_tty", "n00b_os_fd_is_tty", NATIVE_IO, 0, 0),
    NATIVE("__os_terminal_size", "n00b_os_terminal_size", NATIVE_IO, 0,
           0x6),
    NATIVE("__os_pipe", "n00b_os_pipe", NATIVE_IO, 0, 0x3),
    NATIVE("__os_open", "n00b_os_open", NATIVE_BLOCKING_IO, 0x1, 0),
    NATIVE("__os_read", "n00b_os_read", NATIVE_BLOCKING_IO, 0, 0x2),
    NATIVE("__os_close", "n00b_os_close", NATIVE_IO, 0, 0),
    NATIVE("__os_exit", "n00b_os_exit", NATIVE_IO | IR_SUMMARY_DIVERGES, 0,
           0),
    NATIVE("__os_futex_wait", "n00b_os_futex_wait", IR_SUMMARY_MAY_BLOCK,
           0x1, 0),
    NATIVE("__os_futex_wake_all", "n00b_os_futex_wake_all", 0, 0, 0),
    NATIVE("__os_seek", "n00b_os_seek", NATIVE_IO, 0, 0),
    NATIVE("__write_bytes", "n00bb_runtime_write_bytes", NATIVE_BLOCKING_IO,
           0x2, 0),
    NATIVE("__os_thread_yield", "n00b_os_thread_yield", 0, 0, 0),
    NATIVE("__os_thread_fence", "n00b_os_thread_fence", 0, 0, 0),
    NATIVE("__os_thread_id", "n00b_os_thread_id", 0, 0, 0),
    NATIVE("__os_thread_is_main", "n00b_os_thread_is_main", 0, 0, 0),
    NATIVE("__os_thread_start", "n00b_os_thread_start", NATIVE_CALLS, 0, 0),
    NATIVE("__os_thread_stack_high_self", "n00b_os_thread_stack_high_self",
           0, 0, 0),
    NATIVE("__os_stack_red_zone_bytes", "n00b_os_stack_red_zone_bytes", 0,
           0, 0),
    NATIVE("__os_thread_control_self", "n00b_os_thread_control_self", 0, 0,
           0x1),
    NATIVE("__os_thread_capture_self", "n00b_os_thread_capture_self", 0, 0,
           0x1),
    NATIVE("__os_thread_suspend", "n00b_os_thread_suspend",
           IR_SUMMARY_MAY_BLOCK, 0, 0x2),
    NATIVE("__os_thread_resume", "n00b_os_thread_resume", 0, 0x2, 0),
    NATIVE("__os_now_ns", "n00b_os_now_ns", 0, 0, 0),
    NATIVE("__os_cpu_relax", "n00b_os_cpu_relax", 0, 0, 0),
    NATIVE("__panic_trace", "n00bb_runtime_panic_trace", NATIVE_IO, 0, 0),
    NATIVE("__os_map_zeroed", "n00b_os_map_zeroed", IR_SUMMARY_MAY_ALLOCATE,
           0, 0),
    NATIVE("__os_unmap", "n00b_os_unmap", 0, 0, 0x1),
    NATIVE("__os_protect", "n00b_os_protect", 0, 0, 0x1),
    NATIVE("__abi_type", "n00b_builtin_abi_type", 0, 0, 0),
    NATIVE("__abi_field", "n00b_builtin_abi_field", 0, 0, 0),
    NATIVE("__abi_trace", "n00b_builtin_abi_trace", 0, 0, 0),
    NATIVE("__abi_trace_child", "n00b_builtin_abi_trace_child", 0, 0, 0),
    NATIVE("__abi_trace_arm", "n00b_builtin_abi_trace_arm", 0, 0, 0),
    NATIVE("__abi_tag_extractor", "n00b_builtin_abi_tag_extractor", 0, 0, 0),
    NATIVE("__abi_layout", "n00b_builtin_abi_layout", 0, 0, 0),
    NATIVE("__abi_frame_layout", "n00b_builtin_abi_frame_layout", 0, 0, 0),
    NATIVE("__abi_storage", "n00b_builtin_abi_storage", 0, 0, 0),
    NATIVE("__abi_symbol", "n00b_builtin_abi_symbol", 0, 0, 0),
    NATIVE("__abi_module", "n00b_builtin_abi_module", 0, 0, 0),
    NATIVE("__abi_baked_node", "n00b_builtin_abi_baked_node", 0, 0, 0),
    NATIVE("__abi_function", "n00b_builtin_abi_function", 0, 0, 0),
    NATIVE("__abi_type_for", "n00b_builtin_abi_type_for", 0, 0, 0),
    NATIVE("__abi_baked_address", "n00b_abi_baked_address", 0, 0, 0),
    NATIVE("__native_symbol_name", "n00b_builtin_native_symbol_name", 0, 0,
           0),
    NATIVE("__abi_native_frame_at", "n00b_builtin_abi_native_frame_at", 0, 0,
           0),
    NATIVE("__abi_native_frame_root_offset",
           "n00b_builtin_abi_native_frame_root_offset", 0, 0, 0),
    NATIVE("__abi_storage_count", "n00b_builtin_abi_storage_count", 0, 0, 0),
    NATIVE("__abi_storage_address", "n00b_builtin_abi_storage_address", 0, 0,
           0),
    NATIVE("__abi_baked_node_count", "n00b_builtin_abi_baked_node_count", 0,
           0, 0),
    NATIVE("__abi_baked_node_address", "n00b_builtin_abi_baked_node_address",
           0, 0, 0),
    NATIVE("__abi_string_backing_at", "n00b_builtin_abi_string_backing_at", 0,
           0x1, 0),
    NATIVE("__abi_range_index", "n00b_builtin_abi_range_index", 0, 0, 0),
    NATIVE("__abi_bytes", "n00b_builtin_abi_bytes", 0, 0, 0),
    NATIVE("__abi_ref_address", "n00b_builtin_abi_ref_address", 0, 0, 0),
    NATIVE("__load_le_u64", "n00bb_load_le_u64", 0, 0x1, 0),
    NATIVE("__load_le_u32", "n00bb_load_le_u32", 0, 0x1, 0),
    NATIVE("__gc_invoke_finalizer", "n00b_gc_finalize", NATIVE_CALLS, 0, 0),
    NATIVE("__array_length", "__n00bb_array_length", 0, 0x1, 0),
    NATIVE("__array_prefix", "__n00bb_array_prefix", 0, 0x1, 0x1),
    NATIVE("__array_clear", "__n00bb_array_clear", 0, 0, 0x1),
    NATIVE("__ffi_borrow", "n00b_ffi_borrow", 0, 0x1, 0),
    NATIVE("__ffi_release", "n00b_ffi_release", 0, 0, 0),
    NATIVE("__ffi_unwind_jump", "n00b_ffi_unwind_jump", IR_SUMMARY_DIVERGES,
           0, 0),
};

#undef NATIVE

ir_summary_t *
n00bb_native_axiom(ir_summaries_t *summaries, const ir_func_t *function)
{
    if (!function || function->n_blocks) return nullptr;
    for (size_t i = 0; i < sizeof(builtin_native_names)
                               / sizeof(builtin_native_names[0]); i++) {
        const builtin_native_name_t *native = &builtin_native_names[i];
        if (!core_string_eq(native->native, function->name)) continue;
        ir_summary_t *summary = ir_summary_new(summaries, function);
        ir_summary_set_flags(summary, native->effects);
        for (uint32_t p = 0; p < 8; p++) {
            uint8_t flags = 0;
            if (native->reads & (1u << p))
                flags |= IR_SACCESS_READ | IR_SACCESS_READS_ENTRY;
            if (native->writes & (1u << p)) flags |= IR_SACCESS_WRITE;
            if (flags)
                ir_summary_add_access(summary, (ir_splace_t){
                    .kind = IR_SPLACE_PARAM, .param = p}, flags);
        }
        if (native->effects & IR_SUMMARY_CALLS_UNKNOWN)
            ir_summary_add_access(summary,
                (ir_splace_t){.kind = IR_SPLACE_UNKNOWN},
                IR_SACCESS_READ | IR_SACCESS_WRITE | IR_SACCESS_READS_ENTRY);
        return summary;
    }
    return nullptr;
}

static const core_string_t *
builtin_native_name(core_string_t name)
{
    for (size_t i = 0; i < sizeof(builtin_native_names)
                               / sizeof(builtin_native_names[0]); i++)
        if (core_string_eq(builtin_native_names[i].name, name))
            return &builtin_native_names[i].native;
    return nullptr;
}

static th_type *
one_of_named_types(th_ctx *types, core_arena_t *arena,
                   const core_string_t *names, size_t n)
{
    th_type *variable = th_var(types);
    th_type **choices = arena && variable
        ? core_arena_alloc(arena, n * sizeof(*choices)) : nullptr;
    if (!choices) return nullptr;
    for (size_t i = 0; i < n; i++)
        if (!(choices[i] = th_name(types, names[i]))) return nullptr;
    if (!th_var_constrain(types, variable, (th_constraint){
            .kind = TH_CON_ONE_OF,
            .one_of = {.types = choices, .n = n},
        })) return nullptr;
    return variable;
}

th_type *
n00bb_array_index_type(th_ctx *types, core_arena_t *arena)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("int"), CORE_STRING_STATIC("uint"),
        CORE_STRING_STATIC("u64"), CORE_STRING_STATIC("u32"),
        CORE_STRING_STATIC("u16"), CORE_STRING_STATIC("u8"),
    };
    return one_of_named_types(types, arena, names,
                              sizeof(names) / sizeof(names[0]));
}


static bool
register_builtin_comparison_symbols(parse_symtab_t *symtab,
                                    n00bb_tu_state_t *state,
                                    th_ctx *types)
{
    static const core_string_t names[] = {
        CORE_STRING_STATIC("less"), CORE_STRING_STATIC("equal"),
        CORE_STRING_STATIC("greater"), CORE_STRING_STATIC("unordered"),
    };
    parse_namespace_t *type_ns = parse_symtab_ns(
        symtab, CORE_STRING_STATIC("type"));
    if (!type_ns || !type_ns->root) return false;
    parse_symtab_enter_scope_all(symtab, type_ns->root);
    parse_sym_entry_t *symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("type"), CORE_STRING_STATIC("Comparison"),
        PARSE_SYM_TYPEDEF, nullptr);
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.builtin-type"), symbol)) return false;
    symbol->type = th_name(types, CORE_STRING_STATIC("Comparison"));
    symbol->visibility = CORE_STRING_STATIC("public");
    symbol->adt_kind = CORE_STRING_STATIC("enum");
    parse_symtab_push_scope_all(symtab,
                                CORE_STRING_STATIC("Comparison"));
    parse_scope_t *members = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC(""));
    bool ok = members && symbol->type;
    n00bb_enum_range_t range = {0};
    if (ok) {
        members->scope_tag = CORE_STRING_STATIC("enum");
        members->adt_kind = CORE_STRING_STATIC("enum");
        symbol->member_scope = members;
    }
    for (size_t i = 0; ok && i < sizeof(names) / sizeof(names[0]); i++) {
        parse_sym_entry_t *member = parse_symtab_add(symtab,
            CORE_STRING_STATIC(""), names[i], PARSE_SYM_ENUM_CONST, nullptr);
        n00bb_enum_value_t *value = core_arena_alloc(
            &state->semantic_values, sizeof(*value));
        ok = member && value;
        if (!ok) break;
        value->magnitude = i;
        value->negative = false;
        n00bb_enum_range_include(&range, *value);
        member->type = symbol->type;
        member->owner_type = symbol;
        member->mutability = PARSE_SYM_CONST;
        member->visibility = CORE_STRING_STATIC("public");
        member->comptime_value = value;
        member->has_comptime_value = true;
        parse_sym_set_phases(member, PARSE_SYM_PHASE_BOTH,
            PARSE_SYM_PHASE_NONE, PARSE_SYM_PHASE_COMPTIME);
        parse_sym_set_storage_duration(member,
                                       PARSE_SYM_STORAGE_COMPTIME);
    }
    if (ok) ok = n00bb_bind_enum_range(state->types, &state->semantic_values,
                                 symbol, range);
    parse_symtab_pop_scope_all(symtab);
    return ok;
}

static parse_sym_entry_t *
register_native_signature(parse_symtab_t *symtab, th_ctx *types,
                          th_type *nil_type, core_string_t name,
                          const core_string_t *native_name,
                          th_type *signature)
{
    th_type *contract = signature && native_name
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    parse_sym_entry_t *symbol = contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), name, PARSE_SYM_FUNCTION, nullptr)
        : nullptr;
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.native"),
            (void *)native_name)) return nullptr;
    symbol->type = signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = contract;
    return symbol;
}

static parse_sym_entry_t *
builtin_native_symbol(parse_symtab_t *symtab, th_ctx *types,
                      th_type *nil_type, core_string_t name,
                      th_type *result, th_type *const *arguments,
                      size_t count)
{
    th_param *parameters = count
        ? core_alloc_array(th_param, count) : nullptr;
    if (count && !parameters) return nullptr;
    for (size_t i = 0; i < count; i++) {
        if (!arguments[i]) {
            core_free(parameters);
            return nullptr;
        }
        parameters[i] = (th_param){.mode = TH_IN, .type = arguments[i]};
    }
    th_type *signature = result && nil_type
        ? th_func_with_error(types, result, nil_type, parameters,
                             count, nullptr, nullptr, 0) : nullptr;
    core_free(parameters);
    return register_native_signature(symtab, types, nil_type, name,
        builtin_native_name(name), signature);
}

static bool
register_builtin_native_function(parse_symtab_t *symtab, th_ctx *types,
                                 th_type *nil_type, core_string_t name,
                                 th_type *result,
                                 th_type *const *arguments, size_t count)
{
    return builtin_native_symbol(symtab, types, nil_type, name,
                                 result, arguments, count) != nullptr;
}

static bool
register_collection_free_native(parse_symtab_t *symtab, th_ctx *types,
                                th_type *nil_type, core_string_t name,
                                th_type *result,
                                th_type *const *arguments, size_t count)
{
    parse_sym_entry_t *symbol = builtin_native_symbol(symtab, types,
        nil_type, name, result, arguments, count);
    return symbol && parse_sym_set_binding(symbol,
        CORE_STRING_STATIC("n00bb.native.collection-free"), symbol);
}

static bool
register_bitptr_builtin(parse_symtab_t *symtab, th_ctx *types,
                        th_type *nil_type, th_type *reference)
{
    th_type *result = th_name(types,
                              CORE_STRING_STATIC("builtin.bitptr.Bitptr"));
    parse_sym_entry_t *symbol = builtin_native_symbol(symtab, types,
        nil_type, CORE_STRING_STATIC("__bitptr"), result, &reference, 1);
    return symbol && parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.native.returns-parameter"), symbol)
        && parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.native.collection-free"), symbol);
}

bool
register_native_declarations(parse_annot_result_t *annotations, th_ctx *types,
                             n00bb_native_declarations_t natives)
{
    parse_symtab_t *symtab = annotations ? annotations->symtab : nullptr;
    th_type *nil_type = types ? th_name(types, CORE_STRING_STATIC("nil"))
                              : nullptr;
    if (!symtab || !nil_type) return false;
    for (size_t i = 0; i < natives.count; i++) {
        th_type *failure = nullptr;
        core_string_t error = {0};
        th_type *signature = ir_typelang_parse_function(types,
            natives.items[i].signature, &failure, &error);
        core_free(error.data);
        parse_sym_entry_t *symbol = register_native_signature(symtab, types,
            nil_type, natives.items[i].name, &natives.items[i].symbol,
            signature);
        if (!symbol || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.generated-extern"), symbol))
            return false;
    }
    return true;
}

static bool
register_builtin_os_symbols(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *byte = th_name(types, CORE_STRING_STATIC("byte"));
    th_type *u32 = th_name(types, CORE_STRING_STATIC("u32"));
    th_type *u64 = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *uint = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *i32 = th_name(types, CORE_STRING_STATIC("i32"));
    th_type *mapped_item = th_var(types);
    th_type *reference = mapped_item ? th_ind(types, 0, mapped_item) : nullptr;
    th_type *byte_reference = byte ? th_ind(types, 0, byte) : nullptr;
    th_type *optional_reference = byte_reference
        ? th_option(types, byte_reference) : nullptr;
    th_type *map_args[] = {uint, uint};
    th_type *unmap_args[] = {reference, uint};
    th_type *protect_args[] = {byte_reference, uint, u32};
    th_type *thread_context = th_name(types,
        CORE_STRING_STATIC("sys.internal.threads.Thread_Context"));
    th_type *thread_context_ref = thread_context
        ? th_ind(types, 0, thread_context) : nullptr;
    th_type *suspend_args[] = {uint, thread_context_ref, u64};
    th_type *resume_args[] = {uint, thread_context_ref};
    th_type *capture_args[] = {thread_context_ref};
    th_type *write_args[] = {i32, byte_reference, u32};
    th_type *integer = th_name(types, CORE_STRING_STATIC("int"));
    th_type *open_args[] = {byte_reference, integer, u32};
    th_type *read_args[] = {integer, byte_reference, uint};
    th_type *close_args[] = {integer};
    th_type *wait_item = th_var(types);
    th_type *wake_item = th_var(types);
    th_type *futex_wait_args[] = {
        wait_item ? th_ind(types, 0, wait_item) : nullptr, u32, uint,
    };
    th_type *futex_wake_args[] = {
        wake_item ? th_ind(types, 0, wake_item) : nullptr,
    };
    th_type *seek_args[] = {integer, integer, i32};
    th_type *u16_reference = th_ind(types, 0, th_name(types, CORE_STRING_STATIC("u16")));
    th_type *integer_reference = th_ind(types, 0, integer);
    th_type *env_at_args[] = {uint};
    th_type *terminal_size_args[] = {integer, u16_reference, u16_reference};
    th_type *pipe_args[] = {integer_reference, integer_reference};
    th_type *entry_item = th_var(types);
    th_type *argument_item = th_var(types);
    th_type *thread_start_args[] = {
        entry_item ? th_ind(types, 0, entry_item) : nullptr,
        argument_item ? th_ind(types, 0, argument_item) : nullptr,
    };
    return register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_page_size"), uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__gc_enter_blocking"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__gc_leave_blocking"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_env_count"), uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_env_at"), optional_reference,
            env_at_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_arg_count"), uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_arg_at"), optional_reference,
            env_at_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_fd_is_tty"), integer, close_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_terminal_size"), integer,
            terminal_size_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_pipe"), integer, pipe_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_open"), integer, open_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_read"), integer, read_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_close"), integer, close_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_exit"), nil_type, close_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_futex_wait"), i32, futex_wait_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_futex_wake_all"), i32,
            futex_wake_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_seek"), integer, seek_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__write_bytes"), i32, write_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_yield"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_fence"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_id"), uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_is_main"), uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_start"), i32,
            thread_start_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_stack_high_self"),
            optional_reference, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_stack_red_zone_bytes"),
            uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_control_self"),
            uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_capture_self"),
            nil_type, capture_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_suspend"), i32,
            suspend_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_thread_resume"), i32,
            resume_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_now_ns"),
            th_name(types, CORE_STRING_STATIC("uint")), nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_cpu_relax"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__panic_trace"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_map_zeroed"),
            optional_reference, map_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_unmap"), i32, unmap_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__os_protect"), i32, protect_args, 3);
}

static bool
register_builtin_abi_symbols(parse_symtab_t *symtab, th_ctx *types,
                             th_type *nil_type)
{
    th_type *u32 = th_name(types, CORE_STRING_STATIC("u32"));
    th_type *i64 = th_name(types, CORE_STRING_STATIC("int"));
    th_type *uint = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *typespec = th_name(types, CORE_STRING_STATIC("typespec"));
    static const core_string_t record_names[] = {
        CORE_STRING_STATIC("builtin.abi.Type_Record"),
        CORE_STRING_STATIC("builtin.abi.Field_Record"),
        CORE_STRING_STATIC("builtin.abi.Trace_Record"),
        CORE_STRING_STATIC("builtin.abi.Trace_Child_Record"),
        CORE_STRING_STATIC("builtin.abi.Trace_Arm_Record"),
        CORE_STRING_STATIC("builtin.abi.Tag_Extractor_Record"),
        CORE_STRING_STATIC("builtin.abi.Layout_Record"),
        CORE_STRING_STATIC("builtin.abi.Frame_Layout_Record"),
        CORE_STRING_STATIC("builtin.abi.Storage_Record"),
        CORE_STRING_STATIC("builtin.abi.Symbol_Record"),
        CORE_STRING_STATIC("builtin.abi.Module_Record"),
        CORE_STRING_STATIC("builtin.abi.Baked_Node_Record"),
        CORE_STRING_STATIC("builtin.abi.Function_Record"),
    };
    static const core_string_t names[] = {
        CORE_STRING_STATIC("__abi_type"), CORE_STRING_STATIC("__abi_field"),
        CORE_STRING_STATIC("__abi_trace"),
        CORE_STRING_STATIC("__abi_trace_child"),
        CORE_STRING_STATIC("__abi_trace_arm"),
        CORE_STRING_STATIC("__abi_tag_extractor"),
        CORE_STRING_STATIC("__abi_layout"),
        CORE_STRING_STATIC("__abi_frame_layout"),
        CORE_STRING_STATIC("__abi_storage"),
        CORE_STRING_STATIC("__abi_symbol"),
        CORE_STRING_STATIC("__abi_module"),
        CORE_STRING_STATIC("__abi_baked_node"),
        CORE_STRING_STATIC("__abi_function"),
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        th_type *record = th_name(types, record_names[i]);
        th_type *reference = record ? th_ind(types, 0, record) : nullptr;
        th_type *result = reference ? th_option(types, reference) : nullptr;
        th_type *args[] = {u32};
        if (!register_builtin_native_function(symtab, types, nil_type,
                names[i], result, args, 1)) return false;
        if (i == 0) {
            th_type *descriptor_args[] = {typespec};
            if (!register_builtin_native_function(symtab, types, nil_type,
                    CORE_STRING_STATIC("__abi_type_for"), result,
                    descriptor_args, 1)) return false;
        }
    }
    th_type *range_args[] = {u32, u32, u32};
    th_type *bytes_args[] = {u32, u32};
    th_type *byte_reference = th_ind(types, 0, th_name(types, CORE_STRING_STATIC("byte")));
    th_type *bytes_result = byte_reference
        ? th_option(types, byte_reference) : nullptr;
    th_type *native_frame = th_name(types,
        CORE_STRING_STATIC("builtin.abi.Native_Frame_Record"));
    th_type *native_frame_reference = native_frame
        ? th_ind(types, 0, native_frame) : nullptr;
    th_type *native_frame_result = native_frame_reference
        ? th_option(types, native_frame_reference) : nullptr;
    th_type *native_frame_args[] = {
        byte_reference, byte_reference, byte_reference,
    };
    th_type *native_offset_args[] = {native_frame_reference, u32};
    th_type *item = th_var(types);
    th_type *reference = item ? th_ind(types, 0, item) : nullptr;
    th_type *address_args[] = {reference};
    th_type *storage_args[] = {u32};
    th_type *baked_args[] = {th_name(types, CORE_STRING_STATIC("uint"))};
    return register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_baked_address"),
            th_name(types, CORE_STRING_STATIC("bool")), baked_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__native_symbol_name"),
            bytes_result, baked_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_native_frame_at"),
            native_frame_result, native_frame_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_native_frame_root_offset"),
            th_name(types, CORE_STRING_STATIC("i32")), native_offset_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_storage_count"), u32, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_storage_address"),
            bytes_result, storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_baked_node_count"), u32, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_baked_node_address"), bytes_result,
            storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_string_backing_at"),
            bytes_result, &byte_reference, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_range_index"), i64, range_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_bytes"), bytes_result, bytes_args, 2)
        && register_collection_free_native(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_ref_address"), uint, address_args, 1)
        && register_bitptr_builtin(symtab, types, nil_type, reference)
        && register_collection_free_native(symtab, types, nil_type,
            CORE_STRING_STATIC("__load_le_u64"),
            th_name(types, CORE_STRING_STATIC("u64")), &byte_reference, 1)
        && register_collection_free_native(symtab, types, nil_type,
            CORE_STRING_STATIC("__load_le_u32"), u32, &byte_reference, 1);
}

static n00bb_unsafe_arithmetic_t unsafe_arithmetic[] = {
    {.name = CORE_STRING_STATIC("unsafe_add"), .op = IR_OP_ADD},
    {.name = CORE_STRING_STATIC("unsafe_sub"), .op = IR_OP_SUB},
    {.name = CORE_STRING_STATIC("unsafe_mul"), .op = IR_OP_MUL},
    {.name = CORE_STRING_STATIC("unsafe_shl"), .op = IR_OP_SHL,
     .shift = true},
    {.name = CORE_STRING_STATIC("unsafe_shr"), .op = IR_OP_SHR,
     .shift = true},
    {.name = CORE_STRING_STATIC("unsafe_and"), .op = IR_OP_AND},
    {.name = CORE_STRING_STATIC("unsafe_or"), .op = IR_OP_OR},
    {.name = CORE_STRING_STATIC("unsafe_xor"), .op = IR_OP_XOR},
    {.name = CORE_STRING_STATIC("unsafe_not"), .op = IR_OP_NOT,
     .unary = true},
};

static bool
register_builtin_unsafe_arithmetic(parse_symtab_t *symtab, th_ctx *types,
                                   th_type *nil_type)
{
    th_type *uint_type = th_name(types, CORE_STRING_STATIC("uint"));
    for (size_t i = 0;
         i < sizeof(unsafe_arithmetic) / sizeof(unsafe_arithmetic[0]); i++) {
        n00bb_unsafe_arithmetic_t *entry = &unsafe_arithmetic[i];
        th_type *value = th_var(types);
        th_param parameters[] = {
            {.mode = TH_IN, .type = value},
            {.mode = TH_IN, .type = entry->shift ? uint_type : value},
        };
        th_type *signature = value && uint_type && nil_type
            ? th_func_with_error(types, value, nil_type, parameters,
                                 entry->unary ? 1 : 2, nullptr, nullptr, 0)
            : nullptr;
        th_type *contract = signature
            ? th_env_signature_contract(types, signature, nil_type) : nullptr;
        parse_sym_entry_t *symbol = contract ? parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), entry->name,
            PARSE_SYM_FUNCTION, nullptr) : nullptr;
        if (!symbol || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.integer.unsafe"), entry))
            return false;
        symbol->type = signature;
        symbol->error_type = nil_type;
        symbol->callable_contract = contract;
    }
    return true;
}

static bool
register_builtin_bit_counts(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *uint_type = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *u32_type = th_name(types, CORE_STRING_STATIC("u32"));
    th_param parameter = {.mode = TH_IN, .type = uint_type};
    th_type *signature = uint_type && u32_type && nil_type
        ? th_func_with_error(types, u32_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    static const core_string_t names[] = {
        CORE_STRING_STATIC("__count_leading_zeros"),
        CORE_STRING_STATIC("__count_trailing_zeros"),
    };
    if (!contract) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        parse_sym_entry_t *symbol = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), names[i],
            PARSE_SYM_FUNCTION, nullptr);
        if (!symbol || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.integer.bit_count"), symbol))
            return false;
        symbol->type = signature;
        symbol->error_type = nil_type;
        symbol->callable_contract = contract;
    }
    return true;
}

static bool
register_builtin_gc_symbols(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *byte = th_name(types, CORE_STRING_STATIC("byte"));
    th_type *uint = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *offset_type = th_name(types, CORE_STRING_STATIC("i128"));
    th_type *addressed_item = th_var(types);
    th_type *reference = addressed_item
        ? th_ind(types, 0, addressed_item) : nullptr;
    th_param parameters[] = {
        {.mode = TH_IN, .type = reference},
        {.mode = TH_IN, .type = offset_type},
    };
    th_type *signature = reference && offset_type && nil_type
        ? th_func_with_error(types, reference, nil_type, parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    parse_sym_entry_t *symbol = contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__gc_byte_offset"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.byte_offset"), symbol))
        return false;
    symbol->type = signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = contract;

    th_type *difference_type = th_name(types, CORE_STRING_STATIC("int"));
    th_param diff_parameters[] = {
        {.mode = TH_IN, .type = reference},
        {.mode = TH_IN, .type = reference},
    };
    th_type *diff_signature = difference_type && reference
        ? th_func_with_error(types, difference_type, nil_type,
                             diff_parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *diff_contract = diff_signature
        ? th_env_signature_contract(types, diff_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *diff_symbol = diff_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__gc_byte_diff"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!diff_symbol || !parse_sym_set_binding(diff_symbol,
            CORE_STRING_STATIC("n00bb.gc.byte_diff"), diff_symbol))
        return false;
    diff_symbol->type = diff_signature;
    diff_symbol->error_type = nil_type;
    diff_symbol->callable_contract = diff_contract;

    th_param memset_parameters[] = {
        {.mode = TH_IN, .type = reference},
        {.mode = TH_IN, .type = byte},
        {.mode = TH_IN, .type = uint},
    };
    th_type *memset_signature = reference && byte && uint
        ? th_func_with_error(types, nil_type, nil_type, memset_parameters, 3,
                             nullptr, nullptr, 0) : nullptr;
    th_type *memset_contract = memset_signature
        ? th_env_signature_contract(types, memset_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *memset_symbol = memset_contract ? parse_symtab_add(
        symtab, CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__gc_memset"), PARSE_SYM_FUNCTION,
        nullptr) : nullptr;
    if (!memset_symbol || !parse_sym_set_binding(memset_symbol,
            CORE_STRING_STATIC("n00bb.gc.memory"), memset_symbol))
        return false;
    memset_symbol->type = memset_signature;
    memset_symbol->error_type = nil_type;
    memset_symbol->callable_contract = memset_contract;

    th_type *source_item = th_var(types);
    th_type *source_reference = source_item
        ? th_ind(types, 0, source_item) : nullptr;
    th_param memcpy_parameters[] = {
        {.mode = TH_IN, .type = reference},
        {.mode = TH_IN, .type = source_reference},
        {.mode = TH_IN, .type = uint},
    };
    th_type *memcpy_signature = reference && source_reference && uint
        ? th_func_with_error(types, nil_type, nil_type, memcpy_parameters, 3,
                             nullptr, nullptr, 0) : nullptr;
    th_type *memcpy_contract = memcpy_signature
        ? th_env_signature_contract(types, memcpy_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *memcpy_symbol = memcpy_contract ? parse_symtab_add(
        symtab, CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__gc_memcpy"), PARSE_SYM_FUNCTION,
        nullptr) : nullptr;
    if (!memcpy_symbol || !parse_sym_set_binding(memcpy_symbol,
            CORE_STRING_STATIC("n00bb.gc.memory"), memcpy_symbol))
        return false;
    memcpy_symbol->type = memcpy_signature;
    memcpy_symbol->error_type = nil_type;
    memcpy_symbol->callable_contract = memcpy_contract;

    th_type *volatile_item = th_var(types);
    th_type *volatile_ref = volatile_item
        ? th_ind(types, 0, volatile_item) : nullptr;
    th_param volatile_parameter = {
        .mode = TH_IN, .type = volatile_ref,
    };
    th_type *volatile_signature = volatile_ref
        ? th_func_with_error(types, volatile_item, nil_type,
                             &volatile_parameter, 1, nullptr, nullptr, 0)
        : nullptr;
    th_type *volatile_contract = volatile_signature
        ? th_env_signature_contract(types, volatile_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *volatile_symbol = volatile_contract
        ? parse_symtab_add(symtab, CORE_STRING_STATIC("builtin"),
            CORE_STRING_STATIC("__volatile_load"), PARSE_SYM_FUNCTION,
            nullptr) : nullptr;
    if (!volatile_symbol || !parse_sym_set_binding(volatile_symbol,
            CORE_STRING_STATIC("n00bb.volatile.load"), volatile_symbol))
        return false;
    volatile_symbol->type = volatile_signature;
    volatile_symbol->error_type = nil_type;
    volatile_symbol->callable_contract = volatile_contract;

    th_type *item = th_var(types);
    th_arg item_arg = {.kind = TH_ARG_TYPE, .type = item};
    th_type *generic_atomic = item
        ? th_apply(types, CORE_STRING_STATIC("Atomic"), &item_arg, 1)
        : nullptr;
    th_type *generic_ref = generic_atomic
        ? th_ind(types, 0, generic_atomic) : nullptr;
    th_param load_parameter = {.mode = TH_IN, .type = generic_ref};
    th_param store_parameters[] = {
        {.mode = TH_IN, .type = generic_ref},
        {.mode = TH_IN, .type = item},
    };
    th_param cas_parameters[] = {
        {.mode = TH_IN, .type = generic_ref},
        {.mode = TH_IN, .type = item},
        {.mode = TH_IN, .type = item},
    };
    th_type *load_signature = generic_ref
        ? th_func_with_error(types, item, nil_type, &load_parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    th_type *store_signature = generic_ref
        ? th_func_with_error(types, nil_type, nil_type, store_parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *cas_signature = generic_ref && boolean
        ? th_func_with_error(types, boolean, nil_type, cas_parameters, 3,
                             nullptr, nullptr, 0) : nullptr;
    th_type *exchange_signature = generic_ref
        ? th_func_with_error(types, item, nil_type, store_parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *load_contract = load_signature
        ? th_env_signature_contract(types, load_signature, nil_type) : nullptr;
    th_type *store_contract = store_signature
        ? th_env_signature_contract(types, store_signature, nil_type) : nullptr;
    th_type *cas_contract = cas_signature
        ? th_env_signature_contract(types, cas_signature, nil_type) : nullptr;
    th_type *exchange_contract = exchange_signature
        ? th_env_signature_contract(types, exchange_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *load_symbol = load_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__atomic_load"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    parse_sym_entry_t *store_symbol = store_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__atomic_store"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    parse_sym_entry_t *cas_symbol = cas_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__atomic_compare_exchange"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    parse_sym_entry_t *exchange_symbol = exchange_contract
        ? parse_symtab_add(symtab, CORE_STRING_STATIC("builtin"),
            CORE_STRING_STATIC("__atomic_exchange"), PARSE_SYM_FUNCTION,
            nullptr) : nullptr;
    if (!load_symbol || !store_symbol || !cas_symbol || !exchange_symbol
        || !parse_sym_set_binding(load_symbol,
            CORE_STRING_STATIC("n00bb.atomic.load"), load_symbol)
        || !parse_sym_set_binding(store_symbol,
            CORE_STRING_STATIC("n00bb.atomic.store"), store_symbol)
        || !parse_sym_set_binding(cas_symbol,
            CORE_STRING_STATIC("n00bb.atomic.compare_exchange"),
            cas_symbol)
        || !parse_sym_set_binding(exchange_symbol,
            CORE_STRING_STATIC("n00bb.atomic.exchange"), exchange_symbol))
        return false;
    load_symbol->type = load_signature;
    load_symbol->error_type = nil_type;
    load_symbol->callable_contract = load_contract;
    store_symbol->type = store_signature;
    store_symbol->error_type = nil_type;
    store_symbol->callable_contract = store_contract;
    static const core_string_t ordered_access_names[] = {
        CORE_STRING_STATIC("__atomic_load_relaxed"),
        CORE_STRING_STATIC("__atomic_load_acquire"),
        CORE_STRING_STATIC("__atomic_store_relaxed"),
        CORE_STRING_STATIC("__atomic_store_release"),
    };
    static const core_string_t ordered_access_bindings[] = {
        CORE_STRING_STATIC("n00bb.atomic.load_relaxed"),
        CORE_STRING_STATIC("n00bb.atomic.load_acquire"),
        CORE_STRING_STATIC("n00bb.atomic.store_relaxed"),
        CORE_STRING_STATIC("n00bb.atomic.store_release"),
    };
    for (size_t i = 0; i < sizeof(ordered_access_names)
            / sizeof(ordered_access_names[0]); i++) {
        bool loading = i < 2;
        parse_sym_entry_t *ordered = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), ordered_access_names[i],
            PARSE_SYM_FUNCTION, nullptr);
        if (!ordered || !parse_sym_set_binding(ordered,
                ordered_access_bindings[i], ordered))
            return false;
        ordered->type = loading ? load_signature : store_signature;
        ordered->error_type = nil_type;
        ordered->callable_contract = loading ? load_contract : store_contract;
    }
    cas_symbol->type = cas_signature;
    cas_symbol->error_type = nil_type;
    cas_symbol->callable_contract = cas_contract;
    exchange_symbol->type = exchange_signature;
    exchange_symbol->error_type = nil_type;
    exchange_symbol->callable_contract = exchange_contract;
    static const core_string_t atomic_rmw_names[] = {
        CORE_STRING_STATIC("__atomic_fetch_or"),
        CORE_STRING_STATIC("__atomic_fetch_or_release"),
        CORE_STRING_STATIC("__atomic_fetch_or_relaxed"),
        CORE_STRING_STATIC("__atomic_fetch_and"),
        CORE_STRING_STATIC("__atomic_fetch_add"),
        CORE_STRING_STATIC("__atomic_fetch_add_relaxed"),
        CORE_STRING_STATIC("__atomic_fetch_sub"),
        CORE_STRING_STATIC("__atomic_exchange_acquire"),
        CORE_STRING_STATIC("__atomic_exchange_relaxed"),
    };
    static const core_string_t atomic_rmw_bindings[] = {
        CORE_STRING_STATIC("n00bb.atomic.fetch_or"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_or_release"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_or_relaxed"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_and"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_add"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_add_relaxed"),
        CORE_STRING_STATIC("n00bb.atomic.fetch_sub"),
        CORE_STRING_STATIC("n00bb.atomic.exchange_acquire"),
        CORE_STRING_STATIC("n00bb.atomic.exchange_relaxed"),
    };
    for (size_t i = 0;
         i < sizeof(atomic_rmw_names)
             / sizeof(atomic_rmw_names[0]); i++) {
        parse_sym_entry_t *rmw = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), atomic_rmw_names[i],
            PARSE_SYM_FUNCTION, nullptr);
        if (!rmw || !parse_sym_set_binding(rmw, atomic_rmw_bindings[i],
                rmw)) return false;
        rmw->type = exchange_signature;
        rmw->error_type = nil_type;
        rmw->callable_contract = exchange_contract;
    }
    th_type *fence_signature = nil_type
        ? th_func_with_error(types, nil_type, nil_type, nullptr, 0,
                             nullptr, nullptr, 0) : nullptr;
    th_type *fence_contract = fence_signature
        ? th_env_signature_contract(types, fence_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *fence = fence_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__gc_signal_fence"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!fence || !parse_sym_set_binding(fence,
            CORE_STRING_STATIC("n00bb.gc.signal_fence"), fence)) return false;
    fence->type = fence_signature;
    fence->error_type = nil_type;
    fence->callable_contract = fence_contract;
    th_type *prefetch_item = th_var(types);
    th_type *prefetch_ref = prefetch_item
        ? th_ind(types, 0, prefetch_item) : nullptr;
    th_param prefetch_parameter = {.mode = TH_IN, .type = prefetch_ref};
    th_type *prefetch_signature = prefetch_ref
        ? th_func_with_error(types, nil_type, nil_type,
            &prefetch_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_type *prefetch_contract = prefetch_signature
        ? th_env_signature_contract(types, prefetch_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *prefetch = prefetch_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__gc_prefetch"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!prefetch || !parse_sym_set_binding(prefetch,
            CORE_STRING_STATIC("n00bb.gc.prefetch"), prefetch)) return false;
    prefetch->type = prefetch_signature;
    prefetch->error_type = nil_type;
    prefetch->callable_contract = prefetch_contract;
    th_type *payload_type = th_ind(types, 0, byte);
    th_param invoke_parameters[] = {
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("u32"))},
        {.mode = TH_IN, .type = payload_type},
    };
    th_type *invoke_signature = payload_type
        ? th_func_with_error(types, nil_type, nil_type,
            invoke_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *invoke_contract = invoke_signature
        ? th_env_signature_contract(types, invoke_signature, nil_type)
        : nullptr;
    parse_sym_entry_t *invoke = invoke_contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__gc_invoke_finalizer"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!invoke || !parse_sym_set_binding(invoke,
            CORE_STRING_STATIC("n00bb.abi.native"),
            (void *)builtin_native_name(
                CORE_STRING_STATIC("__gc_invoke_finalizer")))
        || !parse_sym_set_binding(invoke,
            CORE_STRING_STATIC("n00bb.native.calls-n00b"), invoke))
        return false;
    invoke->type = invoke_signature;
    invoke->error_type = nil_type;
    invoke->callable_contract = invoke_contract;
    return true;
}

bool
register_builtin_symbols(parse_annot_result_t *annotations,
                         n00bb_tu_state_t *state, th_ctx *types)
{
    parse_symtab_t *symtab = annotations ? annotations->symtab : nullptr;
    if (!symtab || !types) return false;
    parse_namespace_t *values = parse_symtab_ns(
        symtab, CORE_STRING_STATIC("builtin"));
    if (!values || !values->root) return false;

    th_type *value_type = th_var(types);
    th_type *spec_type = th_name(types, CORE_STRING_STATIC("typespec"));
    th_type *bool_type = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_param parameters[] = {
        {.mode = TH_IN, .type = value_type},
        {.mode = TH_IN, .type = spec_type},
    };
    th_type *signature = bool_type && nil_type && spec_type
        ? th_func_with_error(types, bool_type, nil_type, parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    th_type *referent = th_var(types);
    th_type *reference = referent ? th_ind(types, 0, referent) : nullptr;
    th_param read_parameter = {.mode = TH_IN, .type = reference};
    th_type *read_signature = reference
        ? th_func_with_error(types, referent, nil_type, &read_parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    th_type *read_contract = read_signature
        ? th_env_signature_contract(types, read_signature, nil_type) : nullptr;
    th_param write_parameters[] = {
        {.mode = TH_IN, .type = reference},
        {.mode = TH_IN, .type = referent},
    };
    th_type *write_signature = reference
        ? th_func_with_error(types, nil_type, nil_type, write_parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *write_contract = write_signature
        ? th_env_signature_contract(types, write_signature, nil_type)
        : nullptr;
    th_type *option_is_nil_item = th_var(types);
    th_type *option_is_nil_type = option_is_nil_item
        ? th_option(types, option_is_nil_item) : nullptr;
    th_param option_is_nil_parameter = {
        .mode = TH_IN, .type = option_is_nil_type,
    };
    th_type *option_is_nil_signature = option_is_nil_type && bool_type
        ? th_func_with_error(types, bool_type, nil_type,
            &option_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_type *option_get_item = th_var(types);
    th_type *option_get_type = option_get_item
        ? th_option(types, option_get_item) : nullptr;
    th_param option_get_parameter = {.mode = TH_IN, .type = option_get_type};
    th_type *option_get_signature = option_get_type
        ? th_func_with_error(types, option_get_item, nil_type,
            &option_get_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_type *option_default_item = th_var(types);
    th_type *option_default_type = option_default_item
        ? th_option(types, option_default_item) : nullptr;
    th_param option_get_or_else_parameters[] = {
        {.mode = TH_IN, .type = option_default_type},
        {.mode = TH_IN, .type = option_default_item},
    };
    th_type *option_get_or_else_signature = option_default_type
        ? th_func_with_error(types, option_default_item, nil_type,
            option_get_or_else_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *option_is_nil_contract = option_is_nil_signature
        ? th_env_signature_contract(types, option_is_nil_signature, nil_type)
        : nullptr;
    th_type *option_get_contract = option_get_signature
        ? th_env_signature_contract(types, option_get_signature, nil_type)
        : nullptr;
    th_type *option_get_or_else_contract = option_get_or_else_signature
        ? th_env_signature_contract(types, option_get_or_else_signature,
                                    nil_type) : nullptr;
    th_type *array_item = th_var(types);
    th_type *array_type = th_array(types, array_item);
    th_type *array_write_index = n00bb_array_index_type(
        types, &state->semantic_values);
    th_param array_write_parameters[] = {
        {.mode = TH_IN, .type = array_type},
        {.mode = TH_IN, .type = array_write_index},
        {.mode = TH_IN, .type = array_item},
    };
    th_type *array_write_signature = array_type && array_write_index
        ? th_func_with_error(types, nil_type, nil_type,
            array_write_parameters, 3, nullptr, nullptr, 0) : nullptr;
    th_type *array_write_contract = array_write_signature
        ? th_env_signature_contract(types, array_write_signature, nil_type)
        : nullptr;
    th_type *array_read_index = n00bb_array_index_type(
        types, &state->semantic_values);
    th_param array_read_parameters[] = {
        {.mode = TH_IN, .type = array_type},
        {.mode = TH_IN, .type = array_read_index},
    };
    th_type *array_read_signature = array_type && array_read_index
        ? th_func_with_error(types, array_item, nil_type,
            array_read_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *array_read_contract = array_read_signature
        ? th_env_signature_contract(types, array_read_signature, nil_type)
        : nullptr;
    th_type *init_receiver = th_var(types);
    th_type *init_argument = th_var(types);
    th_param init_parameter = {.mode = TH_INOUT, .type = init_receiver};
    th_param init_rest = {.mode = TH_IN, .type = init_argument};
    th_type *init_signature = init_receiver && init_argument
        ? th_func_with_error(types, nil_type, nil_type, &init_parameter, 1,
            &init_rest, nullptr, 0) : nullptr;
    th_type *init_contract = init_signature
        ? th_env_signature_contract(types, init_signature, nil_type)
        : nullptr;
    if (!signature || !contract || !read_signature || !read_contract
        || !write_signature || !write_contract || !option_is_nil_signature
        || !option_get_signature || !option_get_or_else_signature
        || !option_is_nil_contract || !option_get_contract
        || !option_get_or_else_contract
        || !array_read_contract
        || !array_write_contract)
        return false;

    parse_scope_t *saved = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             values->root);
    static const core_string_t comparison_traits[] = {
        CORE_STRING_STATIC("Equal"), CORE_STRING_STATIC("Comparable"),
    };
    for (size_t i = 0; i < sizeof(comparison_traits)
             / sizeof(comparison_traits[0]); i++) {
        core_string_t name = comparison_traits[i];
        parse_sym_entry_t *trait = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), name, PARSE_SYM_TYPEDEF, nullptr);
        if (!trait || !parse_sym_set_binding(trait,
                CORE_STRING_STATIC("n00bb.builtin-type"), trait)) return false;
        trait->type = th_name(types, comparison_traits[i]);
        trait->adt_kind = CORE_STRING_STATIC("trait");
        trait->visibility = CORE_STRING_STATIC("public");
        parse_symtab_push_scope(symtab, CORE_STRING_STATIC("builtin"), name);
        trait->member_scope = parse_symtab_current_scope(symtab,
            CORE_STRING_STATIC("builtin"));
        trait->member_scope->scope_tag = CORE_STRING_STATIC("trait");
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                                 values->root);
    }
    parse_sym_entry_t *symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("has_type"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *read_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("ref_read"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *write_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("ref_write"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *option_is_nil_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__option_is_nil"), PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *option_get_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__option_get"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *option_get_or_else_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"),
        CORE_STRING_STATIC("__option_get_or_else"), PARSE_SYM_FUNCTION,
        nullptr);
    parse_sym_entry_t *array_write_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__array_write"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *array_read_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__array_read"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *init_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("init"),
        PARSE_SYM_FUNCTION, nullptr);
    bool os_builtins_registered = register_builtin_os_symbols(symtab, types,
                                                               nil_type);
    bool abi_builtins_registered = register_builtin_abi_symbols(symtab, types,
                                                                 nil_type);
    bool gc_builtins_registered = register_builtin_gc_symbols(symtab, types,
                                                               nil_type);
    bool unsafe_arithmetic_registered = register_builtin_unsafe_arithmetic(
        symtab, types, nil_type);
    bool bit_counts_registered = register_builtin_bit_counts(symtab, types,
                                                              nil_type);
    th_type *array_length_item = th_var(types);
    th_type *array_length_type = array_length_item
        ? th_array(types, array_length_item) : nullptr;
    th_type *array_length_arguments[] = {array_length_type};
    bool array_length_registered = register_builtin_native_function(symtab,
        types, nil_type, CORE_STRING_STATIC("__array_length"),
        th_name(types, CORE_STRING_STATIC("uint")), array_length_arguments, 1);
    th_type *array_prefix_arguments[] = {
        array_length_type, th_name(types, CORE_STRING_STATIC("uint"))};
    bool array_prefix_registered = register_builtin_native_function(symtab,
        types, nil_type, CORE_STRING_STATIC("__array_prefix"),
        array_length_type, array_prefix_arguments, 2);
    th_type *array_reference = array_length_type
        ? th_ind(types, 0, array_length_type) : nullptr;
    th_type *array_clear_arguments[] = {
        array_reference, th_name(types, CORE_STRING_STATIC("uint"))};
    bool array_clear_registered = register_builtin_native_function(symtab,
        types, nil_type, CORE_STRING_STATIC("__array_clear"),
        nil_type, array_clear_arguments, 2);
    th_type *borrowed_array = th_ind(types, 0,
        th_array(types, th_name(types, CORE_STRING_STATIC("u8"))));
    th_type *borrow_arguments[] = {borrowed_array,
        th_ind(types, 0, th_name(types, CORE_STRING_STATIC("uint"))), th_name(types, CORE_STRING_STATIC("u32"))};
    th_type *release_arguments[] = {borrowed_array};
    bool borrow_registered = register_builtin_native_function(symtab, types,
            nil_type, CORE_STRING_STATIC("__ffi_borrow"),
            th_name(types, CORE_STRING_STATIC("uint")), borrow_arguments, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__ffi_release"), nil_type,
            release_arguments, 1);
    th_type *unwind_arguments[] = {th_name(types, CORE_STRING_STATIC("uint"))};
    borrow_registered = borrow_registered
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__ffi_unwind_jump"), nil_type,
            unwind_arguments, 1);
    if (saved)
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                                 saved);
    if (!symbol || !read_symbol || !write_symbol || !option_is_nil_symbol
        || !option_get_symbol || !option_get_or_else_symbol
        || !array_read_symbol || !array_write_symbol
        || !init_symbol || !init_contract
        || !os_builtins_registered || !abi_builtins_registered
        || !gc_builtins_registered || !unsafe_arithmetic_registered
        || !bit_counts_registered || !array_length_registered
        || !array_prefix_registered || !array_clear_registered
        || !borrow_registered) return false;
    symbol->type = signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = contract;
    read_symbol->type = read_signature;
    read_symbol->error_type = nil_type;
    read_symbol->callable_contract = read_contract;
    write_symbol->type = write_signature;
    write_symbol->error_type = nil_type;
    write_symbol->callable_contract = write_contract;
    option_is_nil_symbol->type = option_is_nil_signature;
    option_is_nil_symbol->error_type = nil_type;
    option_is_nil_symbol->callable_contract = option_is_nil_contract;
    option_get_symbol->type = option_get_signature;
    option_get_symbol->error_type = nil_type;
    option_get_symbol->callable_contract = option_get_contract;
    option_get_or_else_symbol->type = option_get_or_else_signature;
    option_get_or_else_symbol->error_type = nil_type;
    option_get_or_else_symbol->callable_contract = option_get_or_else_contract;
    array_read_symbol->type = array_read_signature;
    array_read_symbol->error_type = nil_type;
    array_read_symbol->callable_contract = array_read_contract;
    if (!parse_sym_set_binding(array_read_symbol,
            CORE_STRING_STATIC("n00bb.array.read"),
            array_read_symbol)) return false;
    array_write_symbol->type = array_write_signature;
    array_write_symbol->error_type = nil_type;
    array_write_symbol->callable_contract = array_write_contract;
    if (!parse_sym_set_binding(array_write_symbol,
            CORE_STRING_STATIC("n00bb.array.write"),
            array_write_symbol)) return false;
    init_symbol->type = init_signature;
    init_symbol->error_type = nil_type;
    init_symbol->callable_contract = init_contract;
    parse_sym_set_binding(init_symbol, CORE_STRING_STATIC("n00bb.init"),
                          init_symbol);
    return register_builtin_comparison_symbols(symtab, state, types)
        && symbol->callable_contract && read_symbol->callable_contract
        && write_symbol->callable_contract
        && option_is_nil_symbol->callable_contract
        && option_get_symbol->callable_contract
        && option_get_or_else_symbol->callable_contract
        && array_read_symbol->callable_contract
        && array_write_symbol->callable_contract
        && init_symbol->callable_contract;
}

bool
n00bb_register_operator_function(th_env *environment, th_ctx *types,
                                 core_string_t trait, core_string_t method,
                                 core_string_t owner, th_type *self,
                                 th_type *other, th_type *result,
                                 core_string_t module, core_string_t package,
                                 th_env_visibility_t visibility)
{
    th_param parameters[] = {
        {.mode = TH_IN, .type = self},
        {.mode = TH_IN, .type = other},
    };
    th_type *signature = th_func_with_error(types, result,
        th_name(types, CORE_STRING_STATIC("nil")), parameters, other ? 2 : 1,
        nullptr, nullptr, 0);
    core_buffer_t qualified = {0};
    core_buffer_printf(&qualified, "%.*s.%.*s.%.*s.%.*s",
                       (int)module.u8_bytes, module.data,
                       (int)trait.u8_bytes, trait.data,
                       (int)owner.u8_bytes, owner.data,
                       (int)method.u8_bytes, method.data);
    bool registered = signature && qualified.data
        && th_env_register_function(environment,
                                    core_buffer_string(&qualified), method,
                                    module, package, visibility, signature);
    core_buffer_release(&qualified);
    return registered;
}

const n00bb_binary_operator_t n00bb_binary_operators[] = {
    {.spelling = CORE_STRING_STATIC("+"),
     .assign_spelling = CORE_STRING_STATIC("+="),
     .trait = CORE_STRING_STATIC("Add"), .method = CORE_STRING_STATIC("__add"),
     .in_place_trait = CORE_STRING_STATIC("Add_Assign"),
     .in_place = CORE_STRING_STATIC("__add_assign"), .op = IR_OP_ADD},
    {.spelling = CORE_STRING_STATIC("-"),
     .assign_spelling = CORE_STRING_STATIC("-="),
     .trait = CORE_STRING_STATIC("Subtract"),
     .method = CORE_STRING_STATIC("__subtract"),
     .in_place_trait = CORE_STRING_STATIC("Subtract_Assign"),
     .in_place = CORE_STRING_STATIC("__subtract_assign"), .op = IR_OP_SUB},
    {.spelling = CORE_STRING_STATIC("*"),
     .assign_spelling = CORE_STRING_STATIC("*="),
     .trait = CORE_STRING_STATIC("Multiply"),
     .method = CORE_STRING_STATIC("__multiply"),
     .in_place_trait = CORE_STRING_STATIC("Multiply_Assign"),
     .in_place = CORE_STRING_STATIC("__multiply_assign"), .op = IR_OP_MUL},
    {.spelling = CORE_STRING_STATIC("/"),
     .assign_spelling = CORE_STRING_STATIC("/="),
     .trait = CORE_STRING_STATIC("Divide"),
     .method = CORE_STRING_STATIC("__divide"),
     .in_place_trait = CORE_STRING_STATIC("Divide_Assign"),
     .in_place = CORE_STRING_STATIC("__divide_assign"), .op = IR_OP_DIV},
    {.spelling = CORE_STRING_STATIC("%"),
     .assign_spelling = CORE_STRING_STATIC("%="),
     .trait = CORE_STRING_STATIC("Remainder"),
     .method = CORE_STRING_STATIC("__remainder"),
     .in_place_trait = CORE_STRING_STATIC("Remainder_Assign"),
     .in_place = CORE_STRING_STATIC("__remainder_assign"), .op = IR_OP_REM},
    {.spelling = CORE_STRING_STATIC("&"),
     .assign_spelling = CORE_STRING_STATIC("&="),
     .trait = CORE_STRING_STATIC("Bit_And"),
     .method = CORE_STRING_STATIC("__bit_and"),
     .in_place_trait = CORE_STRING_STATIC("Bit_And_Assign"),
     .in_place = CORE_STRING_STATIC("__bit_and_assign"), .op = IR_OP_AND,
     .unsigned_only = true},
    {.spelling = CORE_STRING_STATIC("|"),
     .assign_spelling = CORE_STRING_STATIC("|="),
     .trait = CORE_STRING_STATIC("Bit_Or"),
     .method = CORE_STRING_STATIC("__bit_or"),
     .in_place_trait = CORE_STRING_STATIC("Bit_Or_Assign"),
     .in_place = CORE_STRING_STATIC("__bit_or_assign"), .op = IR_OP_OR,
     .unsigned_only = true},
    {.spelling = CORE_STRING_STATIC("^"),
     .assign_spelling = CORE_STRING_STATIC("^="),
     .trait = CORE_STRING_STATIC("Bit_Xor"),
     .method = CORE_STRING_STATIC("__bit_xor"),
     .in_place_trait = CORE_STRING_STATIC("Bit_Xor_Assign"),
     .in_place = CORE_STRING_STATIC("__bit_xor_assign"), .op = IR_OP_XOR,
     .unsigned_only = true},
    {.spelling = CORE_STRING_STATIC("<<"),
     .assign_spelling = CORE_STRING_STATIC("<<="),
     .trait = CORE_STRING_STATIC("Shift_Left"),
     .method = CORE_STRING_STATIC("__shift_left"),
     .in_place_trait = CORE_STRING_STATIC("Shift_Left_Assign"),
     .in_place = CORE_STRING_STATIC("__shift_left_assign"), .op = IR_OP_SHL,
     .unsigned_only = true, .shift = true},
    {.spelling = CORE_STRING_STATIC(">>"),
     .assign_spelling = CORE_STRING_STATIC(">>="),
     .trait = CORE_STRING_STATIC("Shift_Right"),
     .method = CORE_STRING_STATIC("__shift_right"),
     .in_place_trait = CORE_STRING_STATIC("Shift_Right_Assign"),
     .in_place = CORE_STRING_STATIC("__shift_right_assign"), .op = IR_OP_SHR,
     .unsigned_only = true, .shift = true},
};

const size_t n00bb_binary_operator_count =
    sizeof(n00bb_binary_operators) / sizeof(n00bb_binary_operators[0]);

const n00bb_binary_operator_t *
n00bb_binary_operator_spelled(core_string_t spelling)
{
    for (size_t i = 0; i < n00bb_binary_operator_count; i++)
        if (core_string_eq(spelling, n00bb_binary_operators[i].spelling))
            return &n00bb_binary_operators[i];
    return nullptr;
}

const n00bb_binary_operator_t *
n00bb_binary_operator_assigned(core_string_t spelling)
{
    for (size_t i = 0; i < n00bb_binary_operator_count; i++)
        if (core_string_eq(spelling,
                           n00bb_binary_operators[i].assign_spelling))
            return &n00bb_binary_operators[i];
    return nullptr;
}

const n00bb_binary_operator_t *
n00bb_binary_operator_method(core_string_t method)
{
    for (size_t i = 0; i < n00bb_binary_operator_count; i++)
        if (core_string_eq(method, n00bb_binary_operators[i].method))
            return &n00bb_binary_operators[i];
    return nullptr;
}

static bool
register_operator_trait(th_env *environment, th_ctx *types,
                        core_string_t trait, core_string_t method_name,
                        th_mode receiver_mode, th_type *result)
{
    th_param parameters[] = {
        {.mode = receiver_mode, .type = th_var(types)},
        {.mode = TH_IN, .type = th_var(types)},
    };
    th_type *contract = result
        ? th_func_with_error(types, result, th_var(types), parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_member method = {.name = method_name, .type = contract};
    th_type *methods = contract ? th_record(types, &method, 1) : nullptr;
    return methods && th_env_register_trait_contracts(environment, trait,
        CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods);
}

static const core_string_t numeric_types[] = {
    CORE_STRING_STATIC("int"), CORE_STRING_STATIC("uint"),
    CORE_STRING_STATIC("i8"), CORE_STRING_STATIC("i16"),
    CORE_STRING_STATIC("i32"), CORE_STRING_STATIC("i128"),
    CORE_STRING_STATIC("u8"), CORE_STRING_STATIC("u16"),
    CORE_STRING_STATIC("u32"), CORE_STRING_STATIC("u128"),
    CORE_STRING_STATIC("float"), CORE_STRING_STATIC("f64"),
    CORE_STRING_STATIC("f32"),
};

static bool
register_builtin_binary_operators(th_env *environment, th_ctx *types,
                                  core_arena_t *arena)
{
    const n00bb_binary_operator_t *operators = n00bb_binary_operators;
    for (size_t o = 0; o < n00bb_binary_operator_count; o++) {
        if (!register_operator_trait(environment, types, operators[o].trait,
                operators[o].method, TH_IN, th_var(types))
            || !register_operator_trait(environment, types,
                operators[o].in_place_trait, operators[o].in_place,
                TH_INOUT, th_name(types, CORE_STRING_STATIC("nil")))) return false;
        for (size_t n = 0; n < sizeof(numeric_types) / sizeof(numeric_types[0]); n++) {
            core_string_t name = numeric_types[n];
            th_type *self = th_name(types, name);
            if (operators[o].unsigned_only && !n00bb_bitwise_type(self))
                continue;
            th_type *other = operators[o].shift
                ? n00bb_array_index_type(types, arena) : self;
            if (!n00bb_register_operator_function(environment, types,
                    operators[o].trait, operators[o].method, name, self,
                    other, self, CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC))
                return false;
        }
    }
    return true;
}

static bool
register_builtin_unary_operators(th_env *environment, th_ctx *types)
{
    static const struct {
        core_string_t trait;
        core_string_t method;
        bool boolean;
    } operators[] = {
        {CORE_STRING_STATIC("Negate"), CORE_STRING_STATIC("__negate"), false},
        {CORE_STRING_STATIC("Not"), CORE_STRING_STATIC("__not"), true},
    };
    for (size_t o = 0; o < sizeof(operators) / sizeof(operators[0]); o++) {
        th_type *receiver = th_var(types);
        th_type *result = operators[o].boolean
            ? th_name(types, CORE_STRING_STATIC("bool")) : th_var(types);
        th_param contract_param = {.mode = TH_IN, .type = receiver};
        th_type *contract = th_func_with_error(types, result,
            th_var(types), &contract_param, 1, nullptr, nullptr, 0);
        th_member method = {
            .name = operators[o].method,
            .type = contract,
        };
        th_type *methods = method.type ? th_record(types, &method, 1) : nullptr;
        if (!methods || !th_env_register_trait_contracts(environment,
                operators[o].trait, CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
                methods)) return false;
        size_t count = operators[o].boolean ? 1
            : sizeof(numeric_types) / sizeof(numeric_types[0]);
        for (size_t n = 0; n < count; n++) {
            core_string_t name = operators[o].boolean
                ? CORE_STRING_STATIC("bool") : numeric_types[n];
            th_type *self = th_name(types, name);
            if (!n00bb_register_operator_function(environment, types,
                    operators[o].trait, operators[o].method, name, self,
                    nullptr, self, CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC))
                return false;
        }
    }
    return true;
}

static bool
register_builtin_comparison_operators(th_env *environment, th_ctx *types)
{
    const core_string_t *ordered_types = numeric_types;
    static const core_string_t equality_types[] = {
        CORE_STRING_STATIC("bool"), CORE_STRING_STATIC("char"),
        CORE_STRING_STATIC("byte"), CORE_STRING_STATIC("typespec"),
    };
    static const struct {
        core_string_t trait;
        core_string_t method;
        core_string_t result;
    } operators[] = {
        {CORE_STRING_STATIC("Equal"), CORE_STRING_STATIC("__equal"),
         CORE_STRING_STATIC("bool")},
        {CORE_STRING_STATIC("Comparable"), CORE_STRING_STATIC("__cmp"),
         CORE_STRING_STATIC("Comparison")},
    };
    th_type *comparison = th_name(types, CORE_STRING_STATIC("Comparison"));
    th_member members[] = {
        {.name = CORE_STRING_STATIC("less"), .type = comparison},
        {.name = CORE_STRING_STATIC("equal"), .type = comparison},
        {.name = CORE_STRING_STATIC("greater"), .type = comparison},
        {.name = CORE_STRING_STATIC("unordered"), .type = comparison},
    };
    th_type *shape = comparison ? th_record(types, members,
        sizeof(members) / sizeof(members[0])) : nullptr;
    if (!shape || !th_env_register_type(environment,
            CORE_STRING_STATIC("Comparison"), CORE_STRING_STATIC("builtins"),
            CORE_STRING_STATIC("default"), TH_ENV_PUBLIC)
        || !th_env_register_type_shape(environment,
            CORE_STRING_STATIC("Comparison"),
            TH_ENV_TYPE_SHAPE_ENUM, shape)) return false;
    for (size_t o = 0; o < sizeof(operators) / sizeof(operators[0]); o++) {
        th_type *receiver = th_var(types);
        th_type *argument = th_var(types);
        th_type *result = th_name(types, operators[o].result);
        th_param parameters[] = {
            {.mode = TH_IN, .type = receiver},
            {.mode = TH_IN, .type = argument},
        };
        th_type *contract = th_func_with_error(types, result,
            th_name(types, CORE_STRING_STATIC("nil")), parameters, 2, nullptr, nullptr, 0);
        th_member method = {
            .name = operators[o].method,
            .type = contract,
        };
        th_type *methods = method.type
            ? th_trait(types, operators[o].trait, &method, 1, nullptr, 0)
            : nullptr;
        if (!methods || !th_env_register_trait_contracts(environment,
                operators[o].trait, CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
                methods)) return false;
        size_t ordered_count = sizeof(numeric_types) / sizeof(numeric_types[0]);
        bool equal_trait = core_string_eq(operators[o].trait,
                                          CORE_STRING_STATIC("Equal"));
        size_t equality_count = equal_trait
            ? sizeof(equality_types) / sizeof(equality_types[0]) : 0;
        for (size_t n = 0; n <= ordered_count + equality_count; n++) {
            bool sized_family = n == ordered_count + equality_count;
            core_string_t name = sized_family
                ? CORE_STRING_STATIC("sized_integer")
                : n < ordered_count ? ordered_types[n]
                : equality_types[n - ordered_count];
            th_type *self = sized_family ? th_var(types) : th_name(types, name);
            if (sized_family && !th_var_constrain(types, self,
                    (th_constraint){.kind = TH_CON_KIND,
                                    .type_kind = TH_BITINT})) return false;
            th_param primitive_params[] = {
                {.mode = TH_IN, .type = self},
                {.mode = TH_IN, .type = self},
            };
            th_type *signature = th_func_with_error(types, result,
                th_name(types, CORE_STRING_STATIC("nil")), primitive_params, 2,
                nullptr, nullptr, 0);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "builtins.%.*s.%.*s.%.*s",
                (int)operators[o].trait.u8_bytes, operators[o].trait.data,
                (int)name.u8_bytes, name.data,
                (int)operators[o].method.u8_bytes, operators[o].method.data);
            bool registered = signature && qualified.data
                && th_env_register_function(environment,
                    core_buffer_string(&qualified),
                    operators[o].method, CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"),
                    TH_ENV_PUBLIC, signature);
            core_buffer_release(&qualified);
            if (!registered) return false;
        }
        if (equal_trait) {
            th_type *referent = th_var(types);
            th_type *reference = referent
                ? th_ind(types, 0, referent) : nullptr;
            th_param ref_params[] = {
                {.mode = TH_IN, .type = reference},
                {.mode = TH_IN, .type = reference},
            };
            th_type *signature = reference
                ? th_func_with_error(types, result,
                    th_name(types, CORE_STRING_STATIC("nil")), ref_params, 2,
                    nullptr, nullptr, 0) : nullptr;
            if (!signature || !th_env_register_function(environment,
                    CORE_STRING_STATIC("builtins.Equal.reference.__equal"),
                    CORE_STRING_STATIC("__equal"),
                    CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, signature))
                return false;
        }
    }
    return true;
}


bool
register_builtin_traits(th_env *environment, th_ctx *types,
                        core_arena_t *arena)
{
    th_type *methods = th_record(types, nullptr, 0);
    if (!methods ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Sequence_Like"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Set_Like"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Dict_Like"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Brace_Literal"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods))
        return false;
    th_type *callback_referent = th_var(types);
    th_type *callback_reference = callback_referent
        ? th_ind(types, 0, callback_referent) : nullptr;
    if (!callback_referent || !callback_reference
        || !th_var_constrain(types, callback_referent,
            (th_constraint){.kind = TH_CON_KIND,
                            .type_kind = TH_FUNCTION})
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Callback"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods)
        || !th_env_register_intrinsic_type_pattern(environment, CORE_STRING_STATIC("Callback"),
                                                    callback_reference))
        return false;
    th_type *finalizer_receiver = th_var(types);
    th_param finalizer_parameter = {
        .mode = TH_CONSUME, .type = finalizer_receiver,
    };
    th_type *finalizer_signature = finalizer_receiver
        ? th_func_with_error(types, th_name(types, CORE_STRING_STATIC("nil")),
            th_name(types, CORE_STRING_STATIC("nil")), &finalizer_parameter, 1,
            nullptr, nullptr, 0) : nullptr;
    th_member finalizer_method = {
        .name = CORE_STRING_STATIC("__finalize"), .type = finalizer_signature,
    };
    th_type *finalizer_contract = finalizer_signature
        ? th_record(types, &finalizer_method, 1) : nullptr;
    if (!finalizer_contract
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Finalize"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, finalizer_contract))
        return false;
    if (!register_builtin_binary_operators(environment, types, arena))
        return false;
    if (!register_builtin_unary_operators(environment, types)) return false;
    if (!register_builtin_comparison_operators(environment, types)) return false;
    th_type *source = th_var(types);
    th_type *cursor = th_var(types);
    th_type *iter_error = th_var(types);
    th_param iter_param = {.mode = TH_IN, .type = source};
    th_type *iter_signature = th_func_with_error(types, cursor, iter_error,
        &iter_param, 1, nullptr, nullptr, 0);
    th_member iter_method = {
        .name = CORE_STRING_STATIC("iter"),
        .type = iter_signature,
    };
    th_type *iter_contracts = iter_method.type
        ? th_record(types, &iter_method, 1) : nullptr;

    th_type *iterator = th_var(types);
    th_type *item = th_var(types);
    th_type *next_error = th_var(types);
    th_type *optional_item = th_option(types, item);
    th_param next_param = {.mode = TH_INOUT, .type = iterator};
    th_type *next_signature = optional_item
        ? th_func_with_error(types, optional_item, next_error, &next_param,
            1, nullptr, nullptr, 0) : nullptr;
    th_member next_method = {
        .name = CORE_STRING_STATIC("next"),
        .type = next_signature,
    };
    th_param cleanup_param = {.mode = TH_CONSUME, .type = iterator};
    th_type *cleanup_signature = th_func_with_error(types,
        th_name(types, CORE_STRING_STATIC("nil")), th_name(types, CORE_STRING_STATIC("nil")), &cleanup_param, 1,
        nullptr, nullptr, 0);
    th_member iterator_methods[] = {
        next_method,
        {.name = CORE_STRING_STATIC("cleanup"), .type = cleanup_signature},
    };
    th_type *next_contracts = next_method.type && cleanup_signature
        ? th_record(types, iterator_methods, 2) : nullptr;
    if (!iter_contracts || !next_contracts
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Iterable"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, iter_contracts)
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Iterator"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, next_contracts))
        return false;

    th_type *option_receiver = th_var(types);
    th_type *option_result = th_var(types);
    th_type *option_bool = th_name(types, CORE_STRING_STATIC("bool"));
    th_param is_nil_parameter = {.mode = TH_IN, .type = option_receiver};
    th_type *is_nil_signature = option_bool
        ? th_func_with_error(types, option_bool, th_name(types, CORE_STRING_STATIC("nil")),
            &is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_param get_parameter = {.mode = TH_IN, .type = option_receiver};
    th_type *get_signature = th_func_with_error(types, option_result,
        th_name(types, CORE_STRING_STATIC("nil")), &get_parameter, 1, nullptr, nullptr, 0);
    th_type *option_fallback_receiver = th_var(types);
    th_type *option_fallback_result = th_var(types);
    th_param get_or_else_parameters[] = {
        {.mode = TH_IN, .type = option_fallback_receiver},
        {.mode = TH_IN, .type = option_fallback_result},
    };
    th_type *get_or_else_signature = th_func_with_error(types,
        option_fallback_result, th_name(types, CORE_STRING_STATIC("nil")),
        get_or_else_parameters, 2, nullptr, nullptr, 0);
    th_member option_methods[] = {
        {.name = CORE_STRING_STATIC("is_nil"), .type = is_nil_signature},
        {.name = CORE_STRING_STATIC("get"), .type = get_signature},
        {.name = CORE_STRING_STATIC("get_or_else"), .type = get_or_else_signature},
    };
    th_type *option_contracts = th_record(types, option_methods, 3);

    th_type *option_item = th_var(types);
    th_type *option_type = th_option(types, option_item);
    th_param builtin_is_nil_parameter = {.mode = TH_IN, .type = option_type};
    th_type *builtin_is_nil_signature = option_type && option_bool
        ? th_func_with_error(types, option_bool, th_name(types, CORE_STRING_STATIC("nil")),
            &builtin_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;

    th_type *get_item = th_var(types);
    th_type *get_option = th_option(types, get_item);
    th_param builtin_get_parameter = {.mode = TH_IN, .type = get_option};
    th_type *builtin_get_signature = get_option
        ? th_func_with_error(types, get_item, th_name(types, CORE_STRING_STATIC("nil")),
            &builtin_get_parameter, 1, nullptr, nullptr, 0) : nullptr;

    th_type *fallback_item = th_var(types);
    th_type *fallback_option = th_option(types, fallback_item);
    th_param builtin_get_or_else_parameters[] = {
        {.mode = TH_IN, .type = fallback_option},
        {.mode = TH_IN, .type = fallback_item},
    };
    th_type *builtin_get_or_else_signature = fallback_option
        ? th_func_with_error(types, fallback_item, th_name(types, CORE_STRING_STATIC("nil")),
            builtin_get_or_else_parameters, 2, nullptr, nullptr, 0) : nullptr;
    if (!option_contracts || !option_methods[0].type
        || !option_methods[1].type || !option_methods[2].type
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Option"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, option_contracts)
        || !builtin_is_nil_signature || !builtin_get_signature
        || !builtin_get_or_else_signature
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__option_is_nil"),
            CORE_STRING_STATIC("is_nil"), CORE_STRING_STATIC("builtins"),
            CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
            builtin_is_nil_signature)
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__option_get"),
            CORE_STRING_STATIC("get"), CORE_STRING_STATIC("builtins"),
            CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
            builtin_get_signature)
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__option_get_or_else"), CORE_STRING_STATIC("get_or_else"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
            builtin_get_or_else_signature))
        return false;

    th_type *read_receiver = th_var(types);
    th_type *read_index = th_var(types);
    th_type *read_result = th_var(types);
    th_type *read_error = th_var(types);
    th_param read_parameters[] = {
        {.mode = TH_IN, .type = read_receiver},
        {.mode = TH_IN, .type = read_index},
    };
    th_type *read_signature = th_func_with_error(types, read_result,
        read_error, read_parameters, 2, nullptr, nullptr, 0);
    th_member read_method = {.name = CORE_STRING_STATIC("__read"), .type = read_signature};
    th_type *read_trait_contract = read_signature
        ? th_record(types, &read_method, 1) : nullptr;
    th_type *read_item = th_var(types);
    th_type *read_array = th_array(types, read_item);
    th_type *array_read_index = n00bb_array_index_type(types, arena);
    th_param array_read_parameters[] = {
        {.mode = TH_IN, .type = read_array},
        {.mode = TH_IN, .type = array_read_index},
    };
    th_type *array_read_signature = read_array && array_read_index
        ? th_func_with_error(types, read_item, th_name(types, CORE_STRING_STATIC("nil")),
            array_read_parameters, 2, nullptr, nullptr, 0) : nullptr;
    if (!read_trait_contract || !array_read_signature
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Index_Read"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, read_trait_contract)
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__array_read"), CORE_STRING_STATIC("__read"), CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"),
            TH_ENV_PUBLIC, array_read_signature)) return false;

    th_type *slice_receiver = th_var(types);
    th_param slice_parameters[] = {
        {.mode = TH_IN, .type = slice_receiver},
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("int"))},
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("int"))},
    };
    th_type *slice_signature = th_func_with_error(types, th_var(types),
        th_var(types), slice_parameters, 3, nullptr, nullptr, 0);
    th_member slice_method = {
        .name = CORE_STRING_STATIC("__slice"), .type = slice_signature,
    };
    th_type *slice_contract = slice_signature
        ? th_record(types, &slice_method, 1) : nullptr;
    if (!slice_contract
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Index_Slice"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, slice_contract))
        return false;

    th_type *write_receiver = th_var(types);
    th_type *write_index = th_var(types);
    th_type *write_item = th_var(types);
    th_type *write_result = th_var(types);
    th_type *write_error = th_var(types);
    th_param write_parameters[] = {
        {.mode = TH_INOUT, .type = write_receiver},
        {.mode = TH_IN, .type = write_index},
        {.mode = TH_IN, .type = write_item},
    };
    th_type *write_signature = th_func_with_error(types, write_result,
        write_error, write_parameters, 3, nullptr, nullptr, 0);
    th_type *optional_bound = th_option(types, th_name(types, CORE_STRING_STATIC("int")));
    th_param slice_write_parameters[] = {
        {.mode = TH_INOUT, .type = write_receiver},
        {.mode = TH_IN, .type = optional_bound},
        {.mode = TH_IN, .type = optional_bound},
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("bool"))},
        {.mode = TH_IN, .type = th_var(types)},
    };
    th_type *slice_write_signature = optional_bound
        ? th_func_with_error(types, th_var(types), th_var(types),
            slice_write_parameters, 5, nullptr, nullptr, 0) : nullptr;
    th_member write_method = {.name = CORE_STRING_STATIC("__write"), .type = write_signature};
    th_member slice_write_method = {
        .name = CORE_STRING_STATIC("__write_slice"), .type = slice_write_signature,
    };
    th_type *write_contract = write_signature
        ? th_record(types, &write_method, 1) : nullptr;
    th_type *slice_write_contract = slice_write_signature
        ? th_record(types, &slice_write_method, 1) : nullptr;
    th_type *array_item = th_var(types);
    th_type *array_type = th_array(types, array_item);
    th_type *array_write_index = n00bb_array_index_type(types, arena);
    th_param array_write_parameters[] = {
        {.mode = TH_INOUT, .type = array_type},
        {.mode = TH_IN, .type = array_write_index},
        {.mode = TH_IN, .type = array_item},
    };
    th_type *array_write_signature = array_write_index
        ? th_func_with_error(types,
        th_name(types, CORE_STRING_STATIC("nil")), th_name(types, CORE_STRING_STATIC("nil")),
        array_write_parameters, 3, nullptr, nullptr, 0) : nullptr;
    if (!write_contract || !slice_write_contract
        || !array_type || !array_write_signature
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Index_Write"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, write_contract)
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Index_Slice_Write"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, slice_write_contract)
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__array_write"), CORE_STRING_STATIC("__write"), CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"),
            TH_ENV_PUBLIC, array_write_signature)) return false;

    th_type *list_element = th_var(types);
    th_type *array_element = th_var(types);
    th_type *set_element = th_var(types);
    th_type *dict_key = th_var(types);
    th_type *dict_value = th_var(types);
    th_arg list_arg = {.kind = TH_ARG_TYPE, .type = list_element};
    th_arg set_arg = {.kind = TH_ARG_TYPE, .type = set_element};
    th_arg dict_args[] = {
        {.kind = TH_ARG_TYPE, .type = dict_key},
        {.kind = TH_ARG_TYPE, .type = dict_value},
    };
    th_type *list = th_apply(types,
        CORE_STRING_STATIC("builtin.containers.list.List"), &list_arg, 1);
    th_type *set = th_apply(types,
        CORE_STRING_STATIC("builtin.containers.set.Set"), &set_arg, 1);
    th_type *dict = th_apply(types,
        CORE_STRING_STATIC("builtin.containers.dict.Dict"), dict_args, 2);
    return th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Sequence_Like"), list) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Sequence_Like"), th_array(types, array_element)) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Set_Like"), set) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Brace_Literal"), set) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Dict_Like"), dict) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Brace_Literal"), dict);
}
bool
n00bb_builtin_function(parse_sym_entry_t *symbol)
{
    return n00bb_builtin_native_function(symbol)
        || (symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && symbol->scope && !symbol->scope->parent
        && symbol->scope->ns
        && core_string_eq(symbol->scope->ns->ns_name,
                          CORE_STRING_STATIC("builtin"))
        && (core_string_eq(symbol->name, CORE_STRING_STATIC("has_type"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.builtin-operator"))
            || n00bb_builtin_option_function(symbol)
            || n00bb_builtin_array_read_function(symbol)
            || n00bb_builtin_array_write_function(symbol)
            || n00bb_builtin_reference_function(symbol)
            || n00bb_builtin_byte_offset_function(symbol)
            || n00bb_builtin_byte_diff_function(symbol)
            || n00bb_builtin_unsafe_arithmetic(symbol)
            || n00bb_builtin_bit_count_function(symbol)
            || n00bb_builtin_gc_memory_function(symbol)
            || n00bb_builtin_volatile_load_function(symbol)
            || n00bb_builtin_signal_fence_function(symbol)
            || n00bb_builtin_prefetch_function(symbol)
            || n00bb_builtin_atomic_rmw_function(symbol)
            || n00bb_builtin_atomic_cas_function(symbol)
            || n00bb_builtin_atomic_access_function(symbol)
            || n00bb_builtin_callback_function(symbol)
            || n00bb_builtin_init_function(symbol)));
}

bool
n00bb_builtin_native_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node
        && parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.abi.native"));
}

bool
n00bb_builtin_init_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.init"));
}

bool
n00bb_builtin_array_read_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.array.read"));
}

bool
n00bb_builtin_array_write_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.array.write"));
}

bool
n00bb_builtin_byte_offset_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.byte_offset"));
}

bool
n00bb_builtin_byte_diff_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.byte_diff"));
}

const n00bb_unsafe_arithmetic_t *
n00bb_builtin_unsafe_arithmetic(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION && !symbol->decl_node
        ? parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.integer.unsafe"))
        : nullptr;
}

bool
n00bb_builtin_bit_count_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.integer.bit_count"));
}

bool
n00bb_builtin_gc_memory_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.memory"));
}

bool
n00bb_builtin_volatile_load_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.volatile.load"));
}

bool
n00bb_builtin_signal_fence_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.signal_fence"));
}

bool
n00bb_builtin_prefetch_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.prefetch"));
}

bool
n00bb_builtin_atomic_rmw_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node
        && (parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_and"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or_release"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_or_relaxed"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.exchange"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.exchange_acquire"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.exchange_relaxed"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_add"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_add_relaxed"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.fetch_sub")));
}

bool
n00bb_builtin_atomic_cas_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.atomic.compare_exchange"));
}

bool
n00bb_builtin_atomic_access_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node
        && (parse_sym_binding(symbol, CORE_STRING_STATIC("n00bb.atomic.load"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.load_relaxed"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.load_acquire"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.store"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.store_relaxed"))
            || parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.atomic.store_release")));
}



