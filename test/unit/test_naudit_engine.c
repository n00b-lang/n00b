/*
 * WP-001 Phase 3 regression test — audit engine core.
 *
 * Exercises the engine's grammar-compose + parse-forest-walk pipeline
 * against the two fixture sources under test/fixtures/audit/:
 *
 *   1. fixture_null.c   — contains `NULL` in two expression
 *                         positions (lines 5 and 6 of the fixture);
 *                         engine must emit at least one violation
 *                         whose `line` matches the fixture's first
 *                         NULL site.
 *   2. fixture_nullptr.c — contains `nullptr` in the same two
 *                          positions; engine must emit zero
 *                          violations.
 *   3. does_not_exist.c — engine must surface
 *                         N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND.
 *
 * Setup. The loader is invoked against guidance_ok.bnf from
 * test/fixtures/audit/, which carries placeholder `bnf_fragment` and
 * `violation_nt` values. The test overwrites those two fields on the
 * loaded rule with the Phase 3-resolved values before constructing
 * the engine — this exercises the loader+engine integration without
 * requiring the canonical guidance file (that lands in Phase 6).
 *
 * Bootstrap shape mirrors test/unit/test_audit_guidance.c per
 * relaxed test convention: libc <assert.h> + <stdio.h> are allowed
 * for harness scaffolding (NCC.md "NO LIBC ALLOWED" exemption for
 * test files), main(argc, argv) plus n00b_init_simple(argc, argv)
 * first thing, fixture-path discovery via the
 * N00B_AUDIT_TEST_FIXTURE_DIR macro (no hardcoded paths in the C
 * source — set by test/meson.build).
 *
 * Per project DECISIONS.md D-005, there is no `severity` field
 * anywhere in this test — no assertion mentions severity, no
 * field is checked on n00b_audit_violation_t for severity, the
 * loader's per-rule shape has no severity field, the error codes
 * carry no severity.
 *
 * The two fixture .c files MUST contain `NULL` and `nullptr`
 * respectively — the engine PARSES them as target input and is
 * supposed to flag NULL. They are not n00b-audit source code; the
 * n00b-api-guidelines § 2.1 NULL→nullptr rule does NOT apply to
 * them.
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
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build (see test/meson.build)"
#endif

/*
 * Pin the line numbers from the fixture files. fixture_null.c places
 * `NULL` first on line 5 (`int *p = NULL;`); fixture_nullptr.c
 * places `nullptr` on line 5. The numbers are observed from the
 * checked-in fixture files, not invented (per the cross-project
 * numeric-claims rule — see ~/CLAUDE.md feedback_numeric_claims.md).
 * Update these constants if the fixtures' layout changes.
 */
#define FIXTURE_NULL_FIRST_LINE     5

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
 * Load guidance_ok.bnf and patch the rule's bnf_fragment +
 * violation_nt fields to the Phase 3-resolved values. Returns the
 * fully-populated guidance pointer.
 */
static n00b_audit_guidance_t *
load_phase3_guidance(void)
{
    n00b_string_t *p = fixture_path("guidance_ok.bnf");
    auto r = n00b_audit_load_guidance(p);
    assert(n00b_result_is_ok(r));

    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(g != nullptr);
    assert(g->rules != nullptr);
    assert(n00b_list_len(*g->rules) == 1);

    n00b_audit_rule_t *rule = n00b_list_get(*g->rules, 0);
    assert(rule != nullptr);

    /*
     * Inject the DF-C / DF-D-resolved values. The fragment registers
     * `NULL` as a keyword terminal and adds a new alternative on
     * `<provided_identifier>` whose RHS is that keyword. Match the
     * recorded values in src/audit/engine.c's `## Notes` block
     * verbatim — Phase 6's reference guidance file will use the
     * same strings.
     */
    rule->bnf_fragment = n00b_string_from_cstr(
        "<n00b_audit_v_null> ::= %\"NULL\"\n"
        "<provided_identifier> ::= <n00b_audit_v_null>\n");
    rule->violation_nt = n00b_string_from_cstr("n00b_audit_v_null");

    return g;
}

static void
test_engine_new_ok(n00b_audit_engine_t **out_engine,
                   n00b_audit_guidance_t *guidance)
{
    auto r = n00b_audit_engine_new(guidance);
    assert(n00b_result_is_ok(r));
    *out_engine = n00b_result_get(r);
    assert(*out_engine != nullptr);
    printf("  [PASS] engine_new\n");
}

static void
test_check_fixture_null(n00b_audit_engine_t *engine,
                        n00b_audit_guidance_t *guidance)
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
    assert(violations != nullptr);
    int64_t n = n00b_list_len(*violations);
    assert(n >= 1);

    n00b_audit_violation_t *v0 = n00b_list_get(*violations, 0);
    assert(v0 != nullptr);
    assert(v0->rule != nullptr);
    assert(v0->rule == n00b_list_get(*guidance->rules, 0));
    assert(v0->file != nullptr);
    assert(v0->line == FIXTURE_NULL_FIRST_LINE);
    assert(v0->column > 0);

    printf("  [PASS] check_fixture_null (n=%lld, line=%lld, col=%lld)\n",
           (long long)n, (long long)v0->line, (long long)v0->column);
}

static void
test_check_fixture_nullptr(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("fixture_nullptr.c");
    auto r = n00b_audit_engine_check_file(engine, path);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  fixture_nullptr.c check failed: code=%d (%.*s)\n",
                n00b_result_get_err(r),
                (int)n00b_audit_err_str(n00b_result_get_err(r))->u8_bytes,
                n00b_audit_err_str(n00b_result_get_err(r))->data);
    }
    assert(n00b_result_is_ok(r));

    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(r);
    assert(violations != nullptr);
    assert(n00b_list_len(*violations) == 0);

    printf("  [PASS] check_fixture_nullptr (n=0)\n");
}

static void
test_check_missing_target(n00b_audit_engine_t *engine)
{
    n00b_string_t *path = fixture_path("does_not_exist.c");
    auto r = n00b_audit_engine_check_file(engine, path);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND);
    printf("  [PASS] check_missing_target -> TARGET_NOT_FOUND\n");
}

static void
test_err_str_engine_codes(void)
{
    /*
     * Round-trip the five engine error codes through err_str — per
     * n00b-api-guidelines § 5.5, every declared code must round-trip
     * to a non-null rich-literal description.
     */
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_ENGINE_GRAMMAR_LOAD)     != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_ENGINE_RULE_MERGE)       != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_ENGINE_PARSE)            != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_ENGINE_TARGET_NOT_FOUND) != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_ENGINE_BAD_ARGS)         != nullptr);
    printf("  [PASS] err_str engine codes\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    n00b_audit_guidance_t *guidance = load_phase3_guidance();
    n00b_audit_engine_t   *engine   = nullptr;

    test_engine_new_ok(&engine, guidance);
    test_check_fixture_null(engine, guidance);
    test_check_fixture_nullptr(engine);
    test_check_missing_target(engine);
    test_err_str_engine_codes();

    printf("All n00b-audit Phase 3 regression checks passed.\n");
    return 0;
}
