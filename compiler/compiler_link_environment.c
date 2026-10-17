#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "lib/buffer.h"

typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

th_env *
build_import_environment(parse_grammar_t *grammar,
                         core_link_unit_t *link,
                         n00bb_link_analysis_t *analysis)
{
    core_list_t(core_translation_unit_ptr_t) *units = &analysis->units;
    const th_env *const *linked_interfaces = analysis->linked_interfaces;
    size_t linked_interface_count = analysis->linked_interface_count;
    n00bb_tu_state_t *root_state = units->len
        ? tu_state(units->data[0]) : nullptr;
    th_env *source_environment = root_state && root_state->types
        ? th_env_new_in_context(root_state->types) : nullptr;
    bool ok = source_environment && root_state && root_state->types;
    for (size_t i = 0; ok && i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        ok = register_unit_environment(grammar, source_environment, unit);
        if (!ok && !core_diagnostics_failed(
                core_translation_unit_diagnostics(unit)))
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0060"),
                CORE_STRING_STATIC("could not register module declarations in the type environment"));
    }
    core_list_t(th_type_ptr_t) bracket_results =
        core_list_new(th_type_ptr_t);
    core_list_t(th_type_ptr_t) brace_results =
        core_list_new(th_type_ptr_t);
    bool bracket_trait = false;
    bool brace_trait = false;
    for (size_t i = 0; ok && i < units->len; i++) {
        n00bb_tu_state_t *state = tu_state(units->data[i]);
        if (!state) continue;
        for (size_t r = 0; ok && r < state->literal_modifiers.len; r++) {
            n00bb_user_litmod_t *registration =
                &state->literal_modifiers.data[r];
            bool bracket = core_string_eq(registration->kind,
                                           CORE_STRING_STATIC("array"));
            bool brace = core_string_eq(registration->kind,
                                         CORE_STRING_STATIC("dict"))
                || core_string_eq(registration->kind,
                                   CORE_STRING_STATIC("set"));
            if (!bracket && !brace) continue;
            const char *trait = bracket ? "Bracket_Literal"
                                        : "Brace_Literal";
            bool *declared = bracket ? &bracket_trait : &brace_trait;
            core_list_t(th_type_ptr_t) *seen = bracket
                ? &bracket_results : &brace_results;
            if (!*declared) {
                th_type *methods = th_record(root_state->types, nullptr, 0);
                ok = methods && th_env_register_trait_contracts(
                    source_environment, trait, "builtins", "default",
                    TH_ENV_PUBLIC, methods);
                if (!ok) break;
                *declared = true;
            }
            th_type *result = registration->function
                ? th_func_ret(registration->function->type) : nullptr;
            result = result ? th_resolve(result) : nullptr;
            if (result && th_kind_of(result) != TH_VAR) {
                bool duplicate = false;
                for (size_t n = 0; n < seen->len; n++)
                    duplicate |= seen->data[n] == result;
                if (!duplicate) {
                    ok = th_env_register_intrinsic_type_pattern(
                        source_environment, trait, result);
                    if (ok) core_list_push(*seen, result);
                }
            }
            for (size_t i = 0; i < state->literal_site_nodes.len; i++) {
                n00bb_litmod_site_t *site = n00bb_node_literal_site(
                    state->literal_site_nodes.data[i]);
                if (!site || site->registration != registration)
                    continue;
                result = site->signature
                    ? th_resolve(th_func_ret(site->signature)) : nullptr;
                if (!result || th_kind_of(result) == TH_VAR) continue;
                bool duplicate = false;
                for (size_t n = 0; n < seen->len; n++)
                    duplicate |= seen->data[n] == result;
                if (!duplicate) {
                    ok = th_env_register_intrinsic_type_pattern(
                        source_environment, trait, result);
                    if (!ok) break;
                    core_list_push(*seen, result);
                }
            }
        }
    }
    core_list_free(brace_results);
    core_list_free(bracket_results);
    if (ok) ok = th_env_freeze(source_environment);
    if (!ok) {
        th_env_free(source_environment);
        return nullptr;
    }
    th_env *export_environment = source_environment;
    if (linked_interface_count) {
        const th_env **inputs = core_alloc_array(const th_env *,
                                                  linked_interface_count + 1);
        inputs[0] = source_environment;
        for (size_t i = 0; i < linked_interface_count; i++)
            inputs[i + 1] = linked_interfaces[i];
        core_diagnostics_t *diagnostics = core_translation_unit_diagnostics(
            units->data[0]);
        th_env_compose_error_t compose_error = {0};
        export_environment = th_env_compose_report_in_context(inputs,
            linked_interface_count + 1, root_state->types, diagnostics,
            &compose_error);
        core_free(inputs);
        th_env_free(source_environment);
        if (!export_environment || !th_env_freeze(export_environment)) {
            th_env_free(export_environment);
            return nullptr;
        }
    }
    th_env *builtins = root_state && root_state->types
        ? th_env_new_in_context(root_state->types) : nullptr;
    bool builtins_ready = builtins && root_state && root_state->types
        && register_builtin_traits(builtins, root_state->types,
                                   &root_state->semantic_values)
        && th_env_freeze(builtins);
    if (!builtins_ready)
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0061"),
            CORE_STRING_STATIC("could not register builtin trait contracts"));
    const th_env *base_inputs[] = {builtins, export_environment};
    th_env *base = builtins_ready
        ? th_env_compose_report_in_context(base_inputs, 2,
            root_state->types,
            core_translation_unit_diagnostics(units->data[0]), nullptr)
        : nullptr;
    th_env_free(builtins);
    if (!base || !th_env_freeze(base)) {
        th_env_free(base);
        th_env_free(export_environment);
        return nullptr;
    }
    th_env *defaults = th_env_new_in_context(root_state->types);
    bool defaults_ready = defaults
        && register_builtin_value_defaults(defaults, base, link, grammar,
            analysis)
        && th_env_freeze(defaults);
    const th_env *environment_inputs[] = {base, defaults};
    th_env *environment = defaults_ready
        ? th_env_compose_report_in_context(environment_inputs, 2,
            root_state->types,
            core_translation_unit_diagnostics(units->data[0]), nullptr)
        : nullptr;
    th_env_free(defaults);
    th_env_free(base);
    if (!environment || !th_env_freeze(environment)) {
        th_env_free(environment);
        th_env_free(export_environment);
        return nullptr;
    }
    analysis->export_environment = export_environment;
    bool environment_attached = false;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) continue;
        th_ctx_set_method_argument_coercions(state->types,
            TH_COERCE_OPTION_WRAP | TH_COERCE_NIL_OPTION
            | TH_COERCE_RECORD_CONSTRUCT | TH_COERCE_OPTION_UNWRAP
            | TH_COERCE_CUSTOM);
        n00bb_register_value_coercions(state->types);
        bool attached = environment_attached
            ? th_ctx_set_environment_scope(state->types,
                unit->module.identifier.data,
                n00bb_package_id(unit).data)
            : th_ctx_set_environment(state->types, environment,
                unit->module.identifier.data,
                n00bb_package_id(unit).data);
        if (!attached) {
            size_t error_count = th_error_count(state->types);
            th_error_t detail = {0};
            bool has_detail = error_count &&
                th_error_at(state->types, error_count - 1, &detail);
            parse_tree_t *failure_site = nullptr;
            parse_tree_t *open_site = nullptr;
            if (has_detail && detail.constraint && detail.got)
                for (size_t s = 0; s < state->link_use_sites.len; s++) {
                    n00bb_link_use_site_t *site =
                        &state->link_use_sites.data[s];
                    if (!core_string_eq(site->dispatch_method,
                            core_string_from_cstr(detail.constraint)))
                        continue;
                    parse_tree_t *receiver = site->receiver_node;
                    if (!receiver && site->dispatch_trait.data
                        && site->node && !core_tree_is_leaf(site->node))
                        receiver = operator_receiver_node(site);
                    th_type *receiver_type = receiver
                        && !core_tree_is_leaf(receiver)
                        ? core_tree_node_value(receiver).type : nullptr;
                    if (receiver_type && th_unify_probe(state->types,
                            receiver_type, detail.got)) {
                        failure_site = site->node;
                        break;
                    }
                    if (!open_site && receiver_type
                        && th_kind_of(th_resolve(receiver_type)) == TH_VAR)
                        open_site = site->node;
                }
            if (!failure_site) failure_site = open_site;
            core_buffer_t message = {0};
            if (has_detail && detail.constraint) {
                core_buffer_printf(&message,
                    "trait constraint '%s' is not satisfied for %s: %s",
                    detail.constraint,
                    detail.got ? n00bb_print_type(state->types, detail.got)
                               : "this type",
                    detail.message ? detail.message
                                   : "constraint failure");
                if (detail.expected)
                    core_buffer_printf(&message, " (expected %s)",
                        n00bb_print_type(state->types, detail.expected));
                th_env_method_requirement_t requirements[16] = {0};
                size_t requirement_count = th_env_method_requirements(
                    state->types, detail.constraint, requirements, 16);
                for (size_t r = 0; r < requirement_count && r < 16; r++) {
                    size_t candidates = th_env_method_candidates(
                        state->types, requirements[r].trait_name,
                        detail.constraint, nullptr, 0);
                    core_buffer_printf(&message,
                        "; %s has %zu matching candidate%s",
                        requirements[r].trait_name, candidates,
                        candidates == 1 ? "" : "s");
                }
            }
            else {
                core_buffer_puts(&message,
                    "trait constraint is not satisfied in the loaded module graph");
                if (has_detail && detail.message)
                    core_buffer_printf(&message, ": %s", detail.message);
                if (has_detail && detail.got)
                    core_buffer_printf(&message, " (for %s)",
                        n00bb_print_type(state->types, detail.got));
            }
            if (failure_site)
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                    core_string_borrow(message.data, message.byte_len),
                    link_source_location(failure_site));
            else if (has_detail && detail.span.start_line)
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                    core_string_borrow(message.data, message.byte_len),
                    (core_diag_location_t){
                        .line = detail.span.start_line,
                        .column = detail.span.start_col,
                        .end_line = detail.span.end_line,
                        .end_column = detail.span.end_col,
                        .has_location = true,
                    });
            else
                core_diagnostics_report(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0003"),
                    core_string_borrow(message.data, message.byte_len));
            core_buffer_release(&message);
            ok = false;
            break;
        }
        else environment_attached = true;
    }
    if (!ok) {
        th_env_free(environment);
        th_env_free(analysis->export_environment);
        analysis->export_environment = nullptr;
        th_env_free(analysis->interface_environment);
        analysis->interface_environment = nullptr;
        return nullptr;
    }
    return environment;
}
