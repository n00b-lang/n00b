/*
 * Regression test — audit-rule-file loader (WP-005 format v2).
 *
 * Exercises `n00b_audit_load_guidance` against three fixtures:
 *
 *   1. test/fixtures/audit/guidance_ok.bnf
 *        — well-formed v1; loader returns ok and the single-rule
 *          rule set parses cleanly.
 *   2. test/fixtures/audit/guidance_missing_field.bnf
 *        — bnf body omitted from the one rule; loader returns
 *          N00B_AUDIT_ERR_GUIDANCE_SCHEMA.
 *   3. test/fixtures/audit/guidance_bad_version.bnf
 *        — @schema_version 2; loader returns
 *          N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION.
 *
 * Bootstrap shape mirrors test/unit/test_audit_module.c — main(argc,
 * argv) plus `n00b_init_simple(argc, argv)` first thing.
 *
 * Test files operate under the n00b-api-guidelines' relaxed test
 * convention — libc <assert.h> + <stdio.h> + <string.h> for harness
 * scaffolding are acceptable here (NCC.md "NO LIBC ALLOWED" exemption
 * for test files; mirror of the Phase 1 test).
 *
 * Fixture-path discovery: pattern 1 from the Phase 2 prompt — the
 * build defines N00B_AUDIT_TEST_FIXTURE_DIR via `-D` in
 * test/meson.build (no hardcoded paths in this C source).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"

#include "naudit/naudit.h"
#include "naudit/guidance.h"
#include "naudit/errors.h"

#ifndef N00B_AUDIT_TEST_FIXTURE_DIR
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build (see test/meson.build)"
#endif

static n00b_string_t *
fixture_path(const char *fname)
{
    char buf[1024];
    int  n = snprintf(buf, sizeof(buf), "%s/%s",
                      N00B_AUDIT_TEST_FIXTURE_DIR, fname);
    assert(n > 0 && (size_t)n < sizeof(buf));
    return n00b_string_from_cstr(buf);
}

static bool
n00b_string_eq_cstr(n00b_string_t *s, const char *expected)
{
    if (!s) {
        return false;
    }
    size_t elen = strlen(expected);
    return s->u8_bytes == elen && memcmp(s->data, expected, elen) == 0;
}

static void
test_load_ok(void)
{
    n00b_string_t *p = fixture_path("guidance_ok.bnf");
    auto r = n00b_audit_load_guidance(p);
    assert(n00b_result_is_ok(r));

    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(g != nullptr);
    assert(g->schema_version == 1);
    assert(g->project        != nullptr);
    assert(g->description    != nullptr);
    assert(g->source_doc     != nullptr);

    assert(g->dependencies != nullptr);
    assert(n00b_list_len(*g->dependencies) == 0);

    assert(g->rules != nullptr);
    assert(n00b_list_len(*g->rules) == 1);

    n00b_audit_rule_t *rule = n00b_list_get(*g->rules, 0);
    assert(rule != nullptr);
    assert(n00b_string_eq_cstr(rule->id, "n00b.s2_1.null"));
    assert(rule->bnf_fragment != nullptr);
    assert(rule->bnf_fragment->u8_bytes > 0);

    printf("  [PASS] load_ok\n");
}

static void
test_load_missing_field(void)
{
    n00b_string_t *p = fixture_path("guidance_missing_field.bnf");
    auto r = n00b_audit_load_guidance(p);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_AUDIT_ERR_GUIDANCE_SCHEMA);

    printf("  [PASS] load_missing_field -> SCHEMA\n");
}

static void
test_load_bad_version(void)
{
    n00b_string_t *p = fixture_path("guidance_bad_version.bnf");
    auto r = n00b_audit_load_guidance(p);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION);

    printf("  [PASS] load_bad_version -> SCHEMA_VERSION\n");
}

static void
test_err_str_coverage(void)
{
    /*
     * Sanity check: every code declared in errors.h round-trips
     * through n00b_audit_err_str to a non-null rich literal, and
     * unknown codes hit the default branch (also non-null). Per
     * n00b-api-guidelines § 5.5, this is the round-trip rule.
     */
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_GUIDANCE_NOT_FOUND)        != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_GUIDANCE_PARSE)            != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_GUIDANCE_SCHEMA)           != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_GUIDANCE_SCHEMA_VERSION)   != nullptr);
    assert(n00b_audit_err_str(N00B_AUDIT_ERR_GUIDANCE_DEPS_UNIMPLEMENTED) != nullptr);
    assert(n00b_audit_err_str(0) != nullptr);

    printf("  [PASS] err_str coverage\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    test_load_ok();
    test_load_missing_field();
    test_load_bad_version();
    test_err_str_coverage();

    printf("All n00b-audit Phase 2 regression checks passed.\n");
    return 0;
}
