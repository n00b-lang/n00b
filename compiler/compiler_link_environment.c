#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "lib/buffer.h"

typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

static void
dispatch_argument_nodes(parse_grammar_t *grammar, parse_tree_t *node,
                        core_list_t(n00bb_tree_ptr_t) *arguments)
{
    core_string_t trait = {0}, method = {0};
    if (index_dispatch_for_node(grammar, node, &trait, &method)) {
        parse_tree_t *contents = core_tree_child(node, 2);
        for (size_t i = 0; !core_tree_is_leaf(contents)
             && i < core_tree_num_children(contents); i++) {
            parse_tree_t *child = core_tree_child(contents, i);
            if (nt_is(grammar, child, "expression"))
                core_list_push(*arguments, child);
        }
        parse_tree_t *value = n00bb_index_stored_value(grammar, node);
        if (value) core_list_push(*arguments, value);
        return;
    }
    parse_tree_t *call = parse_tree_parent(node);
    if (!nt_is(grammar, call, "postfix-expr")
        || core_tree_child(call, 0) != node) return;
    collect_positional_nodes(grammar,
        child_nt(grammar, call_arguments_node(grammar, call),
                 "positional-args"), arguments);
}

static parse_tree_t *
dispatch_string_where_buffer_expected(parse_grammar_t *grammar,
                                      th_ctx *types, parse_tree_t *site,
                                      th_type *receiver,
                                      const char *method_name)
{
    th_env_method_t method = {0};
    const char *trait = nullptr;
    if (!site || core_tree_is_leaf(site) || !method_name
        || th_env_method_resolve_by_name(types, receiver, method_name,
               &method, &trait) != TH_ENV_METHOD_FOUND
        || !method.signature) return nullptr;
    th_type *signature = th_instantiate(types,
        th_type_clone(types, method.signature));
    const th_param *self = signature ? th_func_param(signature, 0) : nullptr;
    if (!self || !th_unify_in_place(types, self->type, receiver))
        return nullptr;
    core_list_t(n00bb_tree_ptr_t) arguments = core_list_new(n00bb_tree_ptr_t);
    dispatch_argument_nodes(grammar, site, &arguments);
    parse_tree_t *offending = nullptr;
    if (arguments.len < th_func_nparams(signature))
        for (size_t i = 0; !offending && i < arguments.len; i++) {
            parse_tree_t *argument = arguments.data[i];
            if (!core_tree_is_leaf(argument)
                && n00bb_string_where_buffer_expected(types,
                       th_func_param(signature, i + 1)->type,
                       parse_tree_type(argument)))
                offending = argument;
        }
    core_list_free(arguments);
    return offending;
}

static bool
detail_spans_unit(const th_error_t *detail, core_translation_unit_t *unit)
{
    return detail->span.file && detail->span.start_line
        && core_string_eq(core_source_file_path(&unit->source),
                          core_string_from_cstr(detail->span.file));
}

static bool
site_dispatches_method(const n00bb_link_use_site_t *site,
                       const char *method_name)
{
    core_string_t name = site->dispatch_method.data ? site->dispatch_method
        : site->receiver_node ? site->member_name : (core_string_t){0};
    return core_string_eq(name, core_string_from_cstr(method_name));
}

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
            core_translation_unit_t *failure_unit = nullptr;
            th_type *failure_receiver = nullptr;
            parse_tree_t *open_site = nullptr;
            core_translation_unit_t *open_unit = nullptr;
            for (size_t pass = 0; pass < 2; pass++)
            for (size_t u = 0; has_detail && detail.constraint && detail.got
                 && !failure_site && u < units->len; u++) {
                n00bb_tu_state_t *owner = tu_state(units->data[u]);
                if (!pass && !detail_spans_unit(&detail, units->data[u]))
                    continue;
                for (size_t s = 0; owner && s < owner->link_use_sites.len;
                     s++) {
                    n00bb_link_use_site_t *site =
                        &owner->link_use_sites.data[s];
                    if (!site_dispatches_method(site, detail.constraint)
                        || (!pass && link_source_location(site->node).line
                                != detail.span.start_line))
                        continue;
                    parse_tree_t *receiver = site->receiver_node;
                    if (!receiver && site->dispatch_trait.data
                        && site->node && !core_tree_is_leaf(site->node))
                        receiver = operator_receiver_node(site);
                    th_type *receiver_type = receiver
                        ? parse_tree_type(receiver) : nullptr;
                    if (receiver_type && th_unify_probe(state->types,
                            receiver_type, detail.got)) {
                        failure_site = site->node;
                        failure_unit = units->data[u];
                        failure_receiver = receiver_type;
                        break;
                    }
                    if (!open_site && receiver_type
                        && th_kind_of(th_resolve(receiver_type)) == TH_VAR) {
                        open_site = site->node;
                        open_unit = units->data[u];
                    }
                }
            }
            if (!failure_site) {
                failure_site = open_site;
                failure_unit = open_unit;
            }
            for (size_t u = 0; !failure_unit && has_detail
                 && detail.span.file && u < units->len; u++) {
                core_string_t path = core_source_file_path(
                    &units->data[u]->source);
                if (core_string_eq(path,
                        core_string_from_cstr(detail.span.file)))
                    failure_unit = units->data[u];
            }
            if (failure_unit) unit = failure_unit;
            parse_tree_t *string_argument = failure_receiver
                ? dispatch_string_where_buffer_expected(grammar,
                    state->types, failure_site, failure_receiver,
                    detail.constraint)
                : nullptr;
            if (string_argument) {
                core_diagnostics_report_at(
                    core_translation_unit_diagnostics(unit), unit,
                    CORE_DIAG_ERROR, CORE_STRING_STATIC("N00BB-T0036"),
                    CORE_STRING_STATIC(n00bb_string_to_buffer_text),
                    link_source_location(string_argument));
                ok = false;
                break;
            }
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
