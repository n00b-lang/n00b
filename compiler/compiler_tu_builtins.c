#include "compiler_axioms.h"
#include "compiler_tu.h"
#include "enum_value.h"
#include "ir/summary.h"
#include "qualified_names.h"

static bool
n00bb_builtin_init_function(parse_sym_entry_t *symbol);

const n00bb_axiom_t *
n00bb_axiom_of_symbol(parse_sym_entry_t *symbol)
{
    if (!symbol) return nullptr;
    if (n00bb_builtin_function(symbol)) {
        const n00bb_axiom_t *builtin = n00bb_axiom_of_builtin(symbol->name);
        if (builtin) return builtin;
    }
    size_t count = 0;
    const n00bb_axiom_t *const *bindings = n00bb_axiom_bindings(&count);
    for (size_t i = 0; i < count; i++)
        if (parse_sym_binding(symbol, bindings[i]->name)) return bindings[i];
    return nullptr;
}

static void
native_flows(ir_summary_t *summary, const ir_func_t *function,
             uint32_t effects, uint32_t reads, uint32_t writes)
{
    core_list_t(ir_spath_t) inputs = core_list_new(ir_spath_t);
    uint32_t params = (uint32_t)th_func_nparams(function->type);
    for (uint32_t p = 0; p < params; p++) {
        core_list_push(inputs, ((ir_spath_t){
            .root = IR_SPLACE_PARAM, .param = p}));
        if (p >= 8 || (reads & N00BB_AXIOM_PARAM(p)))
            core_list_push(inputs, ((ir_spath_t){
                .root = IR_SPLACE_PARAM, .param = p,
                .flags = IR_SPATH_DEEP}));
    }
    for (uint32_t bit = 0; bit < 32; bit++)
        if (effects & IR_SUMMARY_OBSERVES & (1u << bit))
            core_list_push(inputs, ((ir_spath_t){
                .root = IR_SPLACE_SOURCE, .param = bit}));
    for (size_t i = 0; i < inputs.len; i++) {
        ir_summary_add_flow(summary, IR_SFLOW_RESULTS, &(ir_spath_t){0},
                            &inputs.data[i], IR_SFLOW_DATA);
        for (uint32_t p = 0; p < params && p < 8; p++)
            if (writes & N00BB_AXIOM_PARAM(p))
                ir_summary_add_flow(summary, IR_SFLOW_STORES, &(ir_spath_t){
                        .root = IR_SPLACE_PARAM, .param = p,
                        .flags = IR_SPATH_DEEP},
                    &inputs.data[i], IR_SFLOW_DATA);
    }
    core_list_free(inputs);
}

ir_summary_t *
n00bb_native_axiom(ir_summaries_t *summaries, const ir_func_t *function)
{
    if (!function || function->n_blocks) return nullptr;
    const n00bb_axiom_t *native = n00bb_axiom_of_function(function);
    if (native && native->key == N00BB_AXIOM_NATIVE) {
        ir_summary_t *summary = ir_summary_new(summaries, function);
        ir_summary_set_flags(summary, native->effects
            | (native->collection_free ? 0 : IR_SUMMARY_MAY_COLLECT));
        if (native->effects & IR_SUMMARY_MAY_TRAP)
            ir_summary_set_trap_kinds(summary, IR_STRAP_ALL);
        for (uint32_t p = 0; p < 8; p++) {
            uint8_t flags = 0;
            if (native->reads & N00BB_AXIOM_PARAM(p))
                flags |= IR_SPATH_READ | IR_SPATH_READS_ENTRY;
            if (native->writes & N00BB_AXIOM_PARAM(p))
                flags |= IR_SPATH_WRITE;
            if (flags)
                ir_summary_add_path(summary, &(ir_spath_t){
                    .root = IR_SPLACE_PARAM, .param = p,
                    .flags = flags | IR_SPATH_DEEP | IR_SPATH_ROOT});
        }
        if (native->effects & IR_SUMMARY_CALLS_UNKNOWN)
            ir_summary_add_path(summary, &(ir_spath_t){
                .root = IR_SPLACE_UNKNOWN,
                .flags = IR_SPATH_ACCESS | IR_SPATH_DEEP});
        for (uint32_t p = 0; (native->effects & N00BB_AXIOM_ENTRY_FLAGS)
             && p < th_func_nparams(function->type); p++)
            ir_summary_set_param_escape(summary, p,
                IR_SESCAPE_ADDRESS | IR_SESCAPE_REFERENT);
        ir_summary_set_return(summary,
            (ir_sreturn_t){.kind = native->returns, .param = 0});
        for (uint32_t p = 0; p < 8; p++)
            if (native->holds & N00BB_AXIOM_PARAM(p))
                ir_summary_add_flow(summary, IR_SFLOW_RESULTS,
                    &(ir_spath_t){0},
                    &(ir_spath_t){.root = IR_SPLACE_PARAM, .param = p,
                                  .flags = IR_SPATH_DEEP},
                    IR_SFLOW_ALIAS);
        native_flows(summary, function, native->effects, native->reads,
                     native->writes);
        return summary;
    }
    uint32_t effects = 0;
    bool     foreign = n00bb_axiom_foreign_native(function, &effects);
    if (!foreign && !n00bb_axiom_pure_native(function)) return nullptr;
    ir_summary_t *summary = ir_summary_new(summaries, function);
    if (foreign) {
        ir_summary_set_flags(summary, effects);
        if (effects & IR_SUMMARY_MAY_TRAP)
            ir_summary_set_trap_kinds(summary, IR_STRAP_ALL);
    }
    uint16_t read_only = 0, write_only = 0;
    if (foreign) n00bb_axiom_foreign_access(function, &read_only, &write_only);
    uint32_t written = 0;
    for (uint32_t p = 0; p < th_func_nparams(function->type); p++) {
        bool reads = p >= 16 || !(write_only & (1u << p));
        bool writes = p >= 16 || !(read_only & (1u << p));
        if (reads)
            ir_summary_add_path(summary, &(ir_spath_t){
                .root = IR_SPLACE_PARAM, .param = p,
                .flags = IR_SPATH_READ | IR_SPATH_READS_ENTRY | IR_SPATH_DEEP
                    | IR_SPATH_ROOT});
        const th_param *formal = th_func_param(function->type, p);
        th_type *type = formal && formal->type ? th_resolve(formal->type)
                                               : nullptr;
        if (foreign && writes && type && th_kind_of(type) == TH_INDIRECTION) {
            th_type *referent = th_ind_referent(type);
            const th_builtin_type_t *builtin = referent
                ? th_builtin_of(referent) : nullptr;
            bool raw = referent
                && (th_kind_of(th_resolve(referent)) == TH_BITINT
                    || (builtin && builtin->scalar));
            bool fills = p < 16 && (write_only & (1u << p));
            ir_summary_add_path(summary, &(ir_spath_t){
                .root = IR_SPLACE_PARAM, .param = p,
                .flags = IR_SPATH_WRITE | IR_SPATH_ROOT
                    | (raw ? IR_SPATH_DEEP : 0)
                    | (fills ? IR_SPATH_MUST : 0)});
            if (raw && p < 8) written |= N00BB_AXIOM_PARAM(p);
        }
    }
    ir_summary_set_return(summary,
        (ir_sreturn_t){.kind = IR_SRETURN_UNKNOWN, .param = 0});
    native_flows(summary, function, effects, UINT32_MAX, written);
    return summary;
}

bool
n00bb_native_calls_n00b(const ir_func_t *function)
{
    const n00bb_axiom_t *native = n00bb_axiom_of_function(function);
    return native && native->key == N00BB_AXIOM_NATIVE
        && (native->effects & N00BB_AXIOM_ENTRY_FLAGS);
}

static const core_string_t *
builtin_native_name(core_string_t name)
{
    const n00bb_axiom_t *axiom = n00bb_axiom_of_native_name(name);
    return axiom ? &axiom->native : nullptr;
}

static th_type *
n00bb_array_index_type(th_ctx *types)
{
    return th_name(types, CORE_STRING_STATIC("int"));
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
        member->comptime_value = value;
        member->has_comptime_value = true;
        parse_sym_set_storage_duration(member,
                                       PARSE_SYM_STORAGE_COMPTIME);
    }
    if (ok) ok = n00bb_bind_enum_range(state->types, &state->semantic_values,
                                 symbol, range);
    parse_symtab_pop_scope_all(symtab);
    return ok;
}

typedef struct {
    th_type *signature;
    th_type *contract;
} builtin_fn_t;

static builtin_fn_t
builtin_fn(th_ctx *types, th_type *nil_type, th_type *signature)
{
    return (builtin_fn_t){signature, signature
        ? th_env_signature_contract(types, signature, nil_type) : nullptr};
}

static parse_sym_entry_t *
add_builtin_fn(parse_symtab_t *symtab, th_type *nil_type, core_string_t name,
               builtin_fn_t fn, core_string_t binding, const void *value)
{
    parse_sym_entry_t *symbol = fn.contract ? parse_symtab_add(symtab,
        CORE_STRING_STATIC("builtin"), name, PARSE_SYM_FUNCTION, nullptr)
        : nullptr;
    if (!symbol || (binding.data && !parse_sym_set_binding(symbol, binding,
            value ? (void *)value : symbol))) return nullptr;
    symbol->type = fn.signature;
    symbol->error_type = nil_type;
    symbol->callable_contract = fn.contract;
    return symbol;
}

static parse_sym_entry_t *
add_builtin(parse_symtab_t *symtab, th_ctx *types, th_type *nil_type,
            core_string_t name, th_type *signature, core_string_t binding,
            const void *value)
{
    return add_builtin_fn(symtab, nil_type, name,
        builtin_fn(types, nil_type, signature), binding, value);
}

static parse_sym_entry_t *
register_native_signature(parse_symtab_t *symtab, th_ctx *types,
                          th_type *nil_type, core_string_t name,
                          const core_string_t *native_name,
                          th_type *signature)
{
    return native_name ? add_builtin(symtab, types, nil_type, name, signature,
        CORE_STRING_STATIC("n00bb.abi.native"), native_name) : nullptr;
}

static th_type *
builtin_signature(th_ctx *types, th_type *nil_type, th_type *result,
                  th_type *const *arguments, size_t count)
{
    th_param parameters[8];
    if (!result || !nil_type || count > 8) return nullptr;
    for (size_t i = 0; i < count; i++) {
        if (!arguments[i]) return nullptr;
        parameters[i] = (th_param){.mode = TH_IN, .type = arguments[i]};
    }
    return th_func_with_error(types, result, nil_type,
                              count ? parameters : nullptr, count,
                              nullptr, nullptr, 0);
}

static parse_sym_entry_t *
builtin_native_symbol(parse_symtab_t *symtab, th_ctx *types,
                      th_type *nil_type, core_string_t name,
                      th_type *result, th_type *const *arguments,
                      size_t count)
{
    return register_native_signature(symtab, types, nil_type, name,
        builtin_native_name(name),
        builtin_signature(types, nil_type, result, arguments, count));
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
register_box_primitives(parse_symtab_t *symtab, th_ctx *types,
                        th_type *nil_type)
{
    th_type *any = th_name(types, CORE_STRING_STATIC("Any"));
    th_type *slot = any ? th_ind(types, 0, any) : nullptr;
    th_type *held = th_var(types);
    th_type *produced = th_var(types);
    return slot && held && produced
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__box_of"), slot, &held, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__result_box"), slot, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__box_as"), produced, &slot, 1);
}

static bool
register_bitptr_builtin(parse_symtab_t *symtab, th_ctx *types,
                        th_type *nil_type, th_type *reference)
{
    th_type *result = th_name(types,
                              CORE_STRING_STATIC("builtin.bitptr.Bitptr"));
    return builtin_native_symbol(symtab, types, nil_type,
        CORE_STRING_STATIC("__bitptr"), result, &reference, 1) != nullptr;
}

static th_type *
native_type(th_ctx *types, const n00bb_native_type_t *native)
{
    th_type *item = native->item ? native_type(types, native->item) : nullptr;
    switch (native->kind) {
    case N00BB_NATIVE_TYPE_REF:
        return item ? th_ind(types, 0, item) : nullptr;
    case N00BB_NATIVE_TYPE_ARRAY:
        return item ? th_array(types, item) : nullptr;
    case N00BB_NATIVE_TYPE_LIST: {
        th_arg argument = {.kind = TH_ARG_TYPE, .type = item};
        return item ? th_apply(types, native->name, &argument, 1) : nullptr;
    }
    case N00BB_NATIVE_TYPE_OPTION:
        return item ? th_option(types, item) : nullptr;
    default:
        return th_name(types, native->name);
    }
}

static th_type *
native_signature(th_ctx *types, th_type *nil_type,
                 const n00bb_native_declaration_t *native)
{
    th_param *parameters = native->param_count
        ? core_alloc_array(th_param, native->param_count) : nullptr;
    bool valid = true;
    for (size_t p = 0; p < native->param_count; p++) {
        parameters[p] = (th_param){
            .mode = TH_IN, .type = native_type(types, native->params[p]),
        };
        valid = valid && parameters[p].type;
    }
    th_type *result = native_type(types, native->result);
    th_type *signature = valid && result
        ? th_func_with_error(types, result, nil_type, parameters,
                             native->param_count, nullptr, nullptr, 0)
        : nullptr;
    core_free(parameters);
    return signature;
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
        th_type *signature = native_signature(types, nil_type,
                                              &natives.items[i]);
        parse_sym_entry_t *symbol = register_native_signature(symtab, types,
            nil_type, natives.items[i].name, &natives.items[i].symbol,
            signature);
        if (!symbol || !parse_sym_set_binding(symbol,
                CORE_STRING_STATIC("n00bb.generated-extern"), symbol))
            return false;
    }
    n00bb_note_native_axioms(natives);
    return true;
}

void
n00bb_note_native_axioms(n00bb_native_declarations_t natives)
{
    for (size_t i = 0; i < natives.count; i++) {
        if (natives.items[i].pure)
            n00bb_axiom_note_pure_native(natives.items[i].symbol);
        else n00bb_axiom_note_foreign_native(natives.items[i].symbol,
                                             natives.items[i].name,
                                             natives.items[i].effects,
                                             natives.items[i].read_only,
                                             natives.items[i].write_only);
    }
}

static bool
register_builtin_os_symbols(parse_symtab_t *symtab, th_ctx *types,
                            th_type *nil_type)
{
    th_type *u32 = th_name(types, CORE_STRING_STATIC("u32"));
    th_type *u64 = th_name(types, CORE_STRING_STATIC("uint"));
    th_type *failure_args[] = {
        u32, u64, u64, u64, th_name(types, CORE_STRING_STATIC("string")),
    };
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *text = th_name(types, CORE_STRING_STATIC("string"));
    th_type *effect_subject = th_var(types);
    th_type *reads_subject = th_var(types);
    th_type *writes_subject = th_var(types);
    th_type *has_effect_args[] = {
        effect_subject ? th_ind(types, 0, effect_subject) : nullptr, text,
    };
    th_type *reads_args[] = {
        reads_subject ? th_ind(types, 0, reads_subject) : nullptr, text,
    };
    th_type *writes_args[] = {
        writes_subject ? th_ind(types, 0, writes_subject) : nullptr, text,
    };
    th_type *unobserved = th_var(types);
    th_type *unobserved_args[] = {unobserved};
    return register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__gc_enter_blocking"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__gc_leave_blocking"), nil_type, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__function_has_effect"), boolean,
            has_effect_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__function_reads"), boolean, reads_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__function_writes"), boolean, writes_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__unobserved"), unobserved,
            unobserved_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__report_failure"), nil_type,
            failure_args, 5);
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
        CORE_STRING_STATIC("builtin.abi.Tls_Relocation_Record"),
        CORE_STRING_STATIC("builtin.abi.Symbol_Record"),
        CORE_STRING_STATIC("builtin.abi.Module_Record"),
        CORE_STRING_STATIC("builtin.abi.Baked_Node_Record"),
        CORE_STRING_STATIC("builtin.abi.Function_Record"),
        CORE_STRING_STATIC("builtin.abi.Arm_Record"),
        CORE_STRING_STATIC("builtin.abi.Physical_Run_Record"),
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
        CORE_STRING_STATIC("__abi_tls_relocation"),
        CORE_STRING_STATIC("__abi_symbol"),
        CORE_STRING_STATIC("__abi_module"),
        CORE_STRING_STATIC("__abi_baked_node"),
        CORE_STRING_STATIC("__abi_function"),
        CORE_STRING_STATIC("__abi_arm"),
        CORE_STRING_STATIC("__abi_physical_run"),
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
    th_type *native_root = th_name(types,
        CORE_STRING_STATIC("builtin.abi.Native_Frame_Root"));
    th_type *native_root_reference = native_root
        ? th_ind(types, 0, native_root) : nullptr;
    th_type *native_frame_args[] = {u32};
    th_type *native_root_args[] = {native_frame_reference, u32};
    th_type *item = th_var(types);
    th_type *reference = item ? th_ind(types, 0, item) : nullptr;
    th_type *address_args[] = {reference};
    th_type *storage_args[] = {u32};
    th_type *allocate_args[] = {typespec, uint};
    th_type *baked_args[] = {th_name(types, CORE_STRING_STATIC("uint"))};
    return register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_baked_address"),
            th_name(types, CORE_STRING_STATIC("bool")), baked_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__native_symbol_name"),
            bytes_result, baked_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_native_frame_count"), u32, nullptr, 0)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_native_frame"),
            native_frame_result, native_frame_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_native_frame_root"),
            native_root_reference, native_root_args, 2)
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
            CORE_STRING_STATIC("__gc_allocate_bytes"), byte_reference,
            allocate_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_table_base"), bytes_result,
            storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_table_count"), u32, storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_table_record_bytes"), u32,
            storage_args, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_range_index"), i64, range_args, 3)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_unary_type"), u32, bytes_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_type_specializes"),
            th_name(types, CORE_STRING_STATIC("i32")), bytes_args, 2)
        && register_native_signature(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_typespec_specializes"),
            builtin_native_name(
                CORE_STRING_STATIC("__abi_type_specializes")),
            th_func_with_error(types,
                th_name(types, CORE_STRING_STATIC("i32")), nil_type,
                (th_param[]){{.mode = TH_IN, .type = typespec},
                             {.mode = TH_IN, .type = typespec}},
                2, nullptr, nullptr, 0))
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_type_argument_type"), u32, bytes_args,
            1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_named_type"), u32,
            (th_type *[]){byte_reference, uint, byte_reference, u32}, 4)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__dispatch_arm"), u32,
            (th_type *[]){uint, u32}, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__dispatch_record"), nil_type,
            (th_type *[]){uint, u32, u32}, 3)
        && register_box_primitives(symtab, types, nil_type)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_bytes"), bytes_result, bytes_args, 2)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__abi_ref_address"), uint, address_args, 1)
        && register_bitptr_builtin(symtab, types, nil_type, reference)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__load_le_u64"),
            th_name(types, CORE_STRING_STATIC("u64")), &byte_reference, 1)
        && register_builtin_native_function(symtab, types, nil_type,
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
            {.mode = TH_IN, .type = entry->shift ? th_var(types) : value},
        };
        th_type *signature = value && uint_type && nil_type
            ? th_func_with_error(types, value, nil_type, parameters,
                                 entry->unary ? 1 : 2, nullptr, nullptr, 0)
            : nullptr;
        if (!add_builtin(symtab, types, nil_type, entry->name, signature,
                CORE_STRING_STATIC("n00bb.integer.unsafe"), entry))
            return false;
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
    builtin_fn_t count = builtin_fn(types, nil_type,
        uint_type && u32_type && nil_type
        ? th_func_with_error(types, u32_type, nil_type, &parameter, 1,
                             nullptr, nullptr, 0) : nullptr);
    static const core_string_t names[] = {
        CORE_STRING_STATIC("__count_leading_zeros"),
        CORE_STRING_STATIC("__count_trailing_zeros"),
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!add_builtin_fn(symtab, nil_type, names[i], count,
                CORE_STRING_STATIC("n00bb.integer.bit_count"), nullptr))
            return false;
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
    th_type *source_item = th_var(types);
    th_type *source_reference = source_item
        ? th_ind(types, 0, source_item) : nullptr;
    th_type *volatile_item = th_var(types);
    th_type *volatile_ref = volatile_item
        ? th_ind(types, 0, volatile_item) : nullptr;
    const struct {
        core_string_t name;
        core_string_t binding;
        th_type *result;
        th_type *arguments[3];
        size_t count;
    } memory[] = {
        {CORE_STRING_STATIC("__gc_byte_offset"),
         CORE_STRING_STATIC("n00bb.gc.byte_offset"), reference,
         {reference, offset_type}, 2},
        {CORE_STRING_STATIC("__gc_byte_diff"),
         CORE_STRING_STATIC("n00bb.gc.byte_diff"),
         th_name(types, CORE_STRING_STATIC("int")), {reference, reference}, 2},
        {CORE_STRING_STATIC("__gc_memset"), CORE_STRING_STATIC("n00bb.gc.memory"),
         nil_type, {reference, byte, uint}, 3},
        {CORE_STRING_STATIC("__gc_memcpy"), CORE_STRING_STATIC("n00bb.gc.memory"),
         nil_type, {reference, source_reference, uint}, 3},
        {CORE_STRING_STATIC("__volatile_load"),
         CORE_STRING_STATIC("n00bb.volatile.load"), volatile_item,
         {volatile_ref}, 1},
    };
    for (size_t i = 0; i < sizeof(memory) / sizeof(memory[0]); i++)
        if (!add_builtin(symtab, types, nil_type, memory[i].name,
                builtin_signature(types, nil_type, memory[i].result,
                                  memory[i].arguments, memory[i].count),
                memory[i].binding, nullptr))
            return false;

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
    builtin_fn_t load = builtin_fn(types, nil_type, generic_ref
        ? th_func_with_error(types, item, nil_type, &load_parameter, 1,
                             nullptr, nullptr, 0) : nullptr);
    builtin_fn_t store = builtin_fn(types, nil_type, generic_ref
        ? th_func_with_error(types, nil_type, nil_type, store_parameters, 2,
                             nullptr, nullptr, 0) : nullptr);
    th_type *boolean = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *cas_signature = generic_ref && boolean
        ? th_func_with_error(types, boolean, nil_type, cas_parameters, 3,
                             nullptr, nullptr, 0) : nullptr;
    builtin_fn_t exchange = builtin_fn(types, nil_type, generic_ref
        ? th_func_with_error(types, item, nil_type, store_parameters, 2,
                             nullptr, nullptr, 0) : nullptr);
    if (!add_builtin_fn(symtab, nil_type, CORE_STRING_STATIC("__atomic_load"),
            load, CORE_STRING_STATIC("n00bb.atomic.load"), nullptr)
        || !add_builtin_fn(symtab, nil_type,
            CORE_STRING_STATIC("__atomic_store"), store,
            CORE_STRING_STATIC("n00bb.atomic.store"), nullptr)
        || !add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__atomic_compare_exchange"), cas_signature,
            CORE_STRING_STATIC("n00bb.atomic.compare_exchange"), nullptr)
        || !add_builtin_fn(symtab, nil_type,
            CORE_STRING_STATIC("__atomic_exchange"), exchange,
            CORE_STRING_STATIC("n00bb.atomic.exchange"), nullptr))
        return false;
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
            / sizeof(ordered_access_names[0]); i++)
        if (!add_builtin_fn(symtab, nil_type, ordered_access_names[i],
                i < 2 ? load : store, ordered_access_bindings[i], nullptr))
            return false;
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
             / sizeof(atomic_rmw_names[0]); i++)
        if (!add_builtin_fn(symtab, nil_type, atomic_rmw_names[i], exchange,
                atomic_rmw_bindings[i], nullptr))
            return false;
    builtin_fn_t fence = builtin_fn(types, nil_type, nil_type
        ? th_func_with_error(types, nil_type, nil_type, nullptr, 0,
                             nullptr, nullptr, 0) : nullptr);
    if (!add_builtin_fn(symtab, nil_type,
            CORE_STRING_STATIC("__gc_signal_fence"), fence,
            CORE_STRING_STATIC("n00bb.gc.signal_fence"), nullptr))
        return false;
    static const struct {
        core_string_t name;
        bool          address;
    } machine[] = {
        {CORE_STRING_STATIC("__thread_fence"), false},
        {CORE_STRING_STATIC("__cpu_relax"), false},
        {CORE_STRING_STATIC("__stack_pointer"), true},
    };
    th_type *stack_type = th_name(types, CORE_STRING_STATIC("uint"));
    builtin_fn_t stack = builtin_fn(types, nil_type, stack_type
        ? th_func_with_error(types, stack_type, nil_type, nullptr, 0,
                             nullptr, nullptr, 0) : nullptr);
    for (size_t i = 0; i < sizeof(machine) / sizeof(machine[0]); i++)
        if (!add_builtin_fn(symtab, nil_type, machine[i].name,
                machine[i].address ? stack : fence,
                CORE_STRING_STATIC("n00bb.machine"), nullptr))
            return false;
    th_type *comptime_signature = boolean
        ? th_func_with_error(types, boolean, nil_type, nullptr, 0,
                             nullptr, nullptr, 0) : nullptr;
    if (!add_builtin(symtab, types, nil_type, CORE_STRING_STATIC("comptime"),
            comptime_signature, CORE_STRING_STATIC("n00bb.comptime"),
            nullptr)) return false;
    th_type *prefetch_item = th_var(types);
    th_type *prefetch_ref = prefetch_item
        ? th_ind(types, 0, prefetch_item) : nullptr;
    th_param prefetch_parameter = {.mode = TH_IN, .type = prefetch_ref};
    th_type *prefetch_signature = prefetch_ref
        ? th_func_with_error(types, nil_type, nil_type,
            &prefetch_parameter, 1, nullptr, nullptr, 0) : nullptr;
    if (!add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__gc_prefetch"), prefetch_signature,
            CORE_STRING_STATIC("n00bb.gc.prefetch"), nullptr)) return false;
    th_type *payload_type = th_ind(types, 0, byte);
    th_param invoke_parameters[] = {
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("u32"))},
        {.mode = TH_IN, .type = payload_type},
    };
    th_type *invoke_signature = payload_type
        ? th_func_with_error(types, nil_type, nil_type,
            invoke_parameters, 2, nullptr, nullptr, 0) : nullptr;
    core_string_t invoke = CORE_STRING_STATIC("__gc_invoke_finalizer");
    if (!register_native_signature(symtab, types, nil_type, invoke,
            builtin_native_name(invoke), invoke_signature))
        return false;
    th_type *address_result = payload_type
        ? th_option(types, payload_type) : nullptr;
    builtin_fn_t address = builtin_fn(types, nil_type, address_result
        ? th_func_with_error(types, address_result, nil_type,
            invoke_parameters, 2, nullptr, nullptr, 0) : nullptr);
    static const core_string_t address_names[] = {
        CORE_STRING_STATIC("__ref_address_boxed"),
        CORE_STRING_STATIC("__ref_address_mut_boxed"),
    };
    for (size_t i = 0; i < 2; i++) {
        const core_string_t *native = builtin_native_name(address_names[i]);
        if (!native || !add_builtin_fn(symtab, nil_type, address_names[i],
                address, CORE_STRING_STATIC("n00bb.abi.native"), native))
            return false;
    }
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

    th_type *bool_type = th_name(types, CORE_STRING_STATIC("bool"));
    th_type *nil_type = th_name(types, CORE_STRING_STATIC("nil"));
    th_type *option_is_nil_item = th_var(types);
    th_type *option_is_nil_type = option_is_nil_item
        ? th_option(types, option_is_nil_item) : nullptr;
    th_param option_is_nil_parameter = {
        .mode = TH_IN, .type = option_is_nil_type,
    };
    th_type *option_is_nil_signature = option_is_nil_type && bool_type
        ? th_func_with_error(types, bool_type, nil_type,
            &option_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
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
    th_type *array_item = th_var(types);
    th_type *array_type = th_array(types, array_item);
    th_type *array_index = n00bb_array_index_type(types);
    th_param array_parameters[] = {
        {.mode = TH_IN, .type = array_type},
        {.mode = TH_IN, .type = array_index},
        {.mode = TH_IN, .type = array_item},
    };
    th_type *array_write_signature = array_type && array_index
        ? th_func_with_error(types, nil_type, nil_type,
            array_parameters, 3, nullptr, nullptr, 0) : nullptr;
    th_type *array_read_signature = array_type && array_index
        ? th_func_with_error(types, array_item, nil_type,
            array_parameters, 2, nullptr, nullptr, 0) : nullptr;
    th_type *init_receiver = th_var(types);
    th_type *init_argument = th_var(types);
    th_param init_parameter = {.mode = TH_INOUT, .type = init_receiver};
    th_param init_rest = {.mode = TH_IN, .type = init_argument};
    th_type *init_signature = init_receiver && init_argument
        ? th_func_with_error(types, nil_type, nil_type, &init_parameter, 1,
            &init_rest, nullptr, 0) : nullptr;

    parse_scope_t *saved = parse_symtab_current_scope(
        symtab, CORE_STRING_STATIC("builtin"));
    parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                             values->root);
    bool registered = add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__option_is_nil"), option_is_nil_signature,
            (core_string_t){0}, nullptr)
        && add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__option_get_or_else"),
            option_get_or_else_signature, (core_string_t){0}, nullptr)
        && add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__array_write"), array_write_signature,
            CORE_STRING_STATIC("n00bb.array.write"), nullptr)
        && add_builtin(symtab, types, nil_type,
            CORE_STRING_STATIC("__array_read"), array_read_signature,
            CORE_STRING_STATIC("n00bb.array.read"), nullptr)
        && add_builtin(symtab, types, nil_type, CORE_STRING_STATIC("init"),
            init_signature, CORE_STRING_STATIC("n00bb.init"), nullptr);
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
            unwind_arguments, 1)
        && register_builtin_native_function(symtab, types, nil_type,
            CORE_STRING_STATIC("__ffi_libc_thread_check"), nil_type,
            nullptr, 0);
    if (saved)
        parse_symtab_enter_scope(symtab, CORE_STRING_STATIC("builtin"),
                                 saved);
    return registered
        && os_builtins_registered && abi_builtins_registered
        && gc_builtins_registered && unsafe_arithmetic_registered
        && bit_counts_registered && array_length_registered
        && array_prefix_registered && array_clear_registered
        && borrow_registered
        && register_builtin_comparison_symbols(symtab, state, types);
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
        {.mode = TH_VIEW, .type = self},
        {.mode = TH_VIEW, .type = other},
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

bool
n00bb_trap_integer_arithmetic(ir_instr_t *instruction, ir_class_t class)
{
    if ((class != IR_CLASS_SINT && class != IR_CLASS_UINT)
        || !ir_op_accepts_mod(instruction->op, CORE_STRING_STATIC("trap")))
        return true;
    bool divides = instruction->op == IR_OP_DIV
        || instruction->op == IR_OP_REM;
    return ir_instr_add_mod(instruction, CORE_STRING_STATIC("trap"))
        && (!divides || class != IR_CLASS_SINT
            || ir_instr_add_mod(instruction, CORE_STRING_STATIC("trap")));
}

static bool
register_operator_trait(th_env *environment, th_ctx *types,
                        core_string_t trait, core_string_t method_name,
                        th_mode receiver_mode, th_type *result)
{
    th_param parameters[] = {
        {.mode = receiver_mode, .type = th_var(types)},
        {.mode = TH_VIEW, .type = th_var(types)},
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
register_builtin_binary_operators(th_env *environment, th_ctx *types)
{
    const n00bb_binary_operator_t *operators = n00bb_binary_operators;
    for (size_t o = 0; o < n00bb_binary_operator_count; o++) {
        if (!register_operator_trait(environment, types, operators[o].trait,
                operators[o].method, TH_VIEW, th_var(types))
            || !register_operator_trait(environment, types,
                operators[o].in_place_trait, operators[o].in_place,
                TH_INOUT, th_name(types, CORE_STRING_STATIC("nil")))) return false;
        for (size_t n = 0; n < sizeof(numeric_types) / sizeof(numeric_types[0]); n++) {
            core_string_t name = numeric_types[n];
            th_type *self = th_name(types, name);
            if (operators[o].unsigned_only && !n00bb_bitwise_type(self))
                continue;
            th_type *other = operators[o].shift
                ? th_name(types, CORE_STRING_STATIC("uint")) : self;
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
        th_param contract_param = {.mode = TH_VIEW, .type = receiver};
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
register_bit_integer_from_bits(th_env *environment, th_ctx *types)
{
    th_type *bits = th_name(types, CORE_STRING_STATIC("u128"));
    for (uint64_t width = 1; width <= 128; width++) {
        for (int is_signed = 0; is_signed <= 1; is_signed++) {
            th_type *self = th_bitint(types, width, is_signed);
            th_param parameters[] = {
                {.mode = TH_IN, .type = self},
                {.mode = TH_IN, .type = bits},
            };
            th_type *signature = self && bits
                ? th_func_with_error(types, self,
                    th_name(types, CORE_STRING_STATIC("nil")), parameters, 2,
                    nullptr, nullptr, 0) : nullptr;
            core_buffer_t qualified = {0};
            core_buffer_printf(&qualified, "builtins.Integer.%c%llu.from_bits",
                is_signed ? 'i' : 'u', (unsigned long long)width);
            bool registered = signature && qualified.data
                && th_env_register_function(environment,
                    core_buffer_string(&qualified),
                    CORE_STRING_STATIC("from_bits"),
                    CORE_STRING_STATIC("builtins"),
                    CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, signature);
            core_buffer_release(&qualified);
            if (!registered) return false;
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
        {N00BB_EQUAL_TRAIT, CORE_STRING_STATIC("__equal"),
         CORE_STRING_STATIC("bool")},
        {N00BB_COMPARABLE_TRAIT, CORE_STRING_STATIC("__cmp"),
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
        th_type *result = th_name(types, operators[o].result);
        size_t ordered_count = sizeof(numeric_types) / sizeof(numeric_types[0]);
        bool equal_trait = core_string_eq(operators[o].trait,
                                          N00BB_EQUAL_TRAIT);
        size_t equality_count = equal_trait
            ? sizeof(equality_types) / sizeof(equality_types[0]) : 0;
        for (size_t n = 0; n < ordered_count + equality_count; n++) {
            core_string_t name = n < ordered_count ? ordered_types[n]
                : equality_types[n - ordered_count];
            th_type *self = th_name(types, name);
            th_param primitive_params[] = {
                {.mode = TH_VIEW, .type = self},
                {.mode = TH_VIEW, .type = self},
            };
            th_type *signature = th_func_with_error(types, result,
                th_name(types, CORE_STRING_STATIC("nil")), primitive_params, 2,
                nullptr, nullptr, 0);
            core_buffer_t qualified = {0};
            core_string_t label = n00bb_name_leaf(operators[o].trait);
            core_buffer_printf(&qualified, "builtins.%.*s.%.*s.%.*s",
                (int)label.u8_bytes, label.data,
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
                {.mode = TH_VIEW, .type = reference},
                {.mode = TH_VIEW, .type = reference},
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
register_builtin_traits(th_env *environment, th_ctx *types)
{
    th_type *methods = th_record(types, nullptr, 0);
    if (!methods ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Sequence_Like"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods) ||
        !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Dict_Like"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods))
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
    th_param initializer_receiver = {.mode = TH_INOUT, .type = th_var(types)};
    th_param initializer_rest = {.mode = TH_IN, .type = th_var(types)};
    th_type *initializer_signature = th_func_with_error(types,
        th_name(types, CORE_STRING_STATIC("nil")),
        th_name(types, CORE_STRING_STATIC("nil")), &initializer_receiver, 1,
        &initializer_rest, nullptr, 0);
    th_member initializer_method = {
        .name = CORE_STRING_STATIC("__init"), .type = initializer_signature,
    };
    th_type *initializer_contract = initializer_signature
        ? th_record(types, &initializer_method, 1) : nullptr;
    if (!initializer_contract
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Init"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, initializer_contract))
        return false;
    if (!register_builtin_binary_operators(environment, types))
        return false;
    if (!register_builtin_unary_operators(environment, types)) return false;
    if (!register_builtin_comparison_operators(environment, types)) return false;
    if (!register_bit_integer_from_bits(environment, types)) return false;
    th_type *source = th_var(types);
    th_type *cursor = th_var(types);
    th_type *iter_error = th_var(types);
    th_param iter_param = {.mode = TH_IN, .type = source};
    th_type *iter_signature = th_func_with_error(types, cursor, iter_error,
        &iter_param, 1, nullptr, nullptr, 0);
    th_member iter_method = {
        .name = CORE_STRING_STATIC("__iterator"),
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
        .name = CORE_STRING_STATIC("__next"),
        .type = next_signature,
    };
    th_param cleanup_param = {.mode = TH_CONSUME, .type = iterator};
    th_type *cleanup_signature = th_func_with_error(types,
        th_name(types, CORE_STRING_STATIC("nil")), th_name(types, CORE_STRING_STATIC("nil")), &cleanup_param, 1,
        nullptr, nullptr, 0);
    th_member iterator_methods[] = {
        next_method,
        {.name = CORE_STRING_STATIC("__cleanup"), .type = cleanup_signature},
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
    th_type *option_bool = th_name(types, CORE_STRING_STATIC("bool"));
    th_param is_nil_parameter = {.mode = TH_IN, .type = option_receiver};
    th_type *is_nil_signature = option_bool
        ? th_func_with_error(types, option_bool, th_name(types, CORE_STRING_STATIC("nil")),
            &is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;
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
        {.name = CORE_STRING_STATIC("get_or_else"), .type = get_or_else_signature},
    };
    th_type *option_contracts = th_record(types, option_methods, 2);

    th_type *option_item = th_var(types);
    th_type *option_type = th_option(types, option_item);
    th_param builtin_is_nil_parameter = {.mode = TH_IN, .type = option_type};
    th_type *builtin_is_nil_signature = option_type && option_bool
        ? th_func_with_error(types, option_bool, th_name(types, CORE_STRING_STATIC("nil")),
            &builtin_is_nil_parameter, 1, nullptr, nullptr, 0) : nullptr;

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
        || !option_methods[1].type
        || !th_env_register_trait_contracts(environment, CORE_STRING_STATIC("Option"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, option_contracts)
        || !builtin_is_nil_signature
        || !builtin_get_or_else_signature
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__option_is_nil"),
            CORE_STRING_STATIC("is_nil"), CORE_STRING_STATIC("builtins"),
            CORE_STRING_STATIC("default"), TH_ENV_PUBLIC,
            builtin_is_nil_signature)
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
    th_type *array_read_index = n00bb_array_index_type(types);
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

    th_type *referent = th_var(types);
    th_type *reference = th_ind(types, 0, referent);
    th_param address_parameter = {.mode = TH_IN, .type = reference};
    th_type *address_signature = reference
        ? th_func_with_error(types, reference,
            th_name(types, CORE_STRING_STATIC("nil")), &address_parameter, 1,
            nullptr, nullptr, 0) : nullptr;
    if (!address_signature
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__ref_address"),
            CORE_STRING_STATIC("__ref_address"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"),
            TH_ENV_PUBLIC, address_signature)
        || !th_env_register_function(environment,
            CORE_STRING_STATIC("builtins.__ref_address_mut"),
            CORE_STRING_STATIC("__ref_address_mut"),
            CORE_STRING_STATIC("builtins"), CORE_STRING_STATIC("default"),
            TH_ENV_PUBLIC, address_signature)) return false;

    th_type *slice_receiver = th_var(types);
    th_type *slice_bound = th_option(types,
        th_name(types, CORE_STRING_STATIC("int")));
    th_param slice_parameters[] = {
        {.mode = TH_IN, .type = slice_receiver},
        {.mode = TH_IN, .type = slice_bound},
        {.mode = TH_IN, .type = slice_bound},
        {.mode = TH_IN, .type = th_name(types, CORE_STRING_STATIC("bool"))},
    };
    th_type *slice_signature = slice_bound
        ? th_func_with_error(types, th_var(types), th_var(types),
            slice_parameters, 4, nullptr, nullptr, 0) : nullptr;
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
    th_type *array_write_index = n00bb_array_index_type(types);
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
    th_type *dict_key = th_var(types);
    th_type *dict_value = th_var(types);
    th_arg list_arg = {.kind = TH_ARG_TYPE, .type = list_element};
    th_arg dict_args[] = {
        {.kind = TH_ARG_TYPE, .type = dict_key},
        {.kind = TH_ARG_TYPE, .type = dict_value},
    };
    th_type *list = th_apply(types,
        CORE_STRING_STATIC("builtin.containers.list.List"), &list_arg, 1);
    th_type *dict = th_apply(types,
        CORE_STRING_STATIC("builtin.containers.dict.Dict"), dict_args, 2);
    return th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Sequence_Like"), list) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Sequence_Like"), th_array(types, array_element)) &&
           th_env_register_intrinsic_type_pattern(environment,
               CORE_STRING_STATIC("Dict_Like"), dict);
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
        && (parse_sym_binding(symbol,
                CORE_STRING_STATIC("n00bb.builtin-operator"))
            || n00bb_builtin_option_function(symbol)
            || n00bb_builtin_array_read_function(symbol)
            || n00bb_builtin_array_write_function(symbol)
            || n00bb_builtin_byte_offset_function(symbol)
            || n00bb_builtin_byte_diff_function(symbol)
            || n00bb_builtin_unsafe_arithmetic(symbol)
            || n00bb_builtin_bit_count_function(symbol)
            || n00bb_builtin_gc_memory_function(symbol)
            || n00bb_builtin_volatile_load_function(symbol)
            || n00bb_builtin_signal_fence_function(symbol)
            || n00bb_builtin_machine_function(symbol)
            || n00bb_builtin_comptime_function(symbol)
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

static bool
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
n00bb_builtin_machine_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.machine"));
}

bool
n00bb_builtin_signal_fence_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.gc.signal_fence"));
}

bool
n00bb_builtin_comptime_function(parse_sym_entry_t *symbol)
{
    return symbol && symbol->kind == PARSE_SYM_FUNCTION
        && !symbol->decl_node && parse_sym_binding(symbol,
            CORE_STRING_STATIC("n00bb.comptime"));
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



