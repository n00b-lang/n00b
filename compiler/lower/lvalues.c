#include "lower_internal.h"

// A call to a function names the function rather than reading it, so the
// part of the call that names it is not lowered as a value.
bool
n00bb_mark_call_lvalues(ir_lower_context_t *context, parse_visit_t *visit,
                        bool entering, void *state)
{
    (void)state;
    if (!entering || !visit->annot || !visit->node
        || nt_is(visit->grammar, visit->node, CORE_STRING_STATIC("context-call"))) return true;
    parse_node_facts_t *facts = parse_tree_call_facts(visit->node);
    parse_sym_entry_t *symbol = facts && facts->call_function
        ? canonical_symbol(facts->call_function) : nullptr;
    if (!symbol || symbol->kind != PARSE_SYM_FUNCTION || !facts->call_callee)
        return true;
    return ir_lower_skip_value(context, visit->node, facts->call_callee);
}
