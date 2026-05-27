/*
 * WP-004 Phase 1 regression test — § 13 legacy-spellings rule.
 *
 * Exercises the engine's grammar-compose + parse-forest-walk pipeline
 * against the canonical multi-rule guidance file in the n00b repo,
 * which now carries 7 rules total (WP-001's `n00b.s2_1.null`,
 * WP-002's `n00b.s2_3.malloc`, WP-003's four §§ 2.5/2.6/2.8/2.10
 * rules, and WP-004's `n00b.s13.legacy_spellings`).
 *
 * Setup. The Phase 2 loader is invoked against the canonical file
 * at /Users/viega/n00b/audit-rules.bnf (in n00b's tree,
 * not n00b-audit's). The test gates on `access()` and emits `[SKIP]`
 * if the canonical file is absent (process § 6.5b: env gates surface
 * as [SKIP] for cross-machine fixtures, never silent pass). The
 * shape mirrors test_audit_engine_malloc.c / test_audit_engine_bulk.c.
 *
 * ## Notes — DF-H resolution (WP-004 Phase 1 settlement)
 *
 * The preflight's candidate for the legacy-spellings rule was the
 * pure-BNF approach used by DF-C / DF-F / DF-G, attaching at
 * `<provided_identifier>` (c_ncc.bnf line 3). Reading c_ncc.bnf in
 * full revealed that TWO of the four target identifier-texts are
 * already-registered keywords:
 *
 *   - `%"_Static_assert"` is a keyword at c_ncc.bnf line 149 and
 *     feeds `<_kw_kw_static_assert>` at lines 614-615 (alongside
 *     `%"static_assert"`). It will NOT tokenize as `%IDENTIFIER`,
 *     so the default `<provided_identifier>` attachment does not
 *     reach it.
 *   - `%"__thread"` is a keyword at c_ncc.bnf line 120 and feeds
 *     `<_kw_kw_storage_class>` at lines 619-627 (alongside
 *     `thread_local`, `_Thread_local`, etc.). Same situation.
 *
 * The other two identifier-texts — `__alignof__` and `__alignas__`
 * — are NOT registered keywords (only their non-trailing-underscore
 * relatives `alignas`, `_Alignas`, `alignof`, `_Alignof` appear in
 * c_ncc.bnf). They tokenize via the default `%IDENTIFIER` path and
 * flow through `<provided_identifier>` (c_ncc.bnf line 3) →
 * `<identifier>` (line 448) → `<primary_expression>` (line 571).
 * Same DF-C / DF-F pattern.
 *
 * **DF-H resolution.** Mixed attachment — one violation-NT with
 * four keyword alternatives, attached at THREE distinct non-
 * terminals (one for each collision plus the default):
 *
 *     <n00b_audit_v_legacy_spellings> ::= %"_Static_assert"
 *     <n00b_audit_v_legacy_spellings> ::= %"__thread"
 *     <n00b_audit_v_legacy_spellings> ::= %"__alignof__"
 *     <n00b_audit_v_legacy_spellings> ::= %"__alignas__"
 *     <_kw_kw_static_assert> ::= <n00b_audit_v_legacy_spellings>
 *     <_kw_kw_storage_class> ::= <n00b_audit_v_legacy_spellings>
 *     <provided_identifier> ::= <n00b_audit_v_legacy_spellings>
 *
 * Effect: when the c_tokenizer sees `_Static_assert` or `__thread`,
 * the existing keyword terminals fire; the parser then derives
 * `<_kw_kw_static_assert>` or `<_kw_kw_storage_class>` through the
 * new alternative that wraps `<n00b_audit_v_legacy_spellings>`. For
 * `__alignof__` / `__alignas__`, the tokenizer emits `%IDENTIFIER`
 * (no keyword registered), and the new alternative of
 * `<provided_identifier>` reaches the wrapper through the standard
 * identifier-position path. In all four cases the parse tree
 * contains a `<n00b_audit_v_legacy_spellings>` node at the
 * violation site; `n00b_pt_search_by_nt` finds them all.
 *
 * The standard C23 spellings (`static_assert`, `thread_local`,
 * `alignof`, `alignas`) are deliberately NOT in the alternatives
 * list, so they remain un-flagged. The negative fixture exercises
 * this — using all four C23 spellings yields zero violations.
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test — no assertion mentions severity.
 *
 * The two new fixture .c files MUST contain the forbidden
 * identifiers for the positive case (and MUST NOT for the negative
 * case) — the engine PARSES them as target input and is supposed
 * to flag the listed identifiers. They are not n00b-audit source
 * code; the n00b-api-guidelines rules do NOT apply to them
 * (fixture-file carveout).
 *
 * Bootstrap shape mirrors test_audit_engine_malloc.c.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"

#include "naudit/naudit.h"
#include "naudit/engine.h"
#include "naudit/errors.h"
#include "naudit/guidance.h"
#include "naudit/rule.h"
#include "naudit/violation.h"

#ifndef N00B_AUDIT_TEST_FIXTURE_DIR
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build (see test/meson.build)"
#endif

#define N00B_AUDIT_REFERENCE_GUIDANCE_PATH \
    "/Users/viega/n00b/audit-rules.bnf"

/*
 * Pin the line numbers from the fixture files. fixture_legacy_spellings.c
 * places `_Static_assert` on line 2 and `__thread` on line 3.
 * fixture_null.c places `NULL` on line 5 (`int *p = NULL;`). The
 * numbers are observed from the checked-in fixture files, not
 * invented (per the cross-project numeric-claims rule — see
 * ~/CLAUDE.md feedback_numeric_claims.md). Update these constants
 * if the fixtures' layout changes.
 */
#define FIXTURE_LEGACY_FIRST_LINE 2
#define FIXTURE_NULL_FIRST_LINE   5

/*
 * Compare a libn00b string against a C string. Mirrors the helper in
 * test_audit_engine_malloc.c / test_audit_engine_bulk.c so the
 * assertion shape is identical across the canonical-file tests.
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

static n00b_string_t *
fixture_path(const char *fname)
{
    char buf[1024];
    int  n = snprintf(buf, sizeof(buf), "%s/%s",
                      N00B_AUDIT_TEST_FIXTURE_DIR, fname);
    assert(n > 0 && (size_t)n < sizeof(buf));
    return n00b_string_from_cstr(buf);
}

/*
 * Load the canonical multi-rule guidance file. Asserts the
 * presence of the WP-004 rule id rather than a hard count, so this
 * test remains valid as the canonical file accretes additional
 * rules in successor WPs.
 */
static n00b_audit_guidance_t *
load_canonical_guidance(void)
{
    n00b_string_t *p = n00b_string_from_cstr(
        N00B_AUDIT_REFERENCE_GUIDANCE_PATH);
    auto r = n00b_audit_load_guidance(p);
    assert(n00b_result_is_ok(r));

    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(!!g);
    assert(g->schema_version == 1);
    assert(!!g->rules);
    assert(n00b_list_len(*g->rules) >= 7);

    bool    saw_legacy = false;
    int64_t nrules     = n00b_list_len(*g->rules);
    for (int64_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*g->rules, i);
        assert(!!rule);
        if (n00b_string_eq_cstr(rule->id, "n00b.s13.legacy_spellings")) {
            saw_legacy = true;
        }
    }
    assert(saw_legacy);

    printf("  [PASS] canonical guidance loads (>=7 rules incl. legacy_spellings)\n");
    return g;
}

static void
engine_new_ok(n00b_audit_engine_t **out_engine,
              n00b_audit_guidance_t *guidance)
{
    auto r = n00b_audit_engine_new(guidance);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  engine_new failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));
    *out_engine = n00b_result_get(r);
    assert(!!*out_engine);
    printf("  [PASS] engine_new (7-rule grammar compose)\n");
}

static void
test_check_fixture_legacy(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_legacy_spellings.c");
    auto           r    = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_legacy_spellings.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    int64_t n = n00b_list_len(*violations);
    assert(n >= 1);

    /*
     * Walk every violation; at least one must be the legacy_spellings
     * rule, and the first one's line must pin to the fixture's
     * `_Static_assert(...)` site on line 2.
     */
    bool saw_legacy_rule = false;
    n00b_audit_violation_t *first_legacy = nullptr;
    for (int64_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        assert(!!v);
        assert(!!v->rule);
        if (n00b_string_eq_cstr(v->rule->id, "n00b.s13.legacy_spellings")) {
            saw_legacy_rule = true;
            if (!first_legacy) {
                first_legacy = v;
            }
        }
    }
    assert(saw_legacy_rule);
    assert(!!first_legacy);
    assert(!!first_legacy->file);
    assert(first_legacy->line == FIXTURE_LEGACY_FIRST_LINE);
    assert(first_legacy->column > 0);

    printf("  [PASS] check_fixture_legacy_spellings "
           "(n=%lld, first_legacy_line=%lld, col=%lld)\n",
           (long long)n,
           (long long)first_legacy->line,
           (long long)first_legacy->column);
}

static void
test_check_fixture_no_legacy(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_no_legacy_spellings.c");
    auto           r    = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_no_legacy_spellings.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    assert(n00b_list_len(*violations) == 0);

    printf("  [PASS] check_fixture_no_legacy_spellings (n=0)\n");
}

/*
 * Architecture-additive sanity check: under the 7-rule canonical
 * guidance, the WP-001 NULL rule MUST still fire on fixture_null.c.
 * This is the WP-004 stress point — the engine iterates
 * `guidance->rules`; the new rule must compose into the grammar
 * without disturbing prior rules' detections.
 */
static void
test_check_fixture_null_still_fires(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_null.c");
    auto           r    = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_null.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    int64_t n = n00b_list_len(*violations);
    assert(n >= 1);

    bool saw_null_rule = false;
    n00b_audit_violation_t *first_null = nullptr;
    for (int64_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        assert(!!v);
        assert(!!v->rule);
        if (n00b_string_eq_cstr(v->rule->id, "n00b.s2_1.null")) {
            saw_null_rule = true;
            if (!first_null) {
                first_null = v;
            }
        }
    }
    assert(saw_null_rule);
    assert(!!first_null);
    assert(!!first_null->file);
    assert(first_null->line == FIXTURE_NULL_FIRST_LINE);
    assert(first_null->column > 0);

    printf("  [PASS] check_fixture_null_still_fires "
           "(n=%lld, first_null_line=%lld, col=%lld)\n",
           (long long)n,
           (long long)first_null->line,
           (long long)first_null->column);
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

    n00b_audit_guidance_t *guidance = load_canonical_guidance();
    n00b_audit_engine_t   *engine   = nullptr;

    engine_new_ok(&engine, guidance);
    test_check_fixture_legacy(engine);
    test_check_fixture_no_legacy(engine);
    test_check_fixture_null_still_fires(engine);

    printf("All n00b-audit WP-004 Phase 1 regression checks passed.\n");
    return 0;
}
