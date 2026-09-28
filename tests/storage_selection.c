#include <stdio.h>
#include "storage_selection.h"

static int failures;

static void
check(const char *name, bool passed)
{
    if (passed) return;
    fprintf(stderr, "FAIL %s\n", name);
    failures++;
}

int
main(void)
{
    n00bb_storage_facts_t facts = {
        .is_typed_variable = true,
        .has_lexical_lifetime = true,
        .alias_analysis_complete = true,
        .requested = CORE_STRING_STATIC("pool/opaque"),
        .required = true,
    };
    bool dynamic_duration = false;
    check("explicit request selects exact class without escape",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_EXPLICIT && dynamic_duration);
    facts.has_lexical_lifetime = false;
    check("explicit request applies to an owning global",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_EXPLICIT && !dynamic_duration);
    facts.selected = CORE_STRING_STATIC("gc");
    check("conflicting selected class is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_CONFLICT);
    facts.selected = (core_string_t){0};
    facts.requested = (core_string_t){0};
    check("required request without a class is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_MISSING_CLASS);
    facts.is_typed_variable = false;
    check("request on a non-variable symbol is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_INVALID_TARGET);

    facts.is_typed_variable = true;
    facts.required = false;
    facts.has_lexical_lifetime = true;
    facts.escapes = true;
    check("escaping local requests runtime-managed dynamic storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    facts.is_reference = true;
    check("reference binding does not select separate gc storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    facts.is_reference = false;
    facts.has_lexical_lifetime = false;
    check("escaping owning global does not receive local gc storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    facts.has_lexical_lifetime = true;
    facts.escapes = false;
    facts.alias_analysis_complete = false;
    facts.has_aliases = true;
    check("incomplete alias evidence defers placement to runtime",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    facts.has_aliases = false;
    check("incomplete aliases do not force closed scalar values dynamic",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);

    n00bb_storage_placement_facts_t placement = {
        .has_lexical_lifetime = true,
        .alias_analysis_complete = true,
        .size_known = true,
        .size_bytes = 128,
        .stack_limit_bytes = 256,
    };
    check("bounded known local remains automatic",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    placement.size_bytes = placement.stack_limit_bytes;
    check("local at the configured limit remains automatic",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    placement.size_bytes = 257;
    check("oversized local uses runtime-managed dynamic storage",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    placement.size_known = false;
    check("unknown target layout uses runtime-managed dynamic storage",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    return failures ? 1 : 0;
}
