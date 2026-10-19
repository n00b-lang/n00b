#include <string.h>

#include "compiler_link.h"
#include "compiler_link_lifetime.h"

static parse_sym_entry_t *
alias_global_symbol(parse_grammar_t *grammar, parse_sym_entry_t *symbol,
                    core_list_t(parse_sym_entry_ptr_t) *active)
{
    if (!symbol) return nullptr;
    if (is_module_global_symbol(grammar, symbol)) return symbol;
    for (size_t i = 0; i < active->len; i++)
        if (active->data[i] == symbol) return nullptr;
    core_list_push(*active, symbol);
    parse_sym_entry_t *found = nullptr;
    for (size_t i = 0; i < parse_sym_alias_count(symbol); i++) {
        const parse_sym_alias_t *alias = parse_sym_alias_at(symbol, i);
        parse_sym_entry_t *candidate = alias_global_symbol(grammar,
            alias ? alias->other : nullptr, active);
        if (!candidate) continue;
        if (found && found != candidate) {
            found = nullptr;
            break;
        }
        found = candidate;
    }
    active->len--;
    return found;
}

static parse_sym_entry_t *
reference_expression_global(parse_grammar_t *grammar,
                            core_translation_unit_t *unit,
                            parse_tree_t *expression)
{
    n00bb_tu_state_t *state = tu_state(unit);
    n00bb_alias_edges_t *facts = state && expression
        ? n00bb_node_alias_edges(expression) : nullptr;
    if (!facts) return nullptr;
    core_list_t(parse_sym_entry_ptr_t) active =
        core_list_new(parse_sym_entry_ptr_t);
    parse_sym_entry_t *found = nullptr;
    for (size_t i = 0; i < facts->count; i++) {
        n00bb_alias_edge_t *edge = &facts->edges[i];
        if (edge->kind == N00BB_ALIAS_CALL) continue;
        parse_sym_entry_t *candidate = alias_global_symbol(grammar,
            edge->symbol, &active);
        if (!candidate) continue;
        if (found && found != candidate) {
            found = nullptr;
            break;
        }
        found = candidate;
    }
    core_list_free(active);
    return found;
}

static bool
publish_returned_parameter_aliases(core_link_unit_t *link,
                                   n00bb_link_analysis_t *analysis,
                                   uint32_t producer_stage)
{
    core_dict_t returned_parameters;
    core_dict_init(&returned_parameters, nullptr, nullptr);
    bool ok = true;
    size_t count = core_link_unit_dependency_count(link);
    for (size_t i = 0; i < count; i++) {
        core_link_dependency_view_t call;
        if (!core_link_unit_dependency_at(link, i, &call)) {
            ok = false;
            break;
        }
        if ((call.kind != N00BB_LINK_DEPENDENCY_CALL
                && call.kind != N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                && call.kind != N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            || !call.to || !call.source_unit || !call.source_site) continue;
        if (analysis->may_link_further
            && call.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH)
            continue;
        n00bb_link_payload_t *payload = core_link_declaration_payload(call.to);
        parse_sym_entry_t *callee = payload ? payload->symbol : nullptr;
        if (!callee) continue;
        size_t parameter_index = 0;
        bool cached = false;
        uintptr_t encoded = (uintptr_t)core_dict_get(
            &returned_parameters, callee, &cached);
        if (!cached) {
            bool returns_parameter = function_returned_parameter(
                analysis->grammar, analysis, callee, &parameter_index);
            encoded = returns_parameter ? parameter_index + 2 : 1;
            core_dict_put(&returned_parameters, callee, encoded);
        }
        else if (encoded > 1) parameter_index = encoded - 2;
        bool returns_parameter = encoded > 1;
        if (!returns_parameter) continue;
        parse_tree_t *argument = call_argument_for_parameter(
            analysis->grammar, (parse_tree_t *)call.source_site, callee,
            parameter_index,
            call.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH);
        if (!argument) continue;
        parse_sym_entry_t *global = reference_expression_global(
            analysis->grammar, call.source_unit, argument);
        if (!global) continue;
        core_link_declaration_t *target = link_declaration_for_symbol(
            analysis, global);
        if (!target) {
            ok = false;
            break;
        }

        core_link_declaration_t *source = call.from;
        if (!source) {
            n00bb_tu_state_t *state = tu_state(call.source_unit);
            parse_sym_entry_t *initializer_owner =
                module_variable_initializer_owner(analysis->grammar,
                    state && state->annotations
                        ? state->annotations->symtab : nullptr,
                    (parse_tree_t *)call.source_site);
            source = initializer_owner
                ? link_declaration_for_symbol(analysis, initializer_owner)
                : nullptr;
        }
        if (source) {
            if (!core_link_unit_add_dependency(link,
                    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_ALIAS_SUMMARY,
                    producer_stage, CORE_LINK_DEPENDENCY_RESOLVED,
                    source, target, call.source_unit, call.source_site,
                    call.source_location, call.provenance,
                    call.provenance_version, nullptr)) {
                ok = false;
                break;
            }
        }
        else if (!core_link_unit_add_dependency(link,
                N00BB_LINK_DEPENDENCY_INITIALIZER_USE, producer_stage,
                CORE_LINK_DEPENDENCY_RESOLVED, nullptr, target,
                call.source_unit, call.source_site, call.source_location,
                call.provenance, call.provenance_version, nullptr))
            { ok = false; break; }
    }
    core_dict_free(&returned_parameters);
    return ok;
}

parse_sym_entry_t *
link_dependency_callee_symbol(n00bb_link_analysis_t *analysis,
                              core_link_dependency_view_t *call)
{
    if (!analysis || !call || !call->source_site) return nullptr;
    n00bb_link_payload_t *payload = call->to
        ? core_link_declaration_payload(call->to) : nullptr;
    if (payload && payload->symbol) return payload->symbol;
    n00bb_tu_state_t *state = tu_state(call->source_unit);
    parse_cf_label_t *label = state && state->annotations
        ? parse_cf_label_lookup(state->annotations,
            (parse_tree_t *)call->source_site) : nullptr;
    parse_tree_t *node = label && label->kind == PARSE_CF_CALL && label->cond
        ? label->cond : (parse_tree_t *)call->source_site;
    return bound_function_symbol(node);
}

bool
link_dependency_reference_builtin(core_link_dependency_view_t *call,
                                  core_string_t *name)
{
    if (!call || !call->to) return false;
    core_string_t module = core_link_declaration_module(call->to);
    core_string_t key = core_link_declaration_key(call->to);
    if (!core_string_eq(module, CORE_STRING_STATIC("builtins"))
        || (!core_string_eq(key, CORE_STRING_STATIC("ref_read"))
            && !core_string_eq(key, CORE_STRING_STATIC("ref_write"))))
        return false;
    if (name) *name = key;
    return true;
}

static bool
publish_builtin_reference_global_effects(
    core_link_unit_t *link, n00bb_link_analysis_t *analysis,
    uint32_t producer_stage, bool *complete)
{
    if (!link || !analysis || !complete) return false;
    *complete = true;
    size_t call_count = core_link_unit_dependency_count(link);
    for (size_t i = 0; i < call_count; i++) {
        core_link_dependency_view_t call;
        if (!core_link_unit_dependency_at(link, i, &call)) return false;
        if ((call.kind != N00BB_LINK_DEPENDENCY_CALL
             && call.kind != N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
             && call.kind != N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            || !call.to || !call.source_unit || !call.source_site) continue;
        if (!link_dependency_reference_builtin(&call, nullptr))
            continue;
        parse_sym_entry_t *callee = link_dependency_callee_symbol(
            analysis, &call);
        if (!n00bb_builtin_reference_function(callee)) return false;

        parse_tree_t *argument = call_argument_for_parameter(
            analysis->grammar, (parse_tree_t *)call.source_site, callee, 0,
            false);
        if (!argument) continue;
        core_buffer_t global_name = {0};
        core_list_t(parse_sym_entry_ptr_t) visited =
            core_list_new(parse_sym_entry_ptr_t);
        bool found = simple_returned_global(analysis->grammar, analysis,
            call.source_unit, argument, &visited, &global_name);
        core_list_free(visited);
        if (!found) {
            core_buffer_release(&global_name);
            continue;
        }
        parse_sym_entry_t *global = global_symbol_from_qualified_name(
            analysis->grammar, analysis, core_buffer_string(&global_name));
        core_link_declaration_t *target = global
            ? link_declaration_for_symbol(analysis, global) : nullptr;
        core_buffer_release(&global_name);
        if (!target) {
            *complete = false;
            if (!core_link_unit_add_dependency(link,
                    N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN,
                    producer_stage, CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    call.from, nullptr, call.source_unit, call.source_site,
                    call.source_location, call.provenance,
                    call.provenance_version, nullptr)) return false;
            continue;
        }
        uint32_t effect = core_string_eq(callee->name,
                CORE_STRING_STATIC("ref_write"))
            ? N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
            : N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ;
        if (!core_link_unit_add_dependency(link, effect, producer_stage,
                CORE_LINK_DEPENDENCY_RESOLVED, call.from, target,
                call.source_unit, call.source_site, call.source_location,
                call.provenance, call.provenance_version, nullptr))
            return false;
    }
    return true;
}

bool
publish_function_effect_summaries(core_link_unit_t *link,
                                  n00bb_link_analysis_t *analysis,
                                  uint32_t producer_stage,
                                  core_link_contribution_state_t direct_state,
                                  core_link_contribution_state_t call_state,
                                  core_link_contribution_state_t *state_out)
{
    bool complete = direct_state == CORE_LINK_CONTRIBUTION_COMPLETE
        && call_state == CORE_LINK_CONTRIBUTION_COMPLETE;
    if (!restore_context_effect_dependencies(link, producer_stage))
        return false;
    if (!publish_returned_parameter_aliases(link, analysis, producer_stage))
        return false;
    bool builtin_effects_complete = false;
    if (!publish_builtin_reference_global_effects(link, analysis,
            producer_stage, &builtin_effects_complete)) return false;
    complete &= builtin_effects_complete;
    size_t call_count = core_link_unit_dependency_count(link);
    for (size_t c = 0; c < call_count; c++) {
        core_link_dependency_view_t call;
        if (!core_link_unit_dependency_at(link, c, &call)) return false;
        if ((call.kind != N00BB_LINK_DEPENDENCY_CALL
                && call.kind != N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                && call.kind != N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            || !call.source_unit || !call.source_site) continue;

        core_link_declaration_t *unknown_owner = nullptr;
        bool known_reference_builtin =
            link_dependency_reference_builtin(&call, nullptr);
        n00bb_link_payload_t *callee_payload = call.to
            ? core_link_declaration_payload(call.to) : nullptr;
        parse_sym_entry_t *callee_symbol = callee_payload
            ? callee_payload->symbol : nullptr;
        bool archived_context_effects = callee_symbol
            && parse_sym_binding(callee_symbol,
                CORE_STRING_STATIC("n00bb.context-effects"));
        if (analysis->may_link_further
            && call.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH)
            unknown_owner = call.from ? call.from : call.to;
        else if ((call.state == CORE_LINK_DEPENDENCY_EXTERNAL
             || (call.to
                 && link_declaration_has_only_interface_definition(call.to)))
            && !known_reference_builtin && !archived_context_effects)
            unknown_owner = call.to;
        else if (call.state != CORE_LINK_DEPENDENCY_RESOLVED
                 && !known_reference_builtin)
            unknown_owner = call.from;
        if (!unknown_owner) continue;

        if (!core_link_unit_add_dependency(link,
                N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN,
                producer_stage, CORE_LINK_DEPENDENCY_UNAVAILABLE,
                unknown_owner, nullptr, call.source_unit, call.source_site,
                call.source_location, call.provenance,
                call.provenance_version, nullptr)) return false;
    }

    size_t fact_count = core_link_unit_dependency_count(link);
    core_link_dependency_view_t *calls = core_alloc_array(
        core_link_dependency_view_t, fact_count);
    size_t resolved_call_count = 0;
    bool propagated = true;
    for (size_t c = 0; c < fact_count; c++) {
        core_link_dependency_view_t call;
        if (!core_link_unit_dependency_at(link, c, &call)) {
            propagated = false;
            break;
        }
        if ((call.kind == N00BB_LINK_DEPENDENCY_CALL
                || call.kind == N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                || call.kind == N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            && call.from && call.to
            && call.state == CORE_LINK_DEPENDENCY_RESOLVED)
            calls[resolved_call_count++] = call;
    }
    for (size_t e = 0; propagated
         && e < core_link_unit_dependency_count(link); e++) {
        core_link_dependency_view_t effect;
        if (!core_link_unit_dependency_at(link, e, &effect)) {
            propagated = false;
            break;
        }
        uint32_t summary_kind = effect_summary_kind(effect.kind);
        if (!summary_kind || !effect.from
            || (!effect.to && summary_kind
                != N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN))
            continue;
        bool resolved = effect.state == CORE_LINK_DEPENDENCY_RESOLVED
            && summary_kind
                != N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_UNKNOWN;
        for (size_t c = 0; c < resolved_call_count; c++) {
            const core_link_dependency_view_t *call = &calls[c];
            if (call->to != effect.from) continue;
            if (!core_link_unit_add_dependency(link, summary_kind,
                    producer_stage, resolved
                        ? CORE_LINK_DEPENDENCY_RESOLVED
                        : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    call->from, effect.to, call->source_unit,
                    call->source_site, call->source_location,
                    call->provenance, call->provenance_version, nullptr)) {
                propagated = false;
                break;
            }
        }
    }
    core_free(calls);
    if (!propagated) return false;
    *state_out = complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                          : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}

bool
n00bb_next_call_within(core_translation_unit_t *unit, parse_tree_t *node,
                       size_t *cursor, core_link_dependency_view_t *call,
                       th_type **signature)
{
    n00bb_tu_state_t *state = tu_state(unit);
    core_link_unit_t *link = state ? state->link : nullptr;
    size_t count = link ? core_link_unit_dependency_count(link) : 0;
    while (*cursor < count) {
        size_t i = (*cursor)++;
        if (!core_link_unit_dependency_at(link, i, call)
            || (call->kind != N00BB_LINK_DEPENDENCY_CALL
                && call->kind != N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                && call->kind != N00BB_LINK_DEPENDENCY_CONTEXT_CALL)
            || call->source_unit != unit || !call->to
            || !node_is_within((parse_tree_t *)call->source_site, node))
            continue;
        ir_func_t *callee = core_link_declaration_binding(call->to,
            CORE_STRING_STATIC("n00bb.ir.function"));
        *signature = callee ? th_resolve(callee->type) : nullptr;
        return true;
    }
    return false;
}

bool
n00bb_signature_writes_arguments(th_type *signature)
{
    for (size_t p = 0; signature && p < th_func_nparams(signature); p++) {
        const th_param *parameter = th_func_param(signature, p);
        if (parameter && th_mode_produces_output(parameter->mode))
            return true;
    }
    const th_param *rest = signature ? th_func_rest(signature) : nullptr;
    return rest && th_mode_produces_output(rest->mode);
}

bool
n00bb_callee_writes_globals(core_link_unit_t *link,
                            core_link_declaration_t *callee)
{
    for (size_t j = 0; j < core_link_unit_dependency_count(link); j++) {
        core_link_dependency_view_t effect;
        if (!core_link_unit_dependency_at(link, j, &effect)
            || effect.from != callee) continue;
        if (effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE
            || effect.kind == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE
            || effect.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_WRITE_SUMMARY
            || effect.kind
                == N00BB_LINK_DEPENDENCY_FUNCTION_GLOBAL_READ_WRITE_SUMMARY)
            return true;
    }
    return false;
}

bool
n00bb_use_writes_root(core_translation_unit_t *unit, parse_grammar_t *grammar,
                      parse_tree_t *use, parse_tree_t *region)
{
    for (parse_tree_t *at = parse_tree_parent(use);
         at && at != region; at = parse_tree_parent(at)) {
        if ((nt_is(grammar, at, CORE_STRING_STATIC("assign-stmt"))
             || nt_is(grammar, at, CORE_STRING_STATIC("binop-assign-stmt")))
            && node_is_within(use, core_tree_child(at, 0))) return true;
        if (nt_is(grammar, at, CORE_STRING_STATIC("unary-expr"))
            && core_string_eq(first_token_text(at), CORE_STRING_STATIC("ref")))
            return true;
        if (!nt_is(grammar, at, CORE_STRING_STATIC("postfix-expr"))
            || core_tree_num_children(at) < 3
            || !parse_tree_leaf_is(core_tree_child(at, 1),
                                   CORE_STRING_STATIC("("))) continue;
        size_t cursor = 0;
        core_link_dependency_view_t call;
        th_type *signature = nullptr;
        while (n00bb_next_call_within(unit, at, &cursor, &call, &signature))
            if (!signature || n00bb_signature_writes_arguments(signature))
                return true;
    }
    return false;
}
