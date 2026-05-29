/*
 * WP-003 Phase 1 regression test — bulk §§ 2.5/2.6/2.8/2.10 rules.
 *
 * Exercises the engine's grammar-compose + parse-forest-walk pipeline
 * against the canonical multi-rule guidance file in the n00b repo,
 * which carries 10 rules total: WP-001's
 * `n00b.s2_1.null`, WP-002's `n00b.s2_3.malloc`, WP-003's four
 * (`n00b.s2_5.attribute_legacy`, `n00b.s2_6.typeof_legacy`,
 * `n00b.s2_8.va_list_legacy`, `n00b.s2_10.libc_io`), WP-004's
 * legacy-spellings set split three ways under WP-016
 * (`n00b.s13.legacy_spellings`, `n00b.s13.legacy_thread`,
 * `n00b.s13.legacy_alignment`), and the path rule
 * `n00b.path.file_open_canonical`. This bulk test pins the WP-003 set
 * specifically; the legacy-spellings rules have their own regression
 * in test_naudit_engine_legacy.c. The rule-count assertion below
 * is pinned to 10 to track the canonical file's exact size (will be
 * updated by future WPs that add more rules).
 *
 * Setup. The Phase 2 loader is invoked against the canonical file at
 * /Users/viega/n00b/audit-rules.bnf (in n00b's tree, not
 * n00b-audit's). The test gates on `access()` and emits `[SKIP]` if
 * the canonical file is absent (process § 6.5b: env gates surface as
 * [SKIP] for cross-machine fixtures, never silent pass). The shape
 * mirrors test_audit_engine_malloc.c.
 *
 * ## Notes — DF-G resolution (WP-003 Phase 1 settlement)
 *
 * The preflight's candidate for each new rule was the pure-BNF
 * approach used by DF-C / DF-F, attaching at `<provided_identifier>`
 * (c_ncc.bnf line 3). Reading c_ncc.bnf in full revealed that TWO of
 * the four target identifier-texts are already-registered keywords:
 *
 *   - `%"__attribute__"` is a keyword at c_ncc.bnf line 82 and feeds
 *     `<_kw_kw_gcc_attribute>` at line 287. It will NOT tokenize as
 *     `%IDENTIFIER`, so the WP-001/WP-002 attachment at
 *     `<provided_identifier>` does not reach it.
 *   - `%"__typeof__"` is a keyword at c_ncc.bnf line 122 and feeds
 *     `<_kw_kw_typeof>` at line 749. Same situation.
 *
 * The other identifier-texts (va_list / va_start / va_arg / va_end,
 * and the libc-I/O set printf / fprintf / puts / fputs / snprintf /
 * sprintf / vfprintf / fopen / fclose / fread / fwrite / getc / putc
 * / open / read / write / close / lseek / dup / dup2 / pipe / socket
 * / connect / bind / listen / accept) are NOT registered keywords;
 * all tokenize via the default `%IDENTIFIER` path and flow through
 * `<provided_identifier>` (c_ncc.bnf line 3) → `<identifier>` (line
 * 448) → `<primary_expression>` (line 571). Same DF-C / DF-F pattern.
 *
 * **DF-G resolutions, per rule.**
 *
 * 1. `n00b.s2_5.attribute_legacy` — collision (attribute keyword
 *    pre-registered). Attach at `<_kw_kw_gcc_attribute>`, the
 *    nonterminal the existing grammar uses to consume the keyword:
 *
 *        <n00b_audit_v_attribute_legacy> ::= %"__attribute__"
 *        <_kw_kw_gcc_attribute> ::= <n00b_audit_v_attribute_legacy>
 *
 *    The wrapper-NT is added as an ALTERNATIVE of
 *    `<_kw_kw_gcc_attribute>` (which originally has two alts:
 *    `%"__attribute__"` and `%"__attribute"`). When the parser
 *    consumes the `__attribute__` keyword token inside an
 *    `<attribute_specifier>`, the wrapper-NT derivation is one of
 *    the valid paths; the parser's Earley forest exposes the
 *    wrapper node and `n00b_pt_search_by_nt` finds it.
 *
 * 2. `n00b.s2_6.typeof_legacy` — collision (typeof keyword
 *    pre-registered). Same shape, attaching at `<_kw_kw_typeof>`:
 *
 *        <n00b_audit_v_typeof_legacy> ::= %"__typeof__"
 *        <_kw_kw_typeof> ::= <n00b_audit_v_typeof_legacy>
 *
 *    Original `<_kw_kw_typeof>` carries three alts (%"typeof",
 *    %"__typeof__", %"__typeof"); we only wrap the legacy form
 *    `%"__typeof__"`. The `typeof` standard spelling stays
 *    un-flagged.
 *
 * 3. `n00b.s2_8.va_list_legacy` — pure-BNF (no collision):
 *
 *        <n00b_audit_v_va_list_legacy> ::= %"va_start"
 *        <n00b_audit_v_va_list_legacy> ::= %"va_arg"
 *        <n00b_audit_v_va_list_legacy> ::= %"va_end"
 *        <provided_identifier> ::= <n00b_audit_v_va_list_legacy>
 *
 *    Three keyword alternatives, attached at `<provided_identifier>`.
 *    Same shape as the WP-002 malloc family.
 *
 *    **Footgun resolved.** The first draft also registered
 *    `%"va_list"`. That broke parsing of `va_list ap;` because
 *    once `va_list` tokenizes as a keyword, it stops fitting
 *    `<typedef_name> ::= <typedef_name_terminal> | %IDENTIFIER`
 *    (c_ncc.bnf line 29) — the declaration is no longer a valid
 *    derivation and the Earley parser explores combinatorially
 *    on the remainder of the file (observed: 90s+ hang). The
 *    `va_list` type itself is detected INDIRECTLY: any legitimate
 *    legacy usage requires one of `va_start` / `va_arg` / `va_end`,
 *    which this rule still catches. The `va_arg(ap, T)` call form
 *    also doesn't fit `<argument_expression_list>` (which is
 *    comma-separated assignment-expressions, not types) — the
 *    fixture deliberately uses `va_start` + `va_end` only to
 *    sidestep that grammar limitation.
 *
 * 4. `n00b.s2_10.libc_io` — pure-BNF (no collision), 26 keyword
 *    alternatives covering the ban list:
 *
 *        <n00b_audit_v_libc_io> ::= %"printf"
 *        <n00b_audit_v_libc_io> ::= %"fprintf"
 *        ... (24 more) ...
 *        <provided_identifier> ::= <n00b_audit_v_libc_io>
 *
 *    Same DF-C / DF-F pattern; the engine handles N alternatives
 *    additively without code changes.
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test — no assertion mentions severity.
 *
 * The eight fixture .c files MUST contain the forbidden identifiers
 * for their respective positive cases (and MUST NOT for negative
 * cases) — the engine PARSES them as target input and is supposed
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

/*
 * The canonical rule set lives in the n00b-audit repo itself
 * (`<repo>/audit-rules.bnf`), reached relative to the baked fixture
 * dir — no longer the default jj workspace path (which coupled the test
 * to a sibling workspace's working copy).
 */
#define N00B_AUDIT_REFERENCE_GUIDANCE_PATH \
    N00B_AUDIT_TEST_FIXTURE_DIR "/../../../audit-rules.bnf"

/*
 * Compare a libn00b string against a C string. Mirrors the helper in
 * test_audit_engine_malloc.c so the assertion shape is identical
 * across the canonical-file tests.
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
 * Load the canonical multi-rule guidance file. Asserts the 10-rule
 * count (2 prior + 4 from WP-003 + WP-004's legacy-spellings set split
 * 3 ways under WP-016 + 1 path rule) and that each WP-003 rule id is
 * present (the legacy-spellings rule ids are asserted by
 * test_naudit_engine_legacy.c).
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
    assert(n00b_list_len(*g->rules) == 10);

    bool    saw_null      = false;
    bool    saw_malloc    = false;
    bool    saw_attribute = false;
    bool    saw_typeof    = false;
    bool    saw_va_list   = false;
    bool    saw_libc_io   = false;
    bool    saw_legacy    = false;
    bool    saw_file_open = false;
    int64_t nrules        = n00b_list_len(*g->rules);
    for (int64_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*g->rules, i);
        assert(!!rule);
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_1.null")) {
            saw_null = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_3.malloc")) {
            saw_malloc = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_5.attribute_legacy")) {
            saw_attribute = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_6.typeof_legacy")) {
            saw_typeof = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_8.va_list_legacy")) {
            saw_va_list = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_10.libc_io")) {
            saw_libc_io = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s13.legacy_spellings")) {
            saw_legacy = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.path.file_open_canonical")) {
            saw_file_open = true;
        }
    }
    assert(saw_null);
    assert(saw_malloc);
    assert(saw_attribute);
    assert(saw_typeof);
    assert(saw_va_list);
    assert(saw_libc_io);
    assert(saw_legacy);
    assert(saw_file_open);

    printf("  [PASS] canonical guidance loads (10 rules)\n");
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
    printf("  [PASS] engine_new (10-rule grammar compose)\n");
}

/*
 * Check a positive fixture: must emit >=1 violation AND at least one
 * violation's rule id must match `expected_rule_id`.
 */
static void
assert_positive_fixture(n00b_audit_engine_t *engine,
                        const char          *fixture_fname,
                        const char          *expected_rule_id)
{
    n00b_string_t *path = fixture_path(fixture_fname);
    auto           r    = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  %s check failed: code=%d (%.*s)\n",
                fixture_fname,
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    int64_t n = n00b_list_len(*violations);
    assert(n >= 1);

    bool saw_expected = false;
    for (int64_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        assert(!!v);
        assert(!!v->rule);
        if (n00b_string_eq_cstr(v->rule->id, expected_rule_id)) {
            saw_expected = true;
            break;
        }
    }
    assert(saw_expected);

    printf("  [PASS] positive fixture %s (n=%lld, has rule_id=%s)\n",
           fixture_fname, (long long)n, expected_rule_id);
}

/*
 * Check a negative fixture: must emit zero violations.
 */
static void
assert_negative_fixture(n00b_audit_engine_t *engine,
                        const char          *fixture_fname)
{
    n00b_string_t *path = fixture_path(fixture_fname);
    auto           r    = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  %s check failed: code=%d (%.*s)\n",
                fixture_fname,
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    assert(n00b_list_len(*violations) == 0);

    printf("  [PASS] negative fixture %s (n=0)\n", fixture_fname);
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

    /* Four new rules — positive + negative fixture per rule. */
    assert_positive_fixture(engine, "fixture_attribute_legacy.c",
                            "n00b.s2_5.attribute_legacy");
    assert_negative_fixture(engine, "fixture_no_attribute_legacy.c");

    assert_positive_fixture(engine, "fixture_typeof_legacy.c",
                            "n00b.s2_6.typeof_legacy");
    assert_negative_fixture(engine, "fixture_no_typeof_legacy.c");

    assert_positive_fixture(engine, "fixture_va_list_legacy.c",
                            "n00b.s2_8.va_list_legacy");
    assert_negative_fixture(engine, "fixture_no_va_list_legacy.c");

    assert_positive_fixture(engine, "fixture_libc_io.c",
                            "n00b.s2_10.libc_io");
    assert_negative_fixture(engine, "fixture_no_libc_io.c");

    /*
     * Architecture-additive sanity: under the 6-rule canonical
     * guidance, the WP-001 NULL rule MUST still fire on
     * fixture_null.c, and the WP-002 malloc rule MUST still fire on
     * fixture_malloc.c.
     */
    assert_positive_fixture(engine, "fixture_null.c", "n00b.s2_1.null");
    assert_positive_fixture(engine, "fixture_malloc.c", "n00b.s2_3.malloc");

    printf("All n00b-audit WP-003 Phase 1 regression checks passed.\n");
    return 0;
}
