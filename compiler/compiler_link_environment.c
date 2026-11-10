#include <string.h>

#include "compiler_link.h"
#include "compiler_tu.h"
#include "diagnostics.h"
#include "lib/buffer.h"

typedef th_type *th_type_ptr_t;
core_list_decl(th_type_ptr_t);

th_env *
n00bb_compose_environments(const th_env *const *inputs, size_t count,
                           th_ctx *types, core_translation_unit_t *unit)
{
    th_env_compose_error_t error = {0};
    th_env *composed = th_env_compose_report_in_context(inputs, count, types,
                                                        nullptr, &error);
    if (error.status == TH_ENV_COMPOSE_OK || !unit) return composed;
    static const char *const kinds[] = {
        [TH_ENV_COMPOSE_INVALID_SOURCE] = "an unusable set of declarations",
        [TH_ENV_COMPOSE_TYPE_CONFLICT] = "two different types",
        [TH_ENV_COMPOSE_TRAIT_CONFLICT] = "two different traits",
        [TH_ENV_COMPOSE_FUNCTION_CONFLICT] = "two different functions",
        [TH_ENV_COMPOSE_METADATA_CONFLICT] = "conflicting facts",
        [TH_ENV_COMPOSE_VALUE_CONFLICT] = "two different values",
    };
    core_string_t identity = error.identity.data ? error.identity
        : CORE_STRING_STATIC("a declaration");
    n00bb_internal_error(core_translation_unit_diagnostics(unit), unit,
        (core_diag_location_t){}, "combining module interfaces",
        "found %s named `%.*s`", kinds[error.status],
        (int)identity.u8_bytes, identity.data);
    return composed;
}

typedef struct {
    core_string_t trait;
    bool declared;
    core_list_t(th_type_ptr_t) seen;
} shape_trait_patterns_t;
core_list_decl(shape_trait_patterns_t);

static shape_trait_patterns_t *
shape_trait_patterns(core_list_t(shape_trait_patterns_t) *shapes,
                     core_string_t trait)
{
    for (size_t i = 0; i < shapes->len; i++)
        if (core_string_eq(shapes->data[i].trait, trait))
            return &shapes->data[i];
    core_list_push(*shapes, ((shape_trait_patterns_t){
        .trait = trait, .seen = core_list_new(th_type_ptr_t)}));
    return &shapes->data[shapes->len - 1];
}

static bool
register_shape_trait(th_ctx *types, th_env *environment,
                     shape_trait_patterns_t *patterns)
{
    if (patterns->declared) return true;
    th_type *methods = th_record(types, nullptr, 0);
    patterns->declared = methods && th_env_register_trait_contracts(
        environment, patterns->trait, CORE_STRING_STATIC("builtins"),
        CORE_STRING_STATIC("default"), TH_ENV_PUBLIC, methods);
    return patterns->declared;
}

static bool
register_shape_pattern(th_env *environment, shape_trait_patterns_t *patterns,
                       th_type *result)
{
    for (size_t n = 0; n < patterns->seen.len; n++)
        if (patterns->seen.data[n] == result) return true;
    if (!th_env_register_intrinsic_type_pattern(environment, patterns->trait,
                                                result))
        return false;
    core_list_push(patterns->seen, result);
    return true;
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
            n00bb_internal_error(core_translation_unit_diagnostics(unit),
                unit, (core_diag_location_t){}, "collecting declarations",
                "could not record the declarations of module `%.*s`",
                (int)unit->module.identifier.u8_bytes,
                unit->module.identifier.data);
    }
    core_list_t(shape_trait_patterns_t) shapes =
        core_list_new(shape_trait_patterns_t);
    for (size_t i = 0; ok && i < units->len; i++) {
        n00bb_tu_state_t *state = tu_state(units->data[i]);
        if (!state) continue;
        for (size_t r = 0; ok && r < state->literal_modifiers.len; r++) {
            n00bb_user_litmod_t *registration =
                &state->literal_modifiers.data[r];
            core_string_t trait = n00bb_literal_shape_trait(
                registration->kind);
            if (trait.data) {
                shape_trait_patterns_t *patterns = shape_trait_patterns(
                    &shapes, trait);
                ok = patterns && register_shape_trait(root_state->types,
                    source_environment, patterns);
                th_type *result = registration->function
                    ? th_func_ret(registration->function->type) : nullptr;
                result = result ? th_resolve(result) : nullptr;
                if (ok && result && th_kind_of(result) != TH_VAR)
                    ok = register_shape_pattern(source_environment,
                                                patterns, result);
                for (size_t i = 0; ok && i < state->literal_site_nodes.len;
                     i++) {
                    n00bb_litmod_site_t *site = n00bb_node_literal_site(
                        state->literal_site_nodes.data[i]);
                    if (!site || site->registration != registration)
                        continue;
                    result = site->signature
                        ? th_resolve(th_func_ret(site->signature)) : nullptr;
                    if (result && th_kind_of(result) != TH_VAR)
                        ok = register_shape_pattern(source_environment,
                                                    patterns, result);
                }
            }
        }
    }
    for (size_t t = 0; t < shapes.len; t++)
        core_list_free(shapes.data[t].seen);
    core_list_free(shapes);
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
        export_environment = n00bb_compose_environments(inputs,
            linked_interface_count + 1, root_state->types, units->data[0]);
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
        && register_builtin_traits(builtins, root_state->types)
        && th_env_freeze(builtins);
    if (!builtins_ready)
        n00bb_internal_error(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], (core_diag_location_t){},
            "collecting declarations",
            "could not record the built-in traits");
    const th_env *base_inputs[] = {builtins, export_environment};
    th_env *base = builtins_ready
        ? n00bb_compose_environments(base_inputs, 2, root_state->types,
                                     units->data[0])
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
        ? n00bb_compose_environments(environment_inputs, 2,
                                     root_state->types, units->data[0])
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
        bool attached = environment_attached
            ? th_ctx_set_environment_scope(state->types,
                unit->module.identifier, n00bb_package_id(unit))
            : th_ctx_set_environment(state->types, environment,
                unit->module.identifier, n00bb_package_id(unit));
        if (!attached) {
            n00bb_internal_error(core_translation_unit_diagnostics(unit),
                unit, (core_diag_location_t){}, "collecting declarations",
                "could not attach the trait environment");
            ok = false;
            break;
        }
        environment_attached = true;
    }
    if (ok && !register_dispatch_adapters(environment, link, grammar,
                                          analysis)) {
        n00bb_internal_error(core_translation_unit_diagnostics(
            units->data[0]), units->data[0], (core_diag_location_t){},
            "collecting declarations",
            "could not declare trait dispatch adapters");
        ok = false;
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
