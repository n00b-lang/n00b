#include "compiler_tu.h"
#include "enum_value.h"

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

th_type *
n00bb_array_index_type(th_ctx *types, core_arena_t *arena)
{
    th_type *index = th_var(types);
    th_type **choices = arena && index
        ? core_arena_alloc(arena, 2 * sizeof(*choices)) : nullptr;
    if (!choices) return nullptr;
    choices[0] = th_name(types, "int");
    choices[1] = th_name(types, "uint");
    if (!choices[0] || !choices[1]
        || !th_var_constrain(types, index, (th_constraint){
            .kind = TH_CON_ONE_OF,
            .one_of = {.types = choices, .n = 2},
        })) return nullptr;
    return index;
}


static bool
register_builtin_comparison_symbols(parse_symtab_t *symtab,
                                    n00bb_tu_state_t *state,
                                    th_ctx *types)
{
    static const char *const names[] = {
        "less", "equal", "greater", "unordered",
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
    symbol->type = th_name(types, "Comparison");
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
            CORE_STRING_STATIC(""), core_string_from_cstr(names[i]),
            PARSE_SYM_ENUM_CONST, nullptr);
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

static bool
register_builtin_native_function(parse_symtab_t *symtab, th_ctx *types,
                                 th_type *nil_type, const char *name,
                                 const char *native_name, th_type *result,
                                 th_type *const *arguments, size_t count)
{
    th_param *parameters = count
        ? core_alloc_array(th_param, count) : nullptr;
    if (count && !parameters) return false;
    for (size_t i = 0; i < count; i++) {
        if (!arguments[i]) {
            core_free(parameters);
            return false;
        }
        parameters[i] = (th_param){.mode = TH_IN, .type = arguments[i]};
    }
    th_type *signature = result && nil_type
        ? th_func_with_error(types, result, nil_type, parameters,
                             count, nullptr, nullptr, 0) : nullptr;
    core_free(parameters);
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    parse_sym_entry_t *symbol = contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), core_string_from_cstr(name),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.native"),
            (void *)native_name)) return false;
    symbol->type = signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = contract;
    return true;
}

static bool
register_builtin_os_symbols(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *byte = th_name(types, "byte");
    th_type *u32 = th_name(types, "u32");
    th_type *u64 = th_name(types, "uint");
    th_type *uint = th_name(types, "uint");
    th_type *i32 = th_name(types, "i32");
    th_type *mapped_item = th_var(types);
    th_type *reference = mapped_item ? th_ind(types, 0, mapped_item) : nullptr;
    th_type *byte_reference = byte ? th_ind(types, 0, byte) : nullptr;
    th_arg option_arg = {.kind = TH_ARG_TYPE, .type = byte_reference};
    th_type *optional_reference = byte_reference
        ? th_apply(types, "option", &option_arg, 1) : nullptr;
    th_type *map_args[] = {uint, uint};
    th_type *unmap_args[] = {reference, uint};
    th_type *protect_args[] = {byte_reference, uint, u32};
    th_type *thread_context = th_name(types, "sys.internal.threads.Thread_Context");
    th_type *thread_context_ref = thread_context
        ? th_ind(types, 0, thread_context) : nullptr;
    th_type *suspend_args[] = {uint, thread_context_ref, u64};
    th_type *resume_args[] = {uint, thread_context_ref};
    th_type *capture_args[] = {thread_context_ref};
    return register_builtin_native_function(symtab, types, nil_type,
            "__os_page_size", "n00b_os_page_size", uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_yield", "n00b_os_thread_yield", nil_type,
            nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_fence", "n00b_os_thread_fence", nil_type,
            nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_id", "n00b_os_thread_id", uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_stack_high_self", "n00b_os_thread_stack_high_self",
            optional_reference, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_stack_red_zone_bytes", "n00b_os_stack_red_zone_bytes",
            uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_control_self", "n00b_os_thread_control_self",
            uint, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_capture_self", "n00b_os_thread_capture_self",
            nil_type, capture_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_suspend", "n00b_os_thread_suspend", i32,
            suspend_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_thread_resume", "n00b_os_thread_resume", i32,
            resume_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_now_ns", "n00b_os_now_ns", th_name(types, "uint"),
            nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_cpu_relax", "n00b_os_cpu_relax", nil_type,
            nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_map_zeroed", "n00b_os_map_zeroed",
            optional_reference, map_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_unmap", "n00b_os_unmap", i32, unmap_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            "__os_protect", "n00b_os_protect", i32, protect_args, 3);
}

static bool
register_builtin_abi_symbols(parse_symtab_t *symtab, th_ctx *types,
                             th_type *nil_type)
{
    th_type *u32 = th_name(types, "u32");
    th_type *i64 = th_name(types, "int");
    th_type *uint = th_name(types, "uint");
    th_type *typespec = th_name(types, "typespec");
    const char *record_names[] = {
        "sys.internal.abi.Type_Record",
        "sys.internal.abi.Field_Record",
        "sys.internal.abi.Trace_Record",
        "sys.internal.abi.Trace_Child_Record",
        "sys.internal.abi.Trace_Arm_Record",
        "sys.internal.abi.Tag_Extractor_Record",
        "sys.internal.abi.Frame_Layout_Record",
        "sys.internal.abi.Storage_Record",
    };
    const char *names[] = {
        "__abi_type", "__abi_field", "__abi_trace", "__abi_trace_child",
        "__abi_trace_arm", "__abi_tag_extractor",
        "__abi_frame_layout",
        "__abi_storage",
    };
    const char *native_names[] = {
        "n00b_builtin_abi_type", "n00b_builtin_abi_field",
        "n00b_builtin_abi_trace",
        "n00b_builtin_abi_trace_child",
        "n00b_builtin_abi_trace_arm", "n00b_builtin_abi_tag_extractor",
        "n00b_builtin_abi_frame_layout",
        "n00b_builtin_abi_storage",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        th_type *record = th_name(types, record_names[i]);
        th_type *reference = record ? th_ind(types, 0, record) : nullptr;
        th_arg option_arg = {.kind = TH_ARG_TYPE, .type = reference};
        th_type *result = reference
            ? th_apply(types, "option", &option_arg, 1) : nullptr;
        th_type *args[] = {u32};
        if (!register_builtin_native_function(symtab, types, nil_type,
                names[i], native_names[i], result, args, 1)) return false;
        if (i == 0) {
            th_type *descriptor_args[] = {typespec};
            if (!register_builtin_native_function(symtab, types, nil_type,
                    "__abi_type_for", "n00b_builtin_abi_type_for", result,
                    descriptor_args, 1)) return false;
        }
    }
    th_type *range_args[] = {u32, u32, u32};
    th_type *bytes_args[] = {u32, u32};
    th_type *byte_reference = th_ind(types, 0, th_name(types, "byte"));
    th_arg bytes_option_arg = {.kind = TH_ARG_TYPE, .type = byte_reference};
    th_type *bytes_result = byte_reference
        ? th_apply(types, "option", &bytes_option_arg, 1) : nullptr;
    th_type *native_frame = th_name(types,
        "sys.internal.abi.Native_Frame_Record");
    th_type *native_frame_reference = native_frame
        ? th_ind(types, 0, native_frame) : nullptr;
    th_arg frame_option_arg = {
        .kind = TH_ARG_TYPE, .type = native_frame_reference,
    };
    th_type *native_frame_result = native_frame_reference
        ? th_apply(types, "option", &frame_option_arg, 1) : nullptr;
    th_type *native_frame_args[] = {
        byte_reference, byte_reference, byte_reference,
    };
    th_type *native_offset_args[] = {native_frame_reference, u32};
    th_type *item = th_var(types);
    th_type *reference = item ? th_ind(types, 0, item) : nullptr;
    th_type *address_args[] = {reference};
    th_type *storage_args[] = {u32};
    return register_builtin_native_function(symtab, types, nil_type,
            "__abi_native_frame_at", "n00b_builtin_abi_native_frame_at",
            native_frame_result, native_frame_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_native_frame_root_offset",
            "n00b_builtin_abi_native_frame_root_offset",
            th_name(types, "i32"), native_offset_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_storage_count", "n00b_builtin_abi_storage_count", u32,
            nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_storage_address", "n00b_builtin_abi_storage_address",
            bytes_result, storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_string_backing_at", "n00b_builtin_abi_string_backing_at",
            bytes_result, &byte_reference, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_range_index", "n00b_builtin_abi_range_index", i64,
            range_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_bytes", "n00b_builtin_abi_bytes", bytes_result,
            bytes_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            "__abi_ref_address", "n00b_builtin_abi_ref_address", uint,
            address_args, 1);
}

static bool
register_builtin_wrapping_mul(parse_symtab_t *symtab, th_ctx *types,
                              th_type *nil_type)
{
    th_type *uint_type = th_name(types, "uint");
    th_param parameters[] = {
        {.mode = TH_IN, .type = uint_type},
        {.mode = TH_IN, .type = uint_type},
    };
    th_type *signature = uint_type && nil_type
        ? th_func_with_error(types, uint_type, nil_type, parameters, 2,
                             nullptr, nullptr, 0) : nullptr;
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    parse_sym_entry_t *symbol = contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("wrapping_mul"),
        PARSE_SYM_FUNCTION, nullptr) : nullptr;
    if (!symbol || !parse_sym_set_binding(symbol,
            CORE_STRING_STATIC("n00bb.integer.wrapping_mul"), symbol))
        return false;
    symbol->type = signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = contract;
    return true;
}

static bool
register_builtin_bit_counts(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *uint_type = th_name(types, "uint");
    th_type *u32_type = th_name(types, "u32");
    th_param parameter = {.mode = TH_IN, .type = uint_type};
    th_type *signature = uint_type && u32_type && nil_type
        ? th_func_with_error(types, u32_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr;
    th_type *contract = signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr;
    const char *names[] = {
        "__count_leading_zeros", "__count_trailing_zeros",
    };
    if (!contract) return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        parse_sym_entry_t *symbol = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"), core_string_from_cstr(names[i]),
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
    th_type *byte = th_name(types, "byte");
    th_type *uint = th_name(types, "uint");
    th_type *offset_type = th_name(types, "i128");
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

    th_type *difference_type = th_name(types, "int");
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
        ? th_apply(types, "atomic", &item_arg, 1) : nullptr;
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
    th_type *boolean = th_name(types, "bool");
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
    const char *ordered_access_names[] = {
        "__atomic_load_relaxed", "__atomic_load_acquire",
        "__atomic_store_relaxed", "__atomic_store_release",
    };
    const char *ordered_access_bindings[] = {
        "n00bb.atomic.load_relaxed", "n00bb.atomic.load_acquire",
        "n00bb.atomic.store_relaxed", "n00bb.atomic.store_release",
    };
    for (size_t i = 0; i < sizeof(ordered_access_names)
            / sizeof(ordered_access_names[0]); i++) {
        bool loading = i < 2;
        parse_sym_entry_t *ordered = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"),
            core_string_from_cstr(ordered_access_names[i]),
            PARSE_SYM_FUNCTION, nullptr);
        if (!ordered || !parse_sym_set_binding(ordered,
                core_string_from_cstr(ordered_access_bindings[i]), ordered))
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
    const char *atomic_rmw_names[] = {
        "__atomic_fetch_or", "__atomic_fetch_or_release",
        "__atomic_fetch_or_relaxed",
        "__atomic_fetch_and",
        "__atomic_fetch_add", "__atomic_fetch_add_relaxed",
        "__atomic_fetch_sub",
        "__atomic_exchange_acquire", "__atomic_exchange_relaxed",
    };
    const char *atomic_rmw_bindings[] = {
        "n00bb.atomic.fetch_or", "n00bb.atomic.fetch_or_release",
        "n00bb.atomic.fetch_or_relaxed",
        "n00bb.atomic.fetch_and",
        "n00bb.atomic.fetch_add", "n00bb.atomic.fetch_add_relaxed",
        "n00bb.atomic.fetch_sub",
        "n00bb.atomic.exchange_acquire", "n00bb.atomic.exchange_relaxed",
    };
    for (size_t i = 0;
         i < sizeof(atomic_rmw_names)
             / sizeof(atomic_rmw_names[0]); i++) {
        parse_sym_entry_t *rmw = parse_symtab_add(symtab,
            CORE_STRING_STATIC("builtin"),
            core_string_from_cstr(atomic_rmw_names[i]),
            PARSE_SYM_FUNCTION, nullptr);
        if (!rmw || !parse_sym_set_binding(rmw,
                core_string_from_cstr(atomic_rmw_bindings[i]),
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
        {.mode = TH_IN, .type = th_name(types, "u32")},
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
            "n00b_gc_finalize")) return false;
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
    th_type *spec_type = th_name(types, "typespec");
    th_type *bool_type = th_name(types, "bool");
    th_type *nil_type = th_name(types, "nil");
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
    th_arg option_is_nil_item_arg = {
        .kind = TH_ARG_TYPE, .type = option_is_nil_item,
    };
    th_type *option_is_nil_type = option_is_nil_item
        ? th_apply(types, "option", &option_is_nil_item_arg, 1) : nullptr;
    th_param option_is_nil_parameter = {
        .mode = TH_IN, .type = option_is_nil_type,
    };
    th_type *option_is_nil_signature = option_is_nil_type && bool_type
        ? th_func_with_error(types, bool_type, nil_type,
            &option_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_type *option_get_item = th_var(types);
    th_arg option_get_item_arg = {
        .kind = TH_ARG_TYPE, .type = option_get_item,
    };
    th_type *option_get_type = option_get_item
        ? th_apply(types, "option", &option_get_item_arg, 1) : nullptr;
    th_param option_get_parameter = {.mode = TH_IN, .type = option_get_type};
    th_type *option_get_signature = option_get_type
        ? th_func_with_error(types, option_get_item, nil_type,
            &option_get_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_type *option_default_item = th_var(types);
    th_arg option_default_item_arg = {
        .kind = TH_ARG_TYPE, .type = option_default_item,
    };
    th_type *option_default_type = option_default_item
        ? th_apply(types, "option", &option_default_item_arg, 1) : nullptr;
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
    th_type *string_type = th_name(types, "string");
    th_type *i32_type = th_name(types, "i32");
    th_param fd_write_parameters[] = {
        {.mode = TH_IN, .type = i32_type},
        {.mode = TH_IN, .type = string_type},
    };
    th_type *fd_write_signature = i32_type && string_type
        ? th_func_with_error(types, i32_type, nil_type,
            fd_write_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *fd_write_contract = fd_write_signature
        ? th_env_signature_contract(types, fd_write_signature, nil_type)
        : nullptr;
    th_type *repr_item = th_var(types);
    th_param repr_parameters[] = {
        {.mode = TH_IN, .type = repr_item},
        {.mode = TH_IN, .type = i32_type},
    };
    th_type *repr_signature = repr_item && i32_type
        ? th_func_with_error(types, i32_type, nil_type, repr_parameters,
                             2, nullptr, nullptr, 0) : nullptr;
    th_type *repr_contract = repr_signature
        ? th_env_signature_contract(types, repr_signature, nil_type)
        : nullptr;
    th_param string_add_parameters[] = {
        {.mode = TH_IN, .type = string_type},
        {.mode = TH_IN, .type = string_type},
    };
    th_type *string_add_signature = string_type
        ? th_func_with_error(types, string_type, nil_type,
            string_add_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *string_add_contract = string_add_signature
        ? th_env_signature_contract(types, string_add_signature, nil_type)
        : nullptr;
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
        || !option_get_or_else_contract || !string_add_contract
        || !fd_write_contract || !repr_contract || !array_read_contract
        || !array_write_contract)
        return false;

    parse_scope_t *saved = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             values->root);
    static const char *const comparison_traits[] = {"Equal", "Comparable"};
    for (size_t i = 0; i < sizeof(comparison_traits)
             / sizeof(comparison_traits[0]); i++) {
        core_string_t name = core_string_from_cstr(comparison_traits[i]);
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
    parse_sym_entry_t *string_add_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__string_add"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *array_write_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__array_write"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *array_read_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__array_read"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *fd_write_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__fd_write"),
        PARSE_SYM_FUNCTION, nullptr);
    parse_sym_entry_t *repr_symbol = parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), CORE_STRING_STATIC("__repr_default"),
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
    bool wrapping_mul_registered = register_builtin_wrapping_mul(symtab, types,
                                                                  nil_type);
    bool bit_counts_registered = register_builtin_bit_counts(symtab, types,
                                                              nil_type);
    th_type *array_length_item = th_var(types);
    th_type *array_length_type = array_length_item
        ? th_array(types, array_length_item) : nullptr;
    th_type *array_length_arguments[] = {array_length_type};
    bool array_length_registered = register_builtin_native_function(symtab,
        types, nil_type, "__array_length", "__n00bb_array_length",
        th_name(types, "uint"), array_length_arguments, 1);
    th_type *array_prefix_arguments[] = {
        array_length_type, th_name(types, "uint")};
    bool array_prefix_registered = register_builtin_native_function(symtab,
        types, nil_type, "__array_prefix", "__n00bb_array_prefix",
        array_length_type, array_prefix_arguments, 2);
    th_type *array_reference = array_length_type
        ? th_ind(types, 0, array_length_type) : nullptr;
    th_type *array_clear_arguments[] = {
        array_reference, th_name(types, "uint")};
    bool array_clear_registered = register_builtin_native_function(symtab,
        types, nil_type, "__array_clear", "__n00bb_array_clear",
        nil_type, array_clear_arguments, 2);
    th_type *hash_arguments[] = {th_array(types, th_name(types, "byte"))};
    bool hash_registered = register_builtin_native_function(symtab, types,
        nil_type, "__xxh3_128", "__n00bb_xxh3_128",
        th_name(types, "u128"), hash_arguments, 1);
    th_type *hash_item = th_var(types);
    th_type *scalar_hash_arguments[] = {hash_item};
    bool scalar_hash_registered = register_builtin_native_function(symtab,
        types, nil_type, "__hash_scalar", "__n00bb_hash_scalar",
        th_name(types, "u128"), scalar_hash_arguments, 1);
    th_type *string_hash_arguments[] = {th_name(types, "string")};
    bool string_hash_registered = register_builtin_native_function(symtab,
        types, nil_type, "__hash_string", "__n00bb_hash_string",
        th_name(types, "u128"), string_hash_arguments, 1);
    if (saved)
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                                 saved);
    if (!symbol || !read_symbol || !write_symbol || !option_is_nil_symbol
        || !option_get_symbol || !option_get_or_else_symbol
        || !string_add_symbol || !array_read_symbol || !array_write_symbol
        || !fd_write_symbol || !repr_symbol
        || !init_symbol || !init_contract
        || !os_builtins_registered || !abi_builtins_registered
        || !gc_builtins_registered || !wrapping_mul_registered
        || !bit_counts_registered || !array_length_registered
        || !array_prefix_registered || !array_clear_registered
        || !hash_registered || !scalar_hash_registered
        || !string_hash_registered) return false;
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
    string_add_symbol->type = string_add_signature;
    string_add_symbol->error_type = nil_type;
    string_add_symbol->callable_contract = string_add_contract;
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
    fd_write_symbol->type = fd_write_signature;
    fd_write_symbol->error_type = nil_type;
    fd_write_symbol->callable_contract = fd_write_contract;
    if (!parse_sym_set_binding(fd_write_symbol,
            CORE_STRING_STATIC("n00bb.abi.fd_write"),
            (void *)"n00bb_bootstrap_fd_write")) return false;
    repr_symbol->type = repr_signature;
    repr_symbol->error_type = nil_type;
    repr_symbol->callable_contract = repr_contract;
    if (!parse_sym_set_binding(repr_symbol,
            CORE_STRING_STATIC("n00bb.abi.repr_default"),
            (void *)"n00bb_bootstrap_repr_default")) return false;
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
        && string_add_symbol->callable_contract
        && array_read_symbol->callable_contract
        && array_write_symbol->callable_contract
        && fd_write_symbol->callable_contract
        && repr_symbol->callable_contract
        && init_symbol->callable_contract;
}

static bool
register_builtin_binary_operators(th_env *environment, th_ctx *types)
{
    static const struct {
        const char *trait;
        const char *method;
        bool integer_only;
        bool shift;
    } operators[] = {
        {"Add", "__add", false, false},
        {"Subtract", "__subtract", false, false},
        {"Multiply", "__multiply", false, false},
        {"Divide", "__divide", false, false},
        {"Remainder", "__remainder", false, false},
        {"Bit_And", "__bit_and", true, false},
        {"Bit_Or", "__bit_or", true, false},
        {"Bit_Xor", "__bit_xor", true, false},
        {"Shift_Left", "__shift_left", true, true},
        {"Shift_Right", "__shift_right", true, true},
    };
    static const char *const numeric_types[] = {
        "int", "uint", "i8", "i16", "i32", "i128",
        "u8", "u16", "u32", "u128", "float", "f64",
    };
    for (size_t o = 0; o < sizeof(operators) / sizeof(operators[0]); o++) {
        th_type *receiver = th_var(types);
        th_type *argument = th_var(types);
        th_type *result = th_var(types);
        th_param contract_params[] = {
            {.mode = TH_IN, .type = receiver},
            {.mode = TH_IN, .type = argument},
        };
        th_type *contract = th_func_with_error(types, result,
            th_var(types), contract_params, 2, nullptr, nullptr, 0);
        th_member method = {
            .name = operators[o].method,
            .type = contract,
        };
        th_type *methods = method.type ? th_record(types, &method, 1) : nullptr;
        if (!methods || !th_env_register_trait_contracts(environment,
                operators[o].trait, "builtins", "default", TH_ENV_PUBLIC,
                methods)) return false;
        for (size_t n = 0; n < sizeof(numeric_types) / sizeof(numeric_types[0]); n++) {
            const char *name = numeric_types[n];
            if (operators[o].integer_only && (name[0] == 'f')) continue;
            th_type *self = th_name(types, name);
            th_type *other = operators[o].shift ? th_name(types, "int") : self;
            th_param parameters[] = {
                {.mode = TH_IN, .type = self},
                {.mode = TH_IN, .type = other},
            };
            th_type *signature = th_func_with_error(types, self,
                th_name(types, "nil"), parameters, 2, nullptr, nullptr, 0);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "builtins.%s.%s.%s",
                               operators[o].trait, name, operators[o].method);
            bool registered = signature && qualified.data
                && th_env_register_function(environment, qualified.data,
                    operators[o].method, "builtins", "default",
                    TH_ENV_PUBLIC, signature);
            core_buffer_release(&qualified);
            if (!registered) return false;
        }
        if (!strcmp(operators[o].trait, "Add")) {
            th_type *string = th_name(types, "string");
            th_param parameters[] = {
                {.mode = TH_IN, .type = string},
                {.mode = TH_IN, .type = string},
            };
            th_type *signature = th_func_with_error(types, string,
                th_name(types, "nil"), parameters, 2, nullptr, nullptr, 0);
            if (!signature || !th_env_register_function(environment,
                    "builtins.__string_add", "__add", "builtins",
                    "default", TH_ENV_PUBLIC, signature)) return false;
        }
    }
    return true;
}

static bool
register_builtin_unary_operators(th_env *environment, th_ctx *types)
{
    static const char *const numeric_types[] = {
        "int", "uint", "i8", "i16", "i32", "i128",
        "u8", "u16", "u32", "u128", "float", "f64",
    };
    static const struct {
        const char *trait;
        const char *method;
        bool boolean;
    } operators[] = {
        {"Negate", "__negate", false},
        {"Not", "__not", true},
    };
    for (size_t o = 0; o < sizeof(operators) / sizeof(operators[0]); o++) {
        th_type *receiver = th_var(types);
        th_type *result = operators[o].boolean
            ? th_name(types, "bool") : th_var(types);
        th_param contract_param = {.mode = TH_IN, .type = receiver};
        th_type *contract = th_func_with_error(types, result,
            th_var(types), &contract_param, 1, nullptr, nullptr, 0);
        th_member method = {
            .name = operators[o].method,
            .type = contract,
        };
        th_type *methods = method.type ? th_record(types, &method, 1) : nullptr;
        if (!methods || !th_env_register_trait_contracts(environment,
                operators[o].trait, "builtins", "default", TH_ENV_PUBLIC,
                methods)) return false;
        size_t count = operators[o].boolean ? 1
            : sizeof(numeric_types) / sizeof(numeric_types[0]);
        for (size_t n = 0; n < count; n++) {
            const char *name = operators[o].boolean
                ? "bool" : numeric_types[n];
            th_type *self = th_name(types, name);
            th_param parameter = {.mode = TH_IN, .type = self};
            th_type *signature = th_func_with_error(types, self,
                th_name(types, "nil"), &parameter, 1, nullptr, nullptr, 0);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "builtins.%s.%s.%s",
                               operators[o].trait, name,
                               operators[o].method);
            bool registered = signature && qualified.data
                && th_env_register_function(environment, qualified.data,
                    operators[o].method, "builtins", "default",
                    TH_ENV_PUBLIC, signature);
            core_buffer_release(&qualified);
            if (!registered) return false;
        }
    }
    return true;
}

static bool
register_builtin_comparison_operators(th_env *environment, th_ctx *types)
{
    static const char *const ordered_types[] = {
        "int", "uint", "i8", "i16", "i32", "i128",
        "u8", "u16", "u32", "u128", "float", "f64",
        "string",
    };
    static const char *const equality_types[] = {
        "bool", "char", "byte", "typespec",
    };
    static const struct {
        const char *trait;
        const char *method;
        const char *result;
    } operators[] = {
        {"Equal", "__equal", "bool"},
        {"Comparable", "__cmp", "Comparison"},
    };
    th_type *comparison = th_name(types, "Comparison");
    th_member members[] = {
        {.name = "less", .type = comparison},
        {.name = "equal", .type = comparison},
        {.name = "greater", .type = comparison},
        {.name = "unordered", .type = comparison},
    };
    th_type *shape = comparison ? th_record(types, members,
        sizeof(members) / sizeof(members[0])) : nullptr;
    if (!shape || !th_env_register_type(environment, "Comparison",
            "builtins", "default", TH_ENV_PUBLIC)
        || !th_env_register_type_shape(environment, "Comparison",
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
            th_name(types, "nil"), parameters, 2, nullptr, nullptr, 0);
        th_member method = {
            .name = operators[o].method,
            .type = contract,
        };
        th_type *methods = method.type
            ? th_trait(types, operators[o].trait, &method, 1, nullptr, 0)
            : nullptr;
        if (!methods || !th_env_register_trait_contracts(environment,
                operators[o].trait, "builtins", "default", TH_ENV_PUBLIC,
                methods)) return false;
        size_t ordered_count = sizeof(ordered_types) / sizeof(ordered_types[0]);
        size_t equality_count = !strcmp(operators[o].trait, "Equal")
            ? sizeof(equality_types) / sizeof(equality_types[0]) : 0;
        for (size_t n = 0; n <= ordered_count + equality_count; n++) {
            bool sized_family = n == ordered_count + equality_count;
            const char *name = sized_family ? "sized_integer"
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
                th_name(types, "nil"), primitive_params, 2,
                nullptr, nullptr, 0);
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "builtins.%s.%s.%s",
                operators[o].trait, name, operators[o].method);
            bool registered = signature && qualified.data
                && th_env_register_function(environment, qualified.data,
                    operators[o].method, "builtins", "default",
                    TH_ENV_PUBLIC, signature);
            core_buffer_release(&qualified);
            if (!registered) return false;
        }
        if (!strcmp(operators[o].trait, "Equal")) {
            th_type *referent = th_var(types);
            th_type *reference = referent
                ? th_ind(types, 0, referent) : nullptr;
            th_param ref_params[] = {
                {.mode = TH_IN, .type = reference},
                {.mode = TH_IN, .type = reference},
            };
            th_type *signature = reference
                ? th_func_with_error(types, result,
                    th_name(types, "nil"), ref_params, 2,
                    nullptr, nullptr, 0) : nullptr;
            if (!signature || !th_env_register_function(environment,
                    "builtins.Equal.reference.__equal", "__equal",
                    "builtins", "default", TH_ENV_PUBLIC, signature))
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
        !th_env_register_trait_contracts(environment, "Bracket_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, "Brace_Literal",
            "builtins", "default", TH_ENV_PUBLIC, methods))
        return false;
    th_type *callback_referent = th_var(types);
    th_type *callback_reference = callback_referent
        ? th_ind(types, 0, callback_referent) : nullptr;
    if (!callback_referent || !callback_reference
        || !th_var_constrain(types, callback_referent,
            (th_constraint){.kind = TH_CON_KIND,
                            .type_kind = TH_FUNCTION})
        || !th_env_register_trait_contracts(environment, "Callback",
            "builtins", "default", TH_ENV_PUBLIC, methods)
        || !th_env_register_intrinsic_type_pattern(environment, "Callback",
                                                    callback_reference))
        return false;
    th_type *finalizer_receiver = th_var(types);
    th_param finalizer_parameter = {
        .mode = TH_CONSUME, .type = finalizer_receiver,
    };
    th_type *finalizer_signature = finalizer_receiver
        ? th_func_with_error(types, th_name(types, "nil"),
            th_name(types, "nil"), &finalizer_parameter, 1,
            nullptr, nullptr, 0) : nullptr;
    th_member finalizer_method = {
        .name = "__finalize", .type = finalizer_signature,
    };
    th_type *finalizer_contract = finalizer_signature
        ? th_record(types, &finalizer_method, 1) : nullptr;
    if (!finalizer_contract
        || !th_env_register_trait_contracts(environment, "Finalize",
            "builtins", "default", TH_ENV_PUBLIC, finalizer_contract))
        return false;
    if (!register_builtin_binary_operators(environment, types)) return false;
    if (!register_builtin_unary_operators(environment, types)) return false;
    if (!register_builtin_comparison_operators(environment, types)) return false;
    th_type *source = th_var(types);
    th_type *cursor = th_var(types);
    th_type *iter_error = th_var(types);
    th_param iter_param = {.mode = TH_IN, .type = source};
    th_type *iter_signature = th_func_with_error(types, cursor, iter_error,
        &iter_param, 1, nullptr, nullptr, 0);
    th_member iter_method = {
        .name = "iter",
        .type = iter_signature,
    };
    th_type *iter_contracts = iter_method.type
        ? th_record(types, &iter_method, 1) : nullptr;

    th_type *iterator = th_var(types);
    th_type *item = th_var(types);
    th_type *next_error = th_var(types);
    th_arg item_arg = {.kind = TH_ARG_TYPE, .type = item};
    th_type *optional_item = th_apply(types, "option", &item_arg, 1);
    th_param next_param = {.mode = TH_INOUT, .type = iterator};
    th_type *next_signature = optional_item
        ? th_func_with_error(types, optional_item, next_error, &next_param,
            1, nullptr, nullptr, 0) : nullptr;
    th_member next_method = {
        .name = "next",
        .type = next_signature,
    };
    th_param cleanup_param = {.mode = TH_CONSUME, .type = iterator};
    th_type *cleanup_signature = th_func_with_error(types,
        th_name(types, "nil"), th_name(types, "nil"), &cleanup_param, 1,
        nullptr, nullptr, 0);
    th_member iterator_methods[] = {
        next_method,
        {.name = "cleanup", .type = cleanup_signature},
    };
    th_type *next_contracts = next_method.type && cleanup_signature
        ? th_record(types, iterator_methods, 2) : nullptr;
    if (!iter_contracts || !next_contracts
        || !th_env_register_trait_contracts(environment, "Iterable",
            "builtins", "default", TH_ENV_PUBLIC, iter_contracts)
        || !th_env_register_trait_contracts(environment, "Iterator",
            "builtins", "default", TH_ENV_PUBLIC, next_contracts))
        return false;

    th_type *option_receiver = th_var(types);
    th_type *option_result = th_var(types);
    th_type *option_bool = th_name(types, "bool");
    th_param is_nil_parameter = {.mode = TH_IN, .type = option_receiver};
    th_type *is_nil_signature = option_bool
        ? th_func_with_error(types, option_bool, th_name(types, "nil"),
            &is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
    th_param get_parameter = {.mode = TH_IN, .type = option_receiver};
    th_type *get_signature = th_func_with_error(types, option_result,
        th_name(types, "nil"), &get_parameter, 1, nullptr, nullptr, 0);
    th_type *option_fallback_receiver = th_var(types);
    th_type *option_fallback_result = th_var(types);
    th_param get_or_else_parameters[] = {
        {.mode = TH_IN, .type = option_fallback_receiver},
        {.mode = TH_IN, .type = option_fallback_result},
    };
    th_type *get_or_else_signature = th_func_with_error(types,
        option_fallback_result, th_name(types, "nil"),
        get_or_else_parameters, 2, nullptr, nullptr, 0);
    th_member option_methods[] = {
        {.name = "is_nil", .type = is_nil_signature},
        {.name = "get", .type = get_signature},
        {.name = "get_or_else", .type = get_or_else_signature},
    };
    th_type *option_contracts = th_record(types, option_methods, 3);

    th_type *option_item = th_var(types);
    th_arg option_item_arg = {.kind = TH_ARG_TYPE, .type = option_item};
    th_type *option_type = th_apply(types, "option", &option_item_arg, 1);
    th_param builtin_is_nil_parameter = {.mode = TH_IN, .type = option_type};
    th_type *builtin_is_nil_signature = option_type && option_bool
        ? th_func_with_error(types, option_bool, th_name(types, "nil"),
            &builtin_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;

    th_type *get_item = th_var(types);
    th_arg get_item_arg = {.kind = TH_ARG_TYPE, .type = get_item};
    th_type *get_option = th_apply(types, "option", &get_item_arg, 1);
    th_param builtin_get_parameter = {.mode = TH_IN, .type = get_option};
    th_type *builtin_get_signature = get_option
        ? th_func_with_error(types, get_item, th_name(types, "nil"),
            &builtin_get_parameter, 1, nullptr, nullptr, 0) : nullptr;

    th_type *fallback_item = th_var(types);
    th_arg fallback_item_arg = {.kind = TH_ARG_TYPE, .type = fallback_item};
    th_type *fallback_option = th_apply(types, "option", &fallback_item_arg, 1);
    th_param builtin_get_or_else_parameters[] = {
        {.mode = TH_IN, .type = fallback_option},
        {.mode = TH_IN, .type = fallback_item},
    };
    th_type *builtin_get_or_else_signature = fallback_option
        ? th_func_with_error(types, fallback_item, th_name(types, "nil"),
            builtin_get_or_else_parameters, 2, nullptr, nullptr, 0) : nullptr;
    if (!option_contracts || !option_methods[0].type
        || !option_methods[1].type || !option_methods[2].type
        || !th_env_register_trait_contracts(environment, "Option",
            "builtins", "default", TH_ENV_PUBLIC, option_contracts)
        || !builtin_is_nil_signature || !builtin_get_signature
        || !builtin_get_or_else_signature
        || !th_env_register_function(environment,
            "builtins.__option_is_nil", "is_nil", "builtins",
            "default", TH_ENV_PUBLIC, builtin_is_nil_signature)
        || !th_env_register_function(environment,
            "builtins.__option_get", "get", "builtins",
            "default", TH_ENV_PUBLIC, builtin_get_signature)
        || !th_env_register_function(environment,
            "builtins.__option_get_or_else", "get_or_else",
            "builtins", "default", TH_ENV_PUBLIC,
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
    th_member read_method = {.name = "__read", .type = read_signature};
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
        ? th_func_with_error(types, read_item, th_name(types, "nil"),
            array_read_parameters, 2, nullptr, nullptr, 0) : nullptr;
    if (!read_trait_contract || !array_read_signature
        || !th_env_register_trait_contracts(environment, "Index_Read",
            "builtins", "default", TH_ENV_PUBLIC, read_trait_contract)
        || !th_env_register_function(environment,
            "builtins.__array_read", "__read", "builtins", "default",
            TH_ENV_PUBLIC, array_read_signature)) return false;

    th_type *slice_receiver = th_var(types);
    th_param slice_parameters[] = {
        {.mode = TH_IN, .type = slice_receiver},
        {.mode = TH_IN, .type = th_name(types, "int")},
        {.mode = TH_IN, .type = th_name(types, "int")},
    };
    th_type *slice_signature = th_func_with_error(types, th_var(types),
        th_var(types), slice_parameters, 3, nullptr, nullptr, 0);
    th_member slice_method = {
        .name = "__slice", .type = slice_signature,
    };
    th_type *slice_contract = slice_signature
        ? th_record(types, &slice_method, 1) : nullptr;
    if (!slice_contract
        || !th_env_register_trait_contracts(environment, "Index_Slice",
            "builtins", "default", TH_ENV_PUBLIC, slice_contract))
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
    th_arg bound_arg = {.kind = TH_ARG_TYPE, .type = th_name(types, "int")};
    th_type *optional_bound = th_apply(types, "option", &bound_arg, 1);
    th_param slice_write_parameters[] = {
        {.mode = TH_INOUT, .type = write_receiver},
        {.mode = TH_IN, .type = optional_bound},
        {.mode = TH_IN, .type = optional_bound},
        {.mode = TH_IN, .type = th_name(types, "bool")},
        {.mode = TH_IN, .type = th_var(types)},
    };
    th_type *slice_write_signature = optional_bound
        ? th_func_with_error(types, th_var(types), th_var(types),
            slice_write_parameters, 5, nullptr, nullptr, 0) : nullptr;
    th_member write_method = {.name = "__write", .type = write_signature};
    th_member slice_write_method = {
        .name = "__write_slice", .type = slice_write_signature,
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
        th_name(types, "nil"), th_name(types, "nil"),
        array_write_parameters, 3, nullptr, nullptr, 0) : nullptr;
    if (!write_contract || !slice_write_contract
        || !array_type || !array_write_signature
        || !th_env_register_trait_contracts(environment, "Index_Write",
            "builtins", "default", TH_ENV_PUBLIC, write_contract)
        || !th_env_register_trait_contracts(environment, "Index_Slice_Write",
            "builtins", "default", TH_ENV_PUBLIC, slice_write_contract)
        || !th_env_register_function(environment,
            "builtins.__array_write", "__write", "builtins", "default",
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
    return th_env_register_intrinsic_type_pattern(environment,
               "Bracket_Literal", th_apply(types, "builtin.containers.list.List", &list_arg, 1)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Bracket_Literal", th_array(types, array_element)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Brace_Literal", th_apply(types, "builtin.containers.set.Set", &set_arg, 1)) &&
           th_env_register_intrinsic_type_pattern(environment,
               "Brace_Literal", th_apply(types, "builtin.containers.dict.Dict", dict_args, 2));
}
bool
n00bb_builtin_function(parse_sym_entry_t *symbol)
{
    return n00bb_builtin_fd_write_function(symbol)
        || n00bb_builtin_repr_default_function(symbol)
        || n00bb_builtin_native_function(symbol)
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
            || n00bb_builtin_string_operator(symbol)
            || n00bb_builtin_reference_function(symbol)
            || n00bb_builtin_byte_offset_function(symbol)
            || n00bb_builtin_byte_diff_function(symbol)
            || n00bb_builtin_wrapping_mul_function(symbol)
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
n00bb_builtin_fd_write_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node
        && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.fd_write"));
}

bool
n00bb_builtin_repr_default_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node
        && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.abi.repr_default"));
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
n00bb_builtin_string_operator(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && symbol->scope && !symbol->scope->parent
        && symbol->scope->ns
        && core_string_eq(symbol->scope->ns->ns_name,
                          CORE_STRING_STATIC("builtin"))
        && core_string_eq(symbol->name,
                          CORE_STRING_STATIC("__string_add"));
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

bool
n00bb_builtin_wrapping_mul_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.integer.wrapping_mul"));
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



