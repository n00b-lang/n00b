#include <string.h>

#include "call_plan.h"
#include "compiler_link.h"
#include "compiler_tu.h"
#include "diagnostics.h"
#include "parse/tree_util.h"

typedef struct {
    parse_grammar_t *grammar;
} module_inference_context_t;

bool n00bb_is_trait_dispatch_reference(parse_grammar_t *grammar,
                                       core_translation_unit_t *unit,
                                       parse_tree_t *node);

typedef struct {
    core_translation_unit_t *unit;
    parse_cf_label_t *label;
} n00bb_call_site_t;
core_list_decl(n00bb_call_site_t);

typedef struct {
    th_type *source;
    th_type *instance;
    bool channels;
    core_string_t requirement_trait;
} n00bb_call_signature_t;

typedef struct {
    core_dict_t by_caller;
    core_dict_t instantiated_signatures;
} n00bb_call_site_index_t;

bool
call_units_failed(core_translation_unit_t *const *units, size_t unit_count)
{
    for (size_t i = 0; i < unit_count; i++)
        if (units[i] && core_diagnostics_failed(
                core_translation_unit_diagnostics(units[i])))
            return true;
    return false;
}

parse_tree_t *
call_argument_for_parameter(parse_grammar_t *grammar, parse_tree_t *call,
                           parse_sym_entry_t *function, size_t target,
                           bool trait_dispatch)
{
    if (trait_dispatch && target == 0) {
        parse_tree_t *member = member_access_node(grammar, call);
        if (!member && core_tree_num_children(call))
            member = member_access_node(grammar,
                                         core_tree_child(call, 0));
        return member && core_tree_num_children(member)
            ? core_tree_child(member, 0) : nullptr;
    }
    n00bb_call_plan_t plan;
    (void)n00bb_plan_call(&plan, grammar, function,
        n00bb_symbol_signature(function),
        call_arguments_node(grammar, call), trait_dispatch ? 1 : 0);
    parse_tree_t *found = n00bb_call_plan_parameter_argument(&plan, target);
    n00bb_call_plan_free(&plan);
    return found;
}

bool
publish_link_call_dependencies(core_link_unit_t *link,
                               n00bb_link_analysis_t *analysis,
                               uint32_t producer_stage,
                               core_link_contribution_state_t *state_out)
{
    bool complete = true;
    for (size_t i = 0; i < analysis->units.len; i++) {
        core_translation_unit_t *unit = analysis->units.data[i];
        n00bb_tu_state_t *state = tu_state(unit);
        if (!state || !state->annotations) continue;
        for (size_t label_index = 0;
             label_index < state->annotations->cf_label_order.len;
             label_index++) {
            parse_cf_label_t *label =
                state->annotations->cf_label_order.data[label_index];
            if (!label || label->kind != PARSE_CF_CALL || !label->cond)
                continue;
            parse_tree_t *member = member_access_node(analysis->grammar,
                                                       label->cond);
            bool trait_dispatch = member
                && !call_receiver_is_module(analysis->grammar, unit,
                    core_tree_child(member, 0));
            parse_sym_entry_t *callee = bound_function_symbol(label->cond);
            if (trait_dispatch && n00bb_builtin_option_function(callee))
                continue;
            parse_sym_entry_t *context = attached_symbol(
                analysis->grammar, unit, label->cond);
            bool context_call = !callee
                && context_symbol(analysis->grammar, context);
            if (context_call) callee = context;
            core_link_declaration_t *target = callee
                ? link_declaration_for_symbol(analysis, callee) : nullptr;
            bool external = target
                && core_link_declaration_is_external_requirement(target);
            parse_sym_entry_t *caller = link_effect_owner_symbol(
                analysis->grammar, label->self);
            core_link_declaration_t *source = caller
                ? link_declaration_for_symbol(analysis, caller) : nullptr;
            bool valid_caller = !caller
                || caller->kind == PARSE_SYM_FUNCTION
                || context_symbol(analysis->grammar, caller);
            bool resolved = target && callee
                && (context_call
                    ? context_symbol(analysis->grammar, callee)
                    : callee->kind == PARSE_SYM_FUNCTION)
                && valid_caller && (!caller || source);
            if (caller && !source)
                source = nullptr;
            if (!core_link_unit_add_dependency(link,
                    context_call ? N00BB_LINK_DEPENDENCY_CONTEXT_CALL
                    : trait_dispatch
                        ? N00BB_LINK_DEPENDENCY_TRAIT_DISPATCH
                        : N00BB_LINK_DEPENDENCY_CALL,
                    producer_stage,
                    external && resolved ? CORE_LINK_DEPENDENCY_EXTERNAL
                    : resolved ? CORE_LINK_DEPENDENCY_RESOLVED
                    : trait_dispatch ? CORE_LINK_DEPENDENCY_DEFERRED
                                      : CORE_LINK_DEPENDENCY_UNAVAILABLE,
                    source, resolved ? target : nullptr, unit, label->self,
                    link_source_location(label->self),
                    core_source_file_path(&unit->source), 1, nullptr))
                return false;
            if (!resolved) complete = false;
        }
    }
    *state_out = complete ? CORE_LINK_CONTRIBUTION_COMPLETE
                          : CORE_LINK_CONTRIBUTION_UNAVAILABLE;
    return true;
}

