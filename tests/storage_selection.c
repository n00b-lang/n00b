#include <stdio.h>
#include "storage_selection.h"

static int failures;

#define CHECK(lit, passed) check(CORE_STRING_STATIC(lit), (passed))

static void
check(core_string_t name, bool passed)
{
    if (passed) return;
    fprintf(stderr, "FAIL %.*s\n", (int)name.u8_bytes, name.data);
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
    CHECK("explicit request selects exact class without escape",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_EXPLICIT && dynamic_duration);
    facts.has_lexical_lifetime = false;
    CHECK("explicit request applies to an owning global",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_EXPLICIT && !dynamic_duration);
    facts.selected = CORE_STRING_STATIC("gc");
    CHECK("conflicting selected class is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_CONFLICT);
    facts.selected = (core_string_t){0};
    facts.requested = (core_string_t){0};
    CHECK("required request without a class is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_MISSING_CLASS);
    facts.is_typed_variable = false;
    CHECK("request on a non-variable symbol is rejected",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_INVALID_TARGET);

    facts.is_typed_variable = true;
    facts.required = false;
    facts.has_lexical_lifetime = true;
    facts.escapes = true;
    CHECK("escaping local requests runtime-managed dynamic storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    facts.is_reference = true;
    CHECK("reference binding does not select separate gc storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    facts.is_reference = false;
    facts.has_lexical_lifetime = false;
    CHECK("escaping owning global does not receive local gc storage",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    facts.has_lexical_lifetime = true;
    facts.escapes = false;
    facts.alias_analysis_complete = false;
    facts.has_aliases = true;
    CHECK("incomplete alias evidence defers placement to runtime",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    facts.has_aliases = false;
    CHECK("incomplete aliases do not force closed scalar values dynamic",
          n00bb_storage_decide(&facts, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);

    n00bb_storage_placement_facts_t placement = {
        .has_lexical_lifetime = true,
        .alias_analysis_complete = true,
        .size_known = true,
        .size_bytes = 128,
        .stack_limit_bytes = 256,
    };
    CHECK("bounded known local remains automatic",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    placement.size_bytes = placement.stack_limit_bytes;
    CHECK("local at the configured limit remains automatic",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_KEEP && !dynamic_duration);
    placement.size_bytes = 257;
    CHECK("oversized local uses runtime-managed dynamic storage",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    placement.size_known = false;
    CHECK("unknown target layout uses runtime-managed dynamic storage",
          n00bb_storage_choose_placement(&placement, &dynamic_duration)
              == N00BB_STORAGE_RUNTIME_DYNAMIC && dynamic_duration);
    return failures ? 1 : 0;
}
