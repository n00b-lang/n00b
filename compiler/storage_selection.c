#include "storage_selection.h"

#include <string.h>

n00bb_storage_decision_t
n00bb_storage_decide(const n00bb_storage_facts_t *facts,
                     bool *dynamic_duration)
{
    if (dynamic_duration) *dynamic_duration = false;
    if (!facts) return N00BB_STORAGE_INVALID_TARGET;
    if (facts->required) {
        if (!facts->is_typed_variable)
            return N00BB_STORAGE_INVALID_TARGET;
        if (!facts->requested.data || !facts->requested.u8_bytes)
            return N00BB_STORAGE_MISSING_CLASS;
        if (facts->selected.data && facts->selected.u8_bytes
            && (facts->requested.u8_bytes != facts->selected.u8_bytes
                || memcmp(facts->requested.data, facts->selected.data,
                          facts->requested.u8_bytes)))
            return N00BB_STORAGE_CONFLICT;
        if (dynamic_duration)
            *dynamic_duration = facts->has_lexical_lifetime;
        return N00BB_STORAGE_EXPLICIT;
    }
    return N00BB_STORAGE_KEEP;
}

n00bb_storage_decision_t
n00bb_storage_choose_placement(const n00bb_storage_placement_facts_t *facts,
                               bool *dynamic_duration)
{
    if (dynamic_duration) *dynamic_duration = false;
    if (!facts) return N00BB_STORAGE_INVALID_TARGET;
    if (!facts->has_lexical_lifetime || facts->is_reference)
        return N00BB_STORAGE_KEEP;
    if (!facts->size_known || facts->size_bytes > facts->stack_limit_bytes) {
        if (dynamic_duration) *dynamic_duration = true;
        return N00BB_STORAGE_RUNTIME_DYNAMIC;
    }
    return N00BB_STORAGE_KEEP;
}
