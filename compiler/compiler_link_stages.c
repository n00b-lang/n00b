#include "compiler_cache.h"
#include "compiler_link.h"
#include "compiler_link_lifetime.h"
#include "compiler_ir_setup.h"

core_link_stage_result_t
n00bb_link_stage_ir_bindings(core_link_unit_t *link, uint32_t stage_kind,
                             void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    if (!n00bb_prepare_ir_bindings(link, analysis, stage_kind))
        return CORE_LINK_STAGE_RESULT_FAILED;
    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.ir-bindings"), 1, stage_kind,
        CORE_LINK_CONTRIBUTION_COMPLETE,
        CORE_STRING_STATIC("n00bb.ir-module-initialization"), 1, nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}

core_link_stage_result_t
n00bb_link_stage_semantics(core_link_unit_t *link, uint32_t stage_kind,
                           void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    if (!analysis || !analysis->units.len)
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_list_t(core_translation_unit_ptr_t) *units = &analysis->units;
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->semantic_annotation_complete) {
            core_diagnostics_report(
                core_translation_unit_diagnostics(unit), unit,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0047"),
                CORE_STRING_STATIC(
                    "link-unit semantic analysis requires every module to complete semantic annotation"));
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
        if (core_diagnostics_failed(
                core_translation_unit_diagnostics(unit)))
            return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!module_type_universe_is_shared(units)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0054"),
            CORE_STRING_STATIC("link-unit modules do not share a type universe"));
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        if (!validate_user_litmod_signatures(analysis->grammar,
                                             units->data[i]))
            return CORE_LINK_STAGE_RESULT_FAILED;
    for (size_t i = 0; i < units->len; i++)
        finalize_module_inference(units->data[i]);
    if (!finalize_named_declaration_types(analysis->grammar, units)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0055"),
            CORE_STRING_STATIC("could not finalize named declaration types"));
        return CORE_LINK_STAGE_RESULT_FAILED;
    }

    for (size_t i = 0; i < units->len; i++)
        if (!populate_unit_symbol_shapes(analysis->grammar,
                analysis->ir_context, units->data[i])) {
            core_diagnostics_report(core_translation_unit_diagnostics(
                units->data[i]), units->data[i], CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-L0056"),
                CORE_STRING_STATIC("could not populate canonical symbol shapes"));
            return CORE_LINK_STAGE_RESULT_FAILED;
        }

    th_env *environment = build_import_environment(analysis->grammar,
                                                   link, analysis);
    if (!environment) {
        if (!call_units_failed(units->data, units->len))
            core_diagnostics_report(core_translation_unit_diagnostics(
                units->data[0]), units->data[0], CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-L0057"),
                CORE_STRING_STATIC("could not build the link-unit type environment"));
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!generalize_link_unit(analysis->grammar, link, units)) {
        if (!call_units_failed(units->data, units->len))
            core_diagnostics_report(core_translation_unit_diagnostics(
                units->data[0]), units->data[0], CORE_DIAG_FATAL,
                CORE_STRING_STATIC("N00BB-L0058"),
                CORE_STRING_STATIC(
                    "could not finalize link-unit signatures"));
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) continue;
        if (!th_ctx_set_environment_scope(state->types,
                unit->module.identifier, n00bb_package_id(unit))) {
            core_diagnostics_report(core_translation_unit_diagnostics(unit),
                unit, CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0059"),
                CORE_STRING_STATIC("could not set module type-environment scope"));
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    if (!unify_calls_until_stable(analysis->grammar, link, units->data,
            units->len, true, true, false)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        if (!resolve_for_binding_types(analysis->grammar,
                                       units->data[i])) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!resolve_variant_case_references(link, analysis)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0050"),
            CORE_STRING_STATIC("variant case resolution failed"));
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!diagnose_known_missing_record_fields(analysis)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!resolve_inferred_field_references(link, analysis)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0051"),
            CORE_STRING_STATIC("field reference resolution failed"));
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    mark_resolved_flagset_operator_sites(analysis->grammar, units);
    if (!resolve_inferred_trait_references(analysis->grammar, link,
                                           analysis, environment)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0052"),
            CORE_STRING_STATIC("trait reference resolution failed"));
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!n00bb_cache_check_dispatch(analysis->cache, analysis,
                                    environment)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!resolve_generic_trait_specializations(link, analysis,
                                                stage_kind)) {
        core_diagnostics_report(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], CORE_DIAG_FATAL,
            CORE_STRING_STATIC("N00BB-L0053"),
            CORE_STRING_STATIC("generic trait specialization failed"));
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!unify_calls_until_stable(analysis->grammar, link, units->data,
            units->len, true, true, false)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++) {
        n00bb_tu_state_t *state = tu_state(units->data[i]);
        if (!state || !state->semantic_annotation_complete) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
        if (!validate_cached_array_dimensions(analysis->grammar,
                units->data[i], state)
            || core_diagnostics_failed(
                core_translation_unit_diagnostics(units->data[i]))) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    record_link_unit_call_result_aliases(analysis);
    if (!record_call_argument_escape_facts(analysis)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        if (!finalize_alias_free_local_summaries(
                analysis->grammar, units->data[i])) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    for (size_t i = 0; i < units->len; i++)
        if (!populate_unit_symbol_shapes(analysis->grammar,
                analysis->ir_context, units->data[i])) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        if (core_link_declaration_is_external_requirement(declaration))
            continue;
        if (core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.builtin-symbol"))) continue;
        n00bb_link_payload_t *payload = declaration
            ? core_link_declaration_payload(declaration) : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        core_translation_unit_t *owner = declaration
            ? core_link_declaration_owner(declaration) : nullptr;
        if (!owner && symbol)
            owner = unit_for_symbol(analysis->grammar, units, symbol);
        if (!payload || !symbol || !owner
            || !populate_link_declaration_facts(analysis->grammar,
                    analysis->ir_context, owner, payload)
            || !core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"),
                stage_kind, payload)) {
            if (owner)
                core_diagnostics_report(
                    core_translation_unit_diagnostics(owner), owner,
                    CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0045"),
                    CORE_STRING_STATIC(
                        "could not publish complete canonical declaration facts"));
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
        ir_shape_t *array_shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
        if (array_shape
            && !core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("ir.array-shape"), stage_kind,
                array_shape)) {
            core_diagnostics_report(
                core_translation_unit_diagnostics(owner), owner,
                CORE_DIAG_FATAL, CORE_STRING_STATIC("N00BB-L0045"),
                CORE_STRING_STATIC(
                    "could not publish canonical array shape metadata"));
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    analysis->interface_environment =
        build_function_lifetime_environment(analysis);
    if (!analysis->interface_environment) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        (void)n00bb_check_deferred_conversions(units->data[i]);
    for (size_t i = 0; i < units->len; i++) {
        if (core_diagnostics_failed(core_translation_unit_diagnostics(
                units->data[i]))) {
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.semantic-ready"), 1, stage_kind,
            CORE_LINK_CONTRIBUTION_COMPLETE,
            CORE_STRING_STATIC("n00bb.link-unit-semantic-analysis"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!core_link_unit_add_unit_artifact(link,
            CORE_STRING_STATIC("n00bb.frozen-type-environment"), 1,
            stage_kind, environment, CORE_LINK_PAYLOAD_BORROWED, nullptr)) {
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    analysis->environment = environment;
    return CORE_LINK_STAGE_RESULT_COMPLETE;
}

core_link_stage_result_t
n00bb_link_stage_contributions(core_link_unit_t *link, uint32_t stage_kind,
                               void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    if (!analysis || !select_storage_classes(&analysis->units))
        return CORE_LINK_STAGE_RESULT_FAILED;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        n00bb_link_payload_t *payload = declaration
            ? core_link_declaration_binding(declaration,
                CORE_STRING_STATIC("n00bb.semantic.declaration-facts"))
            : nullptr;
        parse_sym_entry_t *symbol = payload ? payload->symbol : nullptr;
        if (!payload || !symbol) continue;
        payload->requested_storage_class = symbol->requested_storage_class;
        payload->selected_storage_class = symbol->selected_storage_class;
        payload->storage_class_required = symbol->storage_class_required;
        payload->storage_duration = symbol->storage_duration;
    }
    static const core_string_t unavailable_domains[] = {
        CORE_STRING_STATIC("n00bb.finalization"),
    };
    for (size_t i = 0;
         i < sizeof(unavailable_domains) / sizeof(unavailable_domains[0]);
         i++) {
        if (!core_link_unit_set_contribution(link, (core_string_t){0},
                unavailable_domains[i], 1, stage_kind,
                CORE_LINK_CONTRIBUTION_UNAVAILABLE,
                CORE_STRING_STATIC("n00bb.analysis-coverage"), 1, nullptr,
                CORE_LINK_PAYLOAD_BORROWED, nullptr))
            return CORE_LINK_STAGE_RESULT_FAILED;
    }
    bool phase_values_complete = true;
    bool array_shapes_complete = true;
    bool storage_lifetimes_complete = false;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        if (core_link_declaration_is_external_requirement(declaration))
            continue;
        n00bb_link_payload_t *payload = core_link_declaration_binding(
            declaration,
            CORE_STRING_STATIC("n00bb.semantic.declaration-facts"));
        phase_values_complete &= payload && payload->facts_complete
            && payload->phase_value_complete
            && payload->initializer_facts_complete;
        array_shapes_complete &= payload && payload->array_shape_complete;
    }
    const struct {
        core_string_t domain;
        bool complete;
    } semantic_domains[] = {
        {CORE_STRING_STATIC("n00bb.phase-values"), phase_values_complete},
        {CORE_STRING_STATIC("n00bb.array-shapes"), array_shapes_complete},
        {CORE_STRING_STATIC("n00bb.storage-lifetimes"),
         storage_lifetimes_complete},
    };
    for (size_t i = 0;
         i < sizeof(semantic_domains) / sizeof(semantic_domains[0]); i++) {
        if (!core_link_unit_set_contribution(link, (core_string_t){0},
                semantic_domains[i].domain, 1,
                stage_kind,
                semantic_domains[i].complete
                    ? CORE_LINK_CONTRIBUTION_COMPLETE
                    : CORE_LINK_CONTRIBUTION_UNAVAILABLE,
                CORE_STRING_STATIC("n00bb.canonical-declaration-facts"), 1,
                nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
            return CORE_LINK_STAGE_RESULT_FAILED;
    }
    core_link_contribution_state_t call_graph_state;
    if (!publish_link_call_dependencies(link, analysis, stage_kind,
                                        &call_graph_state)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.call-graph"), 1, stage_kind,
            call_graph_state,
            CORE_STRING_STATIC("n00bb.post-inference-call-sites"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_link_contribution_state_t direct_effect_state;
    if (!publish_function_global_effects(link, analysis, stage_kind,
                                         &direct_effect_state)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.function-direct-global-effects"), 1,
            stage_kind, direct_effect_state,
            CORE_STRING_STATIC("n00bb.annotation-reference-sites"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_link_contribution_state_t call_effect_state;
    if (!publish_function_effect_summaries(link, analysis, stage_kind,
            direct_effect_state, call_graph_state, &call_effect_state)
        || !register_context_effect_metadata(analysis, link)
        || !register_package_context_effect_metadata(analysis)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.call-effects"), 1,
            stage_kind, call_effect_state,
            CORE_STRING_STATIC("n00bb.canonical-call-fixed-point"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_link_contribution_state_t direct_initializer_state;
    if (!publish_link_initializer_uses(link, analysis, stage_kind,
                                       &direct_initializer_state)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.initializer-direct-uses"), 1,
            stage_kind, direct_initializer_state,
            CORE_STRING_STATIC("n00bb.annotation-reference-sites"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.initializer-phase-uses"), 1,
            stage_kind, direct_initializer_state,
            CORE_STRING_STATIC("n00bb.phase-classified-initializer-uses"),
            1, nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_link_contribution_state_t initializer_cycle_state;
    if (!validate_initializer_cycles(link, analysis,
                                     &initializer_cycle_state)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.initializer-cycle-check"), 1,
            stage_kind, initializer_cycle_state,
            CORE_STRING_STATIC("n00bb.phase-initializer-dependency-graph"),
            1, nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    if (initializer_cycle_state == CORE_LINK_CONTRIBUTION_FAILED)
        return CORE_LINK_STAGE_RESULT_FAILED;
    core_link_contribution_state_t order_state = initializer_cycle_state;
    n00bb_initializer_order_t *order = initializer_cycle_state
            == CORE_LINK_CONTRIBUTION_COMPLETE
        ? build_initializer_order(link, analysis, &order_state)
        : nullptr;
    if (!order && order_state == CORE_LINK_CONTRIBUTION_COMPLETE)
        return CORE_LINK_STAGE_RESULT_FAILED;
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC(N00BB_INITIALIZER_ORDER_DOMAIN), 1,
            stage_kind, order_state,
            CORE_STRING_STATIC("n00bb.link-unit-scc-order"), 1,
            order, order ? CORE_LINK_PAYLOAD_OWNED
                         : CORE_LINK_PAYLOAD_BORROWED,
            order ? free_initializer_order : nullptr)) {
        if (order) free_initializer_order(order);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (order_state == CORE_LINK_CONTRIBUTION_FAILED)
        return CORE_LINK_STAGE_RESULT_FAILED;
    bool initializer_orders_complete =
        order_state == CORE_LINK_CONTRIBUTION_COMPLETE;
    core_link_contribution_state_t initializer_dependencies_state =
        direct_initializer_state == CORE_LINK_CONTRIBUTION_COMPLETE
            && initializer_cycle_state == CORE_LINK_CONTRIBUTION_COMPLETE
            && initializer_orders_complete
        ? CORE_LINK_CONTRIBUTION_COMPLETE
        : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.initializer-dependencies"), 1,
            stage_kind, initializer_dependencies_state,
            CORE_STRING_STATIC("n00bb.phase-initializer-dependencies"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return CORE_LINK_STAGE_RESULT_FAILED;
    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.bootstrap-pipeline-ran"), 1, stage_kind,
        CORE_LINK_CONTRIBUTION_COMPLETE,
        CORE_STRING_STATIC("n00bb.bootstrap-pipeline"), 1, nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE : CORE_LINK_STAGE_RESULT_FAILED;
}
