#include "lower_internal.h"

static bool
n00bb_mark_addressable_argument(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_tree_t *owner,
                                parse_tree_t *argument, th_mode mode);

static bool
n00bb_mark_addressable_argument(ir_lower_context_t *context,
                                parse_grammar_t *grammar,
                                parse_tree_t *owner,
                                parse_tree_t *argument, th_mode mode)
{
    parse_tree_t *index = n00bb_index_expression_node(grammar, argument);
    if (index)
        return mode != TH_OUT || ir_lower_suppress_emit(context, index);

    parse_tree_t *at = argument;
    parse_tree_t *member = member_access_node(grammar, at);
    if (member) {
        while (member) {
            if (!ir_lower_suppress_emit(context, member)) return false;
            at = core_tree_num_children(member)
                ? core_tree_child(member, 0) : nullptr;
            member = member_access_node(grammar, at);
        }
        parse_tree_t *root = n00bb_simple_lvalue(at);
        if (!root) {
            th_type *base = at && n00bb_node_type(context, at)
                ? th_resolve(n00bb_node_type(context, at)) : nullptr;
            return at && (!base || th_kind_of(base) == TH_VAR
                          || th_kind_of(base) == TH_INDIRECTION);
        }
        return ir_lower_skip_value(context, owner, root);
    }

    parse_tree_t *lvalue = n00bb_simple_lvalue(argument);
    return lvalue && ir_lower_skip_value(context, owner, argument)
        && ir_lower_skip_value(context, owner, lvalue);
}

bool
n00bb_mark_reference_target(ir_lower_context_t *context,
                            parse_grammar_t *grammar,
                            parse_tree_t *owner, parse_tree_t *target)
{
    parse_tree_t *index = n00bb_index_expression_node(grammar, target);
    if (index) {
        if (!ir_lower_suppress_emit(context, index)) return false;
        parse_tree_t *receiver = core_tree_num_children(index)
            ? core_tree_child(index, 0) : nullptr;
        parse_tree_t *root = n00bb_simple_lvalue(receiver);
        return !root || ir_lower_skip_value(context, owner, root);
    }
    return n00bb_mark_addressable_argument(context, grammar, owner, target,
                                            TH_INOUT);
}

bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (!entering || !visit->annot || !visit->node
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("context-call"))) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *call_site = tu
        ? link_use_site_for_node(tu, visit->node) : nullptr;
    parse_tree_t *callee = core_tree_num_children(visit->node)
        ? core_tree_child(visit->node, 0) : nullptr;
    parse_sym_entry_t *symbol = call_site && call_site->init_call
        ? call_site->symbol : parse_tree_sym(visit->node);
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION)
        symbol = callee && lower
            ? bound_function_symbol(callee)
            : nullptr;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION) return true;
    parse_tree_t *member = member_access_node(visit->grammar, callee);
    parse_tree_t *receiver = member && core_tree_num_children(member)
        ? core_tree_child(member, 0) : nullptr;
    bool member_call = member
        && !call_receiver_is_module(visit->grammar, lower->unit, receiver);
    bool callback_call = member_call
        && n00bb_builtin_callback_function(symbol);
    bool trait_call = member_call && !callback_call;
    parse_tree_t *unused_callee = member_call
        ? core_tree_num_children(member) > 2
            ? core_tree_child(member, 2) : nullptr
        : callee;
    if (unused_callee
        && !ir_lower_skip_value(context, visit->node, unused_callee))
        return false;
    if (n00bb_overloaded(visit->grammar, tu, callee, symbol)) return true;

    n00bb_call_plan_t plan;
    bool planned = n00bb_plan_call(&plan, visit->grammar, symbol,
        symbol->callable_contract ? symbol->callable_contract : symbol->type,
        call_arguments_node(visit->grammar, visit->node),
        trait_call && receiver ? 1 : 0);
    bool ok = true;
    for (size_t i = plan.offset; planned && ok && i < plan.parameter_count;
         i++) {
        parse_tree_t *argument = plan.slots[i].argument;
        th_mode mode = TH_IN;
        if (!argument || !function_parameter_info(visit->grammar, symbol,
                plan.formals, i, nullptr, nullptr, nullptr, &mode)
            || !th_mode_produces_output(mode)) continue;
        ok = n00bb_mark_addressable_argument(context, visit->grammar,
            visit->node, argument, mode);
        if (!ok)
            report_unwritable_output(context, visit->grammar, argument, mode,
                symbol, n00bb_call_plan_parameter_name(&plan, i));
    }
    n00bb_call_plan_free(&plan);
    if (!ok && !ir_diags_failed(context->diagnostics))
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not mark a call argument as an lvalue");
    return ok;
}

bool
n00bb_mark_constructor_lvalues(ir_lower_context_t *context,
                               parse_visit_t *visit, bool entering,
                               void *state)
{
    (void)state;
    if (!entering || !visit->node) return true;
    n00bb_lower_context_t *lower = context->user;
    n00bb_tu_state_t *tu = lower ? tu_state(lower->unit) : nullptr;
    n00bb_link_use_site_t *site = tu
        ? link_use_site_for_node(tu, visit->node) : nullptr;
    parse_sym_entry_t *symbol = site ? site->symbol : nullptr;
    th_type *signature = n00bb_symbol_signature(symbol);
    signature = signature ? th_resolve(signature) : nullptr;
    if (!symbol || !signature || th_kind_of(signature) != TH_FUNCTION)
        return true;

    n00bb_call_plan_t plan;
    bool planned = n00bb_plan_call(&plan, visit->grammar, symbol, signature,
        child_nt(visit->grammar, visit->node, CORE_STRING_STATIC("call-args")),
        site->init_call ? 0 : 1);
    bool ok = true;
    for (size_t i = plan.offset; planned && ok && i < plan.parameter_count;
         i++) {
        parse_tree_t *argument = plan.slots[i].argument;
        th_mode mode = TH_INOUT;
        bool target = site->init_call && i == 0;
        if (!argument || (!target && (!function_parameter_info(
                visit->grammar, symbol, plan.formals, i, nullptr, nullptr,
                nullptr, &mode) || !th_mode_produces_output(mode))))
            continue;
        ok = n00bb_mark_addressable_argument(context, visit->grammar,
            visit->node, argument, mode);
        if (!ok)
            report_unwritable_output(context, visit->grammar, argument, mode,
                symbol, target ? (core_string_t){0}
                    : n00bb_call_plan_parameter_name(&plan, i));
    }
    n00bb_call_plan_free(&plan);
    if (!ok && !ir_diags_failed(context->diagnostics))
        lower_internal(context,
            lower_span_for_tree(context, visit->node),
            "could not mark a constructor argument as an lvalue");
    return ok;
}
