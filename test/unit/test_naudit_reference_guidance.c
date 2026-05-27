/*
 * WP-001 Phase 6 Step 6.2: canonical-file regression test (AT-5).
 *
 * Loads /Users/viega/n00b/audit-rules.bnf via the
 * production Phase 2 loader (`n00b_audit_load_guidance`) and
 * asserts the schema shape — proving the production loader
 * accepts the canonical reference rule set end-to-end. The
 * canonical file lives in a separate repo (the n00b tree, not
 * n00b-audit's). If the file isn't present at test time the
 * test emits `[SKIP]` to stdout and returns success rather than
 * silent pass (process § 6.5b: environment gates surface as
 * `[SKIP]` for cross-machine fixtures, never silent pass).
 *
 * Per project DECISIONS.md D-005 there is **no `severity` field**
 * in the rule schema. The struct shape itself enforces this — no
 * assertion in this file references a `severity` member because
 * the member does not exist in `n00b_audit_rule_t`; any future PR
 * adding such a field would break the build, not this test.
 *
 * Per project DECISIONS.md D-006, public headers under
 * `include/audit/` are unprefixed (e.g. `audit/guidance.h`).
 *
 * Per project DECISIONS.md D-008, `!ptr` is the blessed
 * null-check idiom for n00b-audit C source.
 *
 * Relaxed test convention applies: libc `<assert.h>` / `<stdio.h>`
 * / `<unistd.h>` are acceptable for harness scaffolding (NCC.md
 * "NO LIBC ALLOWED" exemption for test files; mirror of the
 * Phase 2 test).
 *
 * Path-hardcoding carveout: the absolute path to the canonical
 * guidance file is acceptable here because (a) the test gates on
 * `access()` and `[SKIP]`s cleanly when the file is absent, and
 * (b) Phase 6 Step 6.3 verifies the test runs successfully
 * against the Step 6.1 file on this workstation. Future-WP
 * cleanup may convert the path to a meson-supplied `-D` macro
 * for cross-machine reproducibility.
 */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"

#include "naudit/naudit.h"
#include "naudit/guidance.h"
#include "naudit/rule.h"

#define N00B_AUDIT_REFERENCE_GUIDANCE_PATH \
    "/Users/viega/n00b/audit-rules.bnf"

/*
 * Compare a libn00b string against a C string. Mirrors the helper
 * in test/unit/test_audit_guidance.c so the assertion shape is
 * identical across the Phase 2 and Phase 6 tests.
 */
static bool
n00b_string_eq_cstr(n00b_string_t *s, const char *expected)
{
    if (!s) {
        return false;
    }
    size_t elen = 0;
    while (expected[elen] != '\0') {
        elen++;
    }
    if (s->u8_bytes != elen) {
        return false;
    }
    for (size_t i = 0; i < elen; i++) {
        if (s->data[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    if (access(N00B_AUDIT_REFERENCE_GUIDANCE_PATH, R_OK) != 0) {
        printf("[SKIP] canonical guidance file not present at %s\n",
               N00B_AUDIT_REFERENCE_GUIDANCE_PATH);
        return 0;
    }

    n00b_string_t *path = n00b_string_from_cstr(
        N00B_AUDIT_REFERENCE_GUIDANCE_PATH);
    assert(!!path);

    auto r = n00b_audit_load_guidance(path);
    assert(n00b_result_is_ok(r));
    printf("  [PASS] canonical guidance loads ok\n");

    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(!!g);
    printf("  [PASS] guidance struct non-null\n");

    assert(g->schema_version == 1);
    printf("  [PASS] schema_version == 1\n");

    assert(!!g->rules);
    assert(n00b_list_len(*g->rules) >= 1);
    printf("  [PASS] rules.len >= 1\n");

    n00b_audit_rule_t *rule = n00b_list_get(*g->rules, 0);
    assert(!!rule);
    printf("  [PASS] rules[0] non-null\n");

    assert(n00b_string_eq_cstr(rule->id, "n00b.s2_1.null"));
    printf("  [PASS] rules[0].id == \"n00b.s2_1.null\"\n");

    assert(!!rule->bnf_fragment);
    assert(rule->bnf_fragment->u8_bytes > 0);
    printf("  [PASS] rules[0].bnf_fragment non-empty\n");

    assert(!!rule->violation_nt);
    assert(rule->violation_nt->u8_bytes > 0);
    printf("  [PASS] rules[0].violation_nt non-empty\n");

    assert(!!rule->bad_example);
    assert(rule->bad_example->u8_bytes > 0);
    printf("  [PASS] rules[0].bad_example non-empty\n");

    assert(!!rule->good_example);
    assert(rule->good_example->u8_bytes > 0);
    printf("  [PASS] rules[0].good_example non-empty\n");

    printf("All canonical-guidance assertions passed.\n");
    return 0;
}
