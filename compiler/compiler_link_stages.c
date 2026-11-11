#include "compiler_cache.h"
#include "compiler_link.h"
#include "compiler_ir_setup.h"
#include "diagnostics.h"

static th_env *
build_interface_environment(n00bb_link_analysis_t *analysis)
{
    if (!analysis || !analysis->export_environment) return nullptr;
    const th_env *inputs[] = {analysis->export_environment};
    th_env *environment = n00bb_compose_environments(inputs, 1, nullptr,
        analysis->units.len ? analysis->units.data[0] : nullptr);
    if (environment && register_package_interfaces(analysis, environment))
        return environment;
    th_env_free(environment);
    return nullptr;
}

static void
report_stage_failure(core_translation_unit_t *const *units, size_t count,
                     core_translation_unit_t *unit, const char *stage,
                     const char *what)
{
    if (!call_units_failed(units, count))
        n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
                             (core_diag_location_t){}, stage, "%s", what);
}

static core_link_stage_result_t
stage_failed(n00bb_link_analysis_t *analysis, const char *stage,
             const char *what)
{
    if (analysis && analysis->units.len)
        report_stage_failure(analysis->units.data, analysis->units.len,
                             analysis->units.data[0], stage, what);
    return CORE_LINK_STAGE_RESULT_FAILED;
}

core_link_stage_result_t
n00bb_link_stage_ir_bindings(core_link_unit_t *link, uint32_t stage_kind,
                             void *opaque)
{
    n00bb_link_analysis_t *analysis = opaque;
    if (!n00bb_prepare_ir_bindings(link, analysis, stage_kind))
        return stage_failed(analysis, "binding declarations to IR",
                            "could not bind the program's declarations "
                            "to IR");
    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.ir-bindings"), 1, stage_kind,
        CORE_LINK_CONTRIBUTION_COMPLETE,
        CORE_STRING_STATIC("n00bb.ir-module-initialization"), 1, nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE
        : stage_failed(analysis, "binding declarations to IR",
                       "could not record the IR bindings");
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
            report_stage_failure(units->data, units->len, unit,
                "checking the program",
                "a module reached whole-program checking before its own "
                "checking finished");
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    if (!module_type_universe_is_shared(units)) {
        report_stage_failure(units->data, units->len, units->data[0],
            "checking the program",
            "the program's modules were checked with separate sets of "
            "types");
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        if (!validate_user_litmod_signatures(analysis->grammar,
                                             units->data[i]))
            return stage_failed(analysis, "checking literal modules",
                                "could not check a literal module's "
                                "handler signatures");
    for (size_t i = 0; i < units->len; i++)
        finalize_module_inference(units->data[i]);
    if (!finalize_named_declaration_types(analysis->grammar, units)) {
        report_stage_failure(units->data, units->len, units->data[0],
            "resolving type declarations",
            "could not resolve the program's type declarations");
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!check_local_names_against_imports(analysis))
        return CORE_LINK_STAGE_RESULT_FAILED;

    for (size_t i = 0; i < units->len; i++)
        if (!populate_unit_symbol_shapes(analysis->grammar,
                analysis->ir_context, units->data[i])) {
            report_stage_failure(units->data, units->len, units->data[i],
                "recording declaration layouts",
                "could not record the layout of a module's declarations");
            return CORE_LINK_STAGE_RESULT_FAILED;
        }

    th_env *environment = build_import_environment(analysis->grammar,
                                                   link, analysis);
    if (!environment) {
        report_stage_failure(units->data, units->len, units->data[0],
            "collecting declarations",
            "could not collect the program's types, traits and functions");
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!resolve_imported_module_symbols(analysis->grammar, units)) {
        report_stage_failure(units->data, units->len, units->data[0],
            "resolving names",
            "could not resolve the symbols the program imports");
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        finalize_module_inference(units->data[i]);
    for (size_t i = 0; i < units->len; i++) {
        core_translation_unit_t *unit = units->data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->types) continue;
        if (!th_ctx_set_environment_scope(state->types,
                unit->module.identifier, n00bb_package_id(unit))) {
            report_stage_failure(units->data, units->len, unit,
                "collecting declarations",
                "could not select the declarations visible to a module");
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    if (!resolve_type_member_references(link, analysis)) {
        report_stage_failure(units->data, units->len, units->data[0],
            "resolving names",
            "could not resolve the members the program names on types");
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    for (size_t i = 0; i < units->len; i++)
        (void)diagnose_reserved_declarations(units->data[i], environment);
    for (size_t i = 0; i < units->len; i++) {
        n00bb_tu_state_t *state = tu_state(units->data[i]);
        if (!state || !state->semantic_annotation_complete) {
            report_stage_failure(units->data, units->len, units->data[i],
                "checking the program",
                "a module's checking stopped before it finished");
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
        if (!validate_cached_array_dimensions(analysis->grammar,
                units->data[i], state)
            && !core_diagnostics_failed(
                core_translation_unit_diagnostics(units->data[i]))) {
            report_stage_failure(units->data, units->len, units->data[i],
                "checking array dimensions",
                "could not check the array dimensions a module caches");
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    for (size_t i = 0; i < units->len; i++)
        if (!populate_unit_symbol_shapes(analysis->grammar,
                analysis->ir_context, units->data[i])) {
            report_stage_failure(units->data, units->len, units->data[i],
                "recording declaration layouts",
                "could not record the layout of a module's declarations");
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
                report_stage_failure(units->data, units->len, owner,
                    "recording declaration facts",
                    "could not record the facts about a declaration");
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
        ir_shape_t *array_shape = parse_sym_binding(symbol,
            CORE_STRING_STATIC("ir.array-shape"));
        if (array_shape
            && !core_link_declaration_set_binding(link, declaration,
                CORE_STRING_STATIC("ir.array-shape"), stage_kind,
                array_shape)) {
            report_stage_failure(units->data, units->len, owner,
                "recording declaration facts",
                "could not record the shape of an array declaration");
            th_env_free(environment);
            return CORE_LINK_STAGE_RESULT_FAILED;
        }
    }
    analysis->interface_environment = build_interface_environment(analysis);
    if (!analysis->interface_environment) {
        report_stage_failure(units->data, units->len, units->data[0],
            "collecting package exports",
            "could not record what the program's packages export");
        th_env_free(environment);
        return CORE_LINK_STAGE_RESULT_FAILED;
    }
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.semantic-ready"), 1, stage_kind,
            CORE_LINK_CONTRIBUTION_COMPLETE,
            CORE_STRING_STATIC("n00bb.link-unit-semantic-analysis"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr)
        || !core_link_unit_add_unit_artifact(link,
            CORE_STRING_STATIC("n00bb.frozen-type-environment"), 1,
            stage_kind, environment, CORE_LINK_PAYLOAD_BORROWED, nullptr)) {
        report_stage_failure(units->data, units->len, units->data[0],
            "checking the program",
            "could not record the result of whole-program checking");
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
        return stage_failed(analysis, "selecting storage classes",
                            "could not select the storage classes of the "
                            "program's variables");
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
            return stage_failed(analysis, "recording analysis coverage",
                                "could not record which analyses are "
                                "unavailable");
    }
    bool array_shapes_complete = true;
    for (size_t i = 0; i < core_link_unit_declaration_count(link); i++) {
        core_link_declaration_t *declaration =
            core_link_unit_declaration_at(link, i);
        if (core_link_declaration_is_external_requirement(declaration))
            continue;
        n00bb_link_payload_t *payload = core_link_declaration_binding(
            declaration,
            CORE_STRING_STATIC("n00bb.semantic.declaration-facts"));
        array_shapes_complete &= payload && payload->array_shape_complete;
    }
    if (!core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.array-shapes"), 1, stage_kind,
            array_shapes_complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                                  : CORE_LINK_CONTRIBUTION_UNAVAILABLE,
            CORE_STRING_STATIC("n00bb.canonical-declaration-facts"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return stage_failed(analysis, "recording array shapes",
                            "could not record the program's array shapes");
    core_link_contribution_state_t call_graph_state;
    if (!publish_link_call_dependencies(link, analysis, stage_kind,
                                        &call_graph_state)
        || !core_link_unit_set_contribution(link, (core_string_t){0},
            CORE_STRING_STATIC("n00bb.call-graph"), 1, stage_kind,
            call_graph_state,
            CORE_STRING_STATIC("n00bb.post-inference-call-sites"), 1,
            nullptr, CORE_LINK_PAYLOAD_BORROWED, nullptr))
        return stage_failed(analysis, "building the call graph",
                            "could not record the program's call graph");
    if (!register_package_module_metadata(analysis))
        return stage_failed(analysis, "collecting package exports",
                            "could not record the package modules' "
                            "metadata");
    return core_link_unit_set_contribution(link, (core_string_t){0},
        CORE_STRING_STATIC("n00bb.bootstrap-pipeline-ran"), 1, stage_kind,
        CORE_LINK_CONTRIBUTION_COMPLETE,
        CORE_STRING_STATIC("n00bb.bootstrap-pipeline"), 1, nullptr,
        CORE_LINK_PAYLOAD_BORROWED, nullptr)
        ? CORE_LINK_STAGE_RESULT_COMPLETE
        : stage_failed(analysis, "checking the program",
                       "could not record that whole-program checking ran");
}
