/*
 * WP-002 Phase 1 regression test — multi-rule engine + malloc family.
 *
 * Exercises the engine's grammar-compose + parse-forest-walk pipeline
 * against the canonical multi-rule guidance file in the n00b repo,
 * running three fixture sources under test/fixtures/audit/:
 *
 *   1. fixture_malloc.c     — contains `malloc` on line 6 and `free`
 *                             on line 7 (expression position via the
 *                             postfix-call form); engine MUST emit
 *                             at least one violation whose
 *                             `rule->id` equals `n00b.s2_3.malloc`.
 *   2. fixture_no_malloc.c  — uses `n00b_alloc` / `n00b_free`
 *                             (deliberately excluded from the
 *                             malloc-family keyword list); engine
 *                             MUST emit zero violations.
 *   3. fixture_null.c       — WP-001's NULL fixture; under the
 *                             multi-rule canonical guidance the
 *                             WP-001 `n00b.s2_1.null` rule MUST
 *                             still fire. This is the WP-002
 *                             architecture-additive sanity check.
 *
 * Setup. The Phase 2 loader is invoked against the canonical file
 * at `/Users/viega/n00b/audit-rules.bnf` (in n00b's
 * tree, not n00b-audit's). The test gates on `access()` and emits
 * `[SKIP]` if the canonical file is absent (process § 6.5b: env
 * gates surface as [SKIP] for cross-machine fixtures, never silent
 * pass). The shape mirrors test_audit_reference_guidance.c's gate.
 *
 * ## Notes — DF-F resolution (WP-002 Phase 1 settlement)
 *
 * The preflight's candidate for the malloc rule was the pure-BNF
 * approach used by DF-C, attaching at `<provided_identifier>`
 * (c_ncc.bnf line 3). Reading c_ncc.bnf in full confirmed that:
 *
 *   - None of `malloc`, `calloc`, `realloc`, `free`,
 *     `_n00b_alloc_raw` are registered keywords anywhere in
 *     c_ncc.bnf. All five tokenize via the default
 *     `%IDENTIFIER` path, which flows through
 *     `<provided_identifier>` (line 3) → `<identifier>`
 *     (line 448) → `<primary_expression>` (line 571) →
 *     `<postfix_expression>` (line 549). The call form
 *     `<postfix_expression> %"(" <argument_expression_list>? %")"`
 *     (line 541) reaches the same identifiers at call sites.
 *   - Underscore-prefixed keyword terminals are well-supported
 *     by the c_ncc.bnf tokenizer — see existing keywords
 *     `%"_Once"` (line 4), `%"_kargs"` (line 5), the full
 *     `_Alignas` / `_Atomic` / `_BitInt` family (lines 128+).
 *     `_n00b_alloc_raw` introduces no new tokenizer behavior.
 *
 * **DF-F resolution.** Same pure-BNF approach as DF-C, with five
 * keyword alternatives instead of one. The composed fragment is:
 *
 *     <n00b_audit_v_malloc_call> ::= %"malloc"
 *     <n00b_audit_v_malloc_call> ::= %"calloc"
 *     <n00b_audit_v_malloc_call> ::= %"realloc"
 *     <n00b_audit_v_malloc_call> ::= %"free"
 *     <n00b_audit_v_malloc_call> ::= %"_n00b_alloc_raw"
 *     <provided_identifier> ::= <n00b_audit_v_malloc_call>
 *
 * Effect: when the c_tokenizer sees any of the five identifier
 * texts, it hashes the text and finds the newly-registered keyword
 * id, emitting the corresponding `%"…"` terminal rather than
 * `%IDENTIFIER`. The parse tree then contains a
 * `<n00b_audit_v_malloc_call>` node wrapping the token at every
 * site where the identifier appears in expression position
 * (including call sites — `<postfix_expression>` reaches the
 * identifier through `<primary_expression>`). `n00b_alloc` and
 * `n00b_free` are deliberately excluded from the keyword list, so
 * they tokenize as plain `%IDENTIFIER` and are not flagged. The
 * approach is therefore **pure-BNF**; no post-walk text
 * discriminator is needed in the engine.
 *
 * Attachment point cited: c_ncc.bnf line 3,
 * `<provided_identifier> ::= %IDENTIFIER`. Same attachment point
 * as DF-C — the WP-001 architecture extends additively to the
 * five-keyword-alternative shape without any engine change.
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test — no assertion mentions severity, no
 * field is checked on `n00b_audit_violation_t` for severity, the
 * loader's per-rule shape has no severity field, the error codes
 * carry no severity.
 *
 * The three fixture .c files MUST contain `malloc` / `free` /
 * `NULL` respectively (and MUST NOT contain `n00b_alloc` /
 * `n00b_free` for `fixture_malloc.c`) — the engine PARSES them as
 * target input and is supposed to flag the listed identifiers.
 * They are not n00b-audit source code; the n00b-api-guidelines
 * § 2.1 NULL→nullptr and § 2.3 malloc→n00b_alloc rules do NOT
 * apply to them.
 *
 * Bootstrap shape mirrors test/unit/test_audit_engine.c per
 * relaxed test convention: libc <assert.h> + <stdio.h> +
 * <string.h> + <unistd.h> are allowed for harness scaffolding
 * (NCC.md "NO LIBC ALLOWED" exemption for test files),
 * main(argc, argv) plus n00b_init_simple(argc, argv) first thing,
 * fixture-path discovery via the N00B_AUDIT_TEST_FIXTURE_DIR
 * macro (no hardcoded paths in the C source — set by
 * test/meson.build), `access()`-gate + `[SKIP]` for the
 * cross-repo canonical file path.
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
 * Pin the line numbers from the fixture files. fixture_malloc.c
 * places `malloc` on line 6 (`int *p = malloc(sizeof(int));`) and
 * `free` on line 7 (`free(p);`); fixture_null.c places `NULL` on
 * line 5 (`int *p = NULL;`). The numbers are observed from the
 * checked-in fixture files, not invented (per the cross-project
 * numeric-claims rule — see ~/CLAUDE.md feedback_numeric_claims.md).
 * Update these constants if the fixtures' layout changes.
 */
#define FIXTURE_MALLOC_FIRST_LINE   6
#define FIXTURE_NULL_FIRST_LINE     5

/*
 * Compare a libn00b string against a C string. Mirrors the helper
 * in test/unit/test_audit_guidance.c and
 * test/unit/test_audit_reference_guidance.c so the assertion shape
 * is identical across the canonical-file tests.
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
 * multi-rule sanity check (rules.len == 2).
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
    /*
     * The canonical file grows as new rules ship across WPs. WP-002
     * shipped 2 rules; WP-003 grew to 6. This test asserts the two
     * rule ids it cares about are PRESENT rather than pinning a hard
     * count — keeps the WP-002 regression valid as the canonical
     * file accretes additional rules in successor WPs.
     */
    assert(n00b_list_len(*g->rules) >= 2);

    /*
     * Confirm both rule ids are present — order isn't load-bearing
     * here (the engine iterates the list), but both must appear.
     */
    bool    saw_null   = false;
    bool    saw_malloc = false;
    int64_t nrules     = n00b_list_len(*g->rules);
    for (int64_t i = 0; i < nrules; i++) {
        n00b_audit_rule_t *rule = n00b_list_get(*g->rules, i);
        assert(!!rule);
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_1.null")) {
            saw_null = true;
        }
        if (n00b_string_eq_cstr(rule->id, "n00b.s2_3.malloc")) {
            saw_malloc = true;
        }
    }
    assert(saw_null);
    assert(saw_malloc);

    printf("  [PASS] canonical guidance loads (>=2 rules incl. null + malloc)\n");
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
    printf("  [PASS] engine_new (multi-rule grammar compose)\n");
}

static void
test_check_fixture_malloc(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_malloc.c");
    auto r = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_malloc.c check failed: code=%d (%.*s)\n",
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
     * Walk every violation; at least one must be the malloc rule,
     * and the first one's line must pin to the malloc fixture's
     * `malloc(sizeof(int))` site on line 6.
     */
    bool saw_malloc_rule = false;
    n00b_audit_violation_t *first_malloc = nullptr;
    for (int64_t i = 0; i < n; i++) {
        n00b_audit_violation_t *v = n00b_list_get(*violations, i);
        assert(!!v);
        assert(!!v->rule);
        if (n00b_string_eq_cstr(v->rule->id, "n00b.s2_3.malloc")) {
            saw_malloc_rule = true;
            if (!first_malloc) {
                first_malloc = v;
            }
        }
    }
    assert(saw_malloc_rule);
    assert(!!first_malloc);
    assert(!!first_malloc->file);
    assert(first_malloc->line == FIXTURE_MALLOC_FIRST_LINE);
    assert(first_malloc->column > 0);

    printf("  [PASS] check_fixture_malloc "
           "(n=%lld, first_malloc_line=%lld, col=%lld)\n",
           (long long)n,
           (long long)first_malloc->line,
           (long long)first_malloc->column);
}

static void
test_check_fixture_no_malloc(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_no_malloc.c");
    auto r = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_no_malloc.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(!!violations);
    assert(n00b_list_len(*violations) == 0);

    printf("  [PASS] check_fixture_no_malloc (n=0)\n");
}

/*
 * Architecture-additive sanity check: under the canonical
 * multi-rule guidance, the WP-001 NULL rule must STILL fire on
 * fixture_null.c. This is the WP-002 stress point — the engine
 * iterates `guidance->rules`; both rules must produce their
 * violations independently.
 */
static void
test_check_fixture_null_still_fires(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_null.c");
    auto r = n00b_audit_engine_check_file(engine, path);
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

    test_engine_new_ok(&engine, guidance);
    test_check_fixture_malloc(engine);
    test_check_fixture_no_malloc(engine);
    test_check_fixture_null_still_fires(engine);

    printf("All n00b-audit WP-002 Phase 1 regression checks passed.\n");
    return 0;
}
