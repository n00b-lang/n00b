/*
 * WP-009 Phase 2 regression test — tree-query (no-fragment) rules.
 *
 * Exercises the WP-009 Phase 2 query-mode codepath. A query-mode
 * rule is a guidance rule that:
 *   - declares a `@violation_nt` naming an NT in the BASE language
 *     grammar (not a user-defined fragment); and
 *   - ships NO `bnf_fragment` block — its BNF body is empty.
 *
 * Phase 2's required changes to make this work:
 *   1. `src/naudit/guidance.c` — the per-rule required-field schema
 *      check no longer demands `bnf_fragment->u8_bytes != 0`; an
 *      empty assembled body is normalised to `rule->bnf_fragment =
 *      nullptr`.
 *   2. `src/naudit/engine.c::get_or_load_grammar` — the
 *      fragment-merge loop skips rules with a null/empty fragment
 *      (this guard was written in Phase 1 in anticipation of
 *      Phase 2, so no Phase 2 change to engine.c was needed —
 *      verified during implementation).
 *
 * Fixture pair:
 *   - guidance_query_mode.bnf — one rule
 *     (`naudit.test.query_mode`) targeting
 *     `<function_definition>` in c_ncc.bnf, with no fragment.
 *   - fixture_query_mode.c — a C source file with EXACTLY TWO
 *     function definitions (`helper` + `main`).
 *
 * Expected outcome: the engine reports exactly 2 violations, both
 * carrying `rule->id == r"naudit.test.query_mode"`.
 *
 * DF-X resolution (Phase 2). The test picks `<function_definition>`
 * as the violation NT. Rationale:
 *   - Verified present in `grammars/c_ncc.bnf` (line 417 —
 *     `<function_definition> @scope("", declarator) ::= ...`).
 *   - Has unambiguous match semantics — exactly one parse-tree
 *     node per defined function in a translation unit.
 *   - Yields a deterministic match count for a small fixture
 *     (two function definitions → two violations).
 *   - The auditor flagged that `<function_call>` is NOT an NT in
 *     `c_ncc.bnf` (function calls are an alternative of
 *     `<postfix_expression>` at line 537), so it cannot be used
 *     as a `@violation_nt`. `<function_definition>` is a top-
 *     level NT and avoids that pitfall.
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test.
 *
 * Bootstrap shape mirrors test/unit/test_naudit_engine.c per the
 * relaxed test convention: libc <assert.h> + <stdio.h> are
 * allowed for harness scaffolding (NCC.md "NO LIBC ALLOWED"
 * exemption for test files), main(argc, argv) plus
 * n00b_init_simple(argc, argv) first thing, fixture-path
 * discovery via the N00B_AUDIT_TEST_FIXTURE_DIR macro (no
 * hardcoded paths in the C source — set by the top-level
 * meson.build).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build (see meson.build)"
#endif

/*
 * The Phase 2 fixture (`fixture_query_mode.c`) contains exactly
 * two `<function_definition>` matches: `helper` and `main`. The
 * number is observed from the checked-in fixture file, not
 * invented (per the cross-project numeric-claims rule —
 * ~/CLAUDE.md feedback_numeric_claims.md). Update this constant
 * if the fixture's layout changes.
 */
#define EXPECTED_QUERY_MODE_MATCHES 2

/*
 * Compare a libn00b string against a C string. Same shape as the
 * helper in test/unit/test_naudit_engine_malloc.c so the
 * assertion idiom is consistent across the engine tests.
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
 * Load the query-mode guidance fixture and assert its shape. The
 * key Phase 2 invariant: the lone rule loads cleanly even though
 * it has NO BNF body — the guidance loader's per-rule schema
 * check no longer demands a non-empty `bnf_fragment`.
 */
static n00b_audit_guidance_t *
load_query_mode_guidance(void)
{
    n00b_string_t *p = fixture_path("guidance_query_mode.bnf");
    auto r = n00b_audit_load_guidance(p);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  load_guidance failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(!!g);
    assert(!!g->rules);
    assert(n00b_list_len(*g->rules) == 1);

    n00b_audit_rule_t *rule = n00b_list_get(*g->rules, 0);
    assert(!!rule);
    assert(n00b_string_eq_cstr(rule->id, "naudit.test.query_mode"));
    /*
     * Phase 2 invariant: a query-mode rule's `bnf_fragment` is
     * `nullptr`. The loader normalises an empty assembled body
     * to nullptr in `guidance.c` so the engine's fragment-merge
     * skip-on-null branch fires (rather than the skip-on-empty
     * branch).
     */
    assert(rule->bnf_fragment == nullptr);
    assert(!!rule->violation_nt);
    assert(n00b_string_eq_cstr(rule->violation_nt,
                               "function_definition"));

    printf("  [PASS] query-mode guidance loads "
           "(1 rule, bnf_fragment == nullptr, "
           "violation_nt == function_definition)\n");
    return g;
}

static void
test_engine_new_ok(n00b_audit_engine_t **out_engine,
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
    printf("  [PASS] engine_new (query-mode rule, no fragment to merge)\n");
}

/*
 * Run the engine on the query-mode fixture and assert:
 *   - the result is OK (no parse / target / schema error);
 *   - the violation count equals EXPECTED_QUERY_MODE_MATCHES
 *     (one violation per `<function_definition>` in the source);
 *   - every violation's rule is the lone query-mode rule.
 */
static void
test_check_fixture_query_mode(n00b_audit_engine_t   *engine,
                              n00b_audit_guidance_t *guidance)
{
    n00b_string_t *path = fixture_path("fixture_query_mode.c");
    auto r = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_query_mode.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    int64_t n = n00b_list_len(*violations);
    assert(n == EXPECTED_QUERY_MODE_MATCHES);

    n00b_audit_rule_t *expected_rule = n00b_list_get(*guidance->rules, 0);
    assert(!!expected_rule);

    for (int64_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        assert(!!v);
        assert(!!v->rule);
        /*
         * Two complementary checks: rule->id matches the
         * expected text (catches a wrong rule pointer that
         * happens to share the same address-space slot), and
         * the pointer itself matches the loaded rule (catches
         * a duplicated id where the engine emitted against an
         * unintended rule struct).
         */
        assert(n00b_string_eq_cstr(v->rule->id, "naudit.test.query_mode"));
        assert(v->rule == expected_rule);
        assert(!!v->file);
        assert(v->line > 0);
        assert(v->column > 0);
    }

    printf("  [PASS] check_fixture_query_mode "
           "(n=%lld, all rule_id=naudit.test.query_mode)\n",
           (long long)n);
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    n00b_audit_guidance_t *guidance = load_query_mode_guidance();
    n00b_audit_engine_t   *engine   = nullptr;

    test_engine_new_ok(&engine, guidance);
    test_check_fixture_query_mode(engine, guidance);

    printf("All n00b-audit WP-009 Phase 2 regression checks passed.\n");
    return 0;
}
