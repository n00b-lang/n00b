#include "lower_internal.h"

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
    parse_tree_t *unused_callee = member_call
        ? core_tree_num_children(member) > 2
            ? core_tree_child(member, 2) : nullptr
        : callee;
    if (unused_callee
        && !ir_lower_skip_value(context, visit->node, unused_callee))
        return false;
    return true;
}
