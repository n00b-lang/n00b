#include "lower_internal.h"

static bool
atomic_aggregate_layout_pending(ir_module_t *module, th_type *atomic_type,
                                const core_target_desc_t *target);
static bool
prepare_ir_type_layouts(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *owner,
                        ir_module_t *module, th_type *type,
                        const core_target_desc_t *target,
                        core_list_t(th_type_ptr_t) *visited);
static bool
prepare_ir_type_layouts_for_symbol(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_sym_entry_t *symbol,
                                   ir_module_t *module,
                                   const core_target_desc_t *target);
static void
declaration_internal(core_translation_unit_t *owner, core_string_t what);
static bool
bind_link_declarations_to_ir(parse_grammar_t *grammar,
                             core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             uint32_t producer_stage);

bool
n00bb_prepare_ir_bindings(core_link_unit_t *link,
                          n00bb_link_analysis_t *analysis,
                          uint32_t stage_kind)
{
    if (!analysis->ir_context)
        return false;
    if (!initialize_ir_modules(analysis->ir_context, analysis->grammar,
                               link, analysis, stage_kind))
        return false;
    if (!bind_link_declarations_to_ir(analysis->grammar, link, analysis,
                                      stage_kind)) {
        if (!core_link_unit_has_fatal_diagnostics(link))
            n00bb_internal_error(core_translation_unit_diagnostics(
                    analysis->units.data[0]), analysis->units.data[0],
                (core_diag_location_t){0}, "preparing code generation",
                "could not bind program declarations to their IR");
        return false;
    }
    if (!select_storage_placement(analysis))
        return false;
    if (!core_link_unit_validate(link)) {
        if (!core_diagnostics_failed(core_translation_unit_diagnostics(
                analysis->units.data[0])) &&
            !core_link_unit_has_fatal_diagnostics(link))
            n00bb_internal_error(core_link_unit_diagnostics(link), nullptr,
                (core_diag_location_t){0}, "preparing code generation",
                "the program's declarations failed validation");
        return false;
    }
    if (!bind_link_references_to_nodes(analysis->grammar, link)) {
        n00bb_internal_error(core_translation_unit_diagnostics(
                analysis->units.data[0]), analysis->units.data[0],
            (core_diag_location_t){0}, "preparing code generation",
            "could not bind references to their source");
        return false;
    }
    for (size_t i = 0; i < analysis->units.len; i++)
        if (!validate_module_declarations(analysis->grammar,
                analysis->units.data[i], analysis->ir_context)) {
            if (!core_diagnostics_failed(core_translation_unit_diagnostics(
                    analysis->units.data[i])))
                n00bb_internal_error(core_translation_unit_diagnostics(
                        analysis->units.data[i]), analysis->units.data[i],
                    (core_diag_location_t){0}, "preparing code generation",
                    "could not validate the module's declarations");
            return false;
        }
    return true;
}

void
dump_layout_for_unit(parse_grammar_t *grammar, core_translation_unit_t *unit)
{
    n00bb_tu_state_t *state = tu_state(unit);
    if (!state || !state->annotations || !state->annotations->symtab ||
        !state->ir_module) return;
    core_string_t target = ir_module_target(state->ir_module);
    core_list_t(parse_scope_ptr_t) scopes = module_scopes_in_namespace(
        state, CORE_STRING_STATIC("type"));
    for (size_t s = 0; s < scopes.len; s++) {
        for (parse_sym_entry_t *symbol = scopes.data[s]->first_in_scope;
             symbol; symbol = symbol->next_in_scope) {
            if (n00bb_injected_member(symbol)) continue;
            if (symbol->kind != PARSE_SYM_TYPEDEF || !symbol->type) continue;
            ir_layout_compute(state->ir_module, symbol->type, nullptr);
            ir_descriptor_t *descriptor = ir_layout_of(state->ir_module,
                symbol->type, target);
            if (!descriptor) continue;
            core_string_t printed = n00bb_print_type_qualified(state->types,
                                                               symbol->type);
            core_string_t target_text = target.data
                ? target : CORE_STRING_STATIC("<none>");
            printf("layout %.*s target=%.*s size=",
                   (int)printed.u8_bytes, printed.data,
                   (int)target_text.u8_bytes, target_text.data);
            if (ir_layout_is_settled(&descriptor->size))
                printf("%llu", (unsigned long long)descriptor->size.value);
            else fputs("<unresolved>", stdout);
            fputs(" align=", stdout);
            if (ir_layout_is_settled(&descriptor->align))
                printf("%llu", (unsigned long long)descriptor->align.value);
            else fputs("<unresolved>", stdout);
            putchar('\n');
            for (uint32_t m = 0; m < descriptor->n_members; m++) {
                const ir_member_layout_t *member = &descriptor->members[m];
                printf("  member %.*s bit-offset=",
                       (int)member->name.u8_bytes, member->name.data);
                if (ir_layout_is_settled(&member->bit_offset))
                    printf("%llu", (unsigned long long)
                        member->bit_offset.value);
                else fputs("<unresolved>", stdout);
                fputs(" bit-width=", stdout);
                if (ir_layout_is_settled(&member->bit_width))
                    printf("%llu", (unsigned long long)
                        member->bit_width.value);
                else fputs("<unresolved>", stdout);
                putchar('\n');
            }
        }
    }
    core_list_free(scopes);
    (void)grammar;
}

static bool
atomic_aggregate_layout_pending(ir_module_t *module, th_type *atomic_type,
                                const core_target_desc_t *target)
{
    const th_arg *argument = th_named_arg(atomic_type, 0);
    th_type *child = argument && argument->kind == TH_ARG_TYPE
        ? th_resolve(argument->type) : nullptr;
    if (!child || ir_layout_final(module, child, target->triple))
        return false;
    if (th_kind_of(child) == TH_RECORD) return true;
    th_type *definition = th_kind_of(child) == TH_NAMED
        ? ir_type_definition(module, child) : nullptr;
    return definition && th_kind_of(definition) == TH_RECORD;
}

static bool
prepare_ir_type_layouts(parse_grammar_t *grammar,
                        core_translation_unit_t *unit, parse_tree_t *owner,
                        ir_module_t *module, th_type *type,
                        const core_target_desc_t *target,
                        core_list_t(th_type_ptr_t) *visited)
{
    type = type ? th_resolve(type) : nullptr;
    if (!type) return true;
    for (size_t i = 0; i < visited->len; i++)
        if (visited->data[i] == type) return true;
    core_list_push(*visited, type);

    bool ok = true;
    switch (th_kind_of(type)) {
    case TH_NAMED: {
        core_string_t name = {};
        size_t argument_count = 0;
        bool is_variant = false;
        th_named_info(type, &name, &is_variant, &argument_count);
        int expected = !is_variant
            ? builtin_generic_type_arity(name) : -1;
        if (expected >= 0 && argument_count != (size_t)expected) {
            validate_builtin_generic_type_arity(grammar, owner, unit, type);
            ok = false;
            break;
        }
        if (th_builtin_kind_of(type) == TH_BUILTIN_ATOMIC) {
            const th_arg *argument = th_named_arg(type, 0);
            if (argument_count != 1 || !argument
                || argument->kind != TH_ARG_TYPE
                || !ir_type_define(module, type, argument->type)) {
                n00bb_internal_error(core_translation_unit_diagnostics(unit),
                    unit, link_source_location(owner), "laying out types",
                    "could not register the atomic type layout");
                ok = false;
                break;
            }
        }
        for (size_t i = 0; ok && i < argument_count; i++) {
            const th_arg *argument = th_named_arg(type, i);
            if (argument && argument->kind == TH_ARG_TYPE)
                ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                                              argument->type, target, visited);
        }
        if (ok && th_builtin_kind_of(type) == TH_BUILTIN_ATOMIC
            && !atomic_aggregate_layout_pending(module, type, target)
            && !n00bb_atomic_native_support(module, type, target)) {
            core_string_t type_text = n00bb_print_type(
                ir_module_types(module), type);
            n00bb_report(core_translation_unit_diagnostics(unit), unit,
                N00B_DIAG_ATOMIC_UNSUPPORTED, link_source_location(owner),
                N00B_TEXT_IS_NOT_SUPPORTED_ON_TARGET,
                N00BB_ARGS(ARG_TEXT(type_text), ARG_TEXT(target->triple)));
            ok = false;
        }
        break;
    }
    case TH_INDIRECTION:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_ind_referent(type), target, visited);
        break;
    case TH_SEQUENCE:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_seq_elem(type), target, visited);
        break;
    case TH_RECORD:
        for (size_t i = 0; ok && i < th_record_len(type); i++) {
            const th_member *member = th_record_member(type, i);
            ok = member && prepare_ir_type_layouts(grammar, unit, owner,
                module, member->type, target, visited);
        }
        break;
    case TH_FUNCTION:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_func_ret(type), target, visited)
            && prepare_ir_type_layouts(grammar, unit, owner, module,
                th_func_error(type), target, visited);
        for (size_t i = 0; ok && i < th_func_nparams(type); i++) {
            const th_param *parameter = th_func_param(type, i);
            ok = parameter && prepare_ir_type_layouts(grammar, unit, owner,
                module, parameter->type, target, visited);
        }
        if (ok && th_func_rest(type))
            ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                th_func_rest(type)->type, target, visited);
        for (size_t i = 0; ok && i < th_func_nkw(type); i++) {
            const th_kwparam *parameter = th_func_kw(type, i);
            ok = parameter && prepare_ir_type_layouts(grammar, unit, owner,
                module, parameter->type, target, visited);
        }
        break;
    case TH_SUM:
        for (size_t i = 0; ok && i < th_sum_len(type); i++)
            ok = prepare_ir_type_layouts(grammar, unit, owner, module,
                th_sum_arm(type, i), target, visited);
        break;
    case TH_BINDER:
        ok = prepare_ir_type_layouts(grammar, unit, owner, module,
            th_binder_body(type), target, visited);
        break;
    default:
        break;
    }
    return ok;
}

static bool
prepare_ir_type_layouts_for_symbol(parse_grammar_t *grammar,
                                   core_translation_unit_t *unit,
                                   parse_sym_entry_t *symbol,
                                   ir_module_t *module,
                                   const core_target_desc_t *target)
{
    core_list_t(th_type_ptr_t) visited = core_list_new(th_type_ptr_t);
    bool ok = prepare_ir_type_layouts(grammar, unit, symbol->decl_node,
        module, symbol->type, target, &visited)
        && prepare_ir_type_layouts(grammar, unit, symbol->decl_node, module,
            symbol->callable_contract, target, &visited);
    core_list_free(visited);
    return ok;
}

static void
declaration_internal(core_translation_unit_t *owner, core_string_t what)
{
    if (n00bb_unit_failed(owner)) return;
    n00bb_internal_error(core_translation_unit_diagnostics(owner), owner,
        (core_diag_location_t){0}, "binding program declarations", "%.*s",
        (int)what.u8_bytes, what.data);
}

static bool
bind_link_declaration(parse_grammar_t *grammar, core_link_unit_t *link,
                      n00bb_link_analysis_t *analysis,
                      uint32_t producer_stage,
                      core_link_declaration_t *declaration)
{
    if (core_link_declaration_is_external_requirement(declaration))
        return true;
    if (core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.builtin-symbol"))) return true;
    n00bb_link_payload_t *payload = declaration
        ? core_link_declaration_payload(declaration) : nullptr;
    parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
    core_translation_unit_t *owner = declaration
        ? core_link_declaration_owner(declaration) : nullptr;
    if (!owner && symbol)
        owner = unit_for_symbol(grammar, &analysis->units, symbol);
    n00bb_tu_state_t *state = tu_state(owner);
    if (!symbol || !state || !state->ir_module) {
        core_string_t module = declaration
            ? core_link_declaration_module(declaration)
            : (core_string_t){0};
        core_string_t key = declaration
            ? core_link_declaration_key(declaration)
            : (core_string_t){0};
        core_buffer_t message = {0};
        core_buffer_printf(&message,
            "LinkUnit declaration '%.*s:%.*s' has no symbol or IR module owner",
            (int)module.u8_bytes, module.data,
            (int)key.u8_bytes, key.data);
        n00bb_internal_error(core_link_unit_diagnostics(link), owner,
            (core_diag_location_t){0}, "binding program declarations",
            "%.*s", (int)message.byte_len, message.data);
        core_buffer_release(&message);
        return false;
    }
    if (!core_link_declaration_set_binding(link, declaration,
            CORE_STRING_STATIC("n00bb.semantic.symbol"),
            producer_stage, symbol)) return false;
    if (!prepare_ir_type_layouts_for_symbol(grammar, owner, symbol,
                                             state->ir_module,
                                             &analysis->target))
        return false;
    bool module_scope = symbol->scope
        && core_string_eq(symbol->scope->scope_tag,
                          CORE_STRING_STATIC("module"));
    if (is_named_type_declaration(grammar, symbol)
        || context_symbol(grammar, symbol)) {
        th_type *type = link_semantic_type_for_symbol(
            grammar, owner, symbol);
        if (!type) {
            declaration_internal(owner, CORE_STRING_STATIC(
                    "link-unit declaration has no finalized semantic type"));
            return false;
        }
        if (!core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("n00bb.semantic.type"),
                producer_stage, type)) {
            declaration_internal(owner, CORE_STRING_STATIC(
                "could not bind a declaration's semantic type"));
            return false;
        }
    }
    else if (module_scope && symbol->kind == PARSE_SYM_FUNCTION) {
        ir_func_t *function = core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.ir.function"));
        if (!function) {
            declaration_internal(owner, CORE_STRING_STATIC(
                    "link-unit function has no direct canonical IR binding"));
            return false;
        }
    }
    else if (module_scope && symbol->kind == PARSE_SYM_VARIABLE) {
        if (!symbol->type) {
            declaration_internal(owner, CORE_STRING_STATIC(
                    "link-unit global has no finalized source type"));
            return false;
        }
        ir_global_t *global = core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.ir.global"));
        if (!global && !state->interface_module) {
            declaration_internal(owner, CORE_STRING_STATIC(
                    "link-unit global has no direct canonical IR binding"));
            return false;
        }
    }
    else if (symbol->kind == PARSE_SYM_TYPEDEF
             || symbol->kind == PARSE_SYM_TAG) {
        if (!symbol->type) {
            declaration_internal(owner, CORE_STRING_STATIC(
                    "link-unit type has no finalized semantic binding"));
            return false;
        }
        if (!core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("n00bb.semantic.type"),
                producer_stage, symbol->type)) {
            declaration_internal(owner, CORE_STRING_STATIC(
                "could not bind a declaration's semantic type"));
            return false;
        }
    }
    if (!core_link_declaration_binding(declaration,
            CORE_STRING_STATIC("n00bb.semantic.type"))) {
        th_type *semantic_type = symbol->type
            ? symbol->type : symbol->callable_contract;
        if (semantic_type
            && !core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("n00bb.semantic.type"),
                producer_stage, semantic_type)) {
            declaration_internal(owner, CORE_STRING_STATIC(
                "could not bind a declaration's semantic type"));
            return false;
        }
    }
    return true;
}

static bool
bind_link_declarations_to_ir(parse_grammar_t *grammar,
                             core_link_unit_t *link,
                             n00bb_link_analysis_t *analysis,
                             uint32_t producer_stage)
{
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        core_translation_unit_t *owner =
            core_link_declaration_owner(declaration);
        if (!bind_link_declaration(grammar, link, analysis, producer_stage,
                                   declaration)
            && !n00bb_unit_failed(owner))
            return false;
    }
    return true;
}
