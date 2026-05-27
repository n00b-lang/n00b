/*
 * WP-007 Phase 2 regression test — engine populates the new
 * rewrite + span fields on `n00b_audit_violation_t`.
 *
 * Covers:
 *   1. A guidance fragment that DOES carry a rewrite block (NULL →
 *      nullptr) → engine emits violations whose `rewrite` field is
 *      the rewrite template ("nullptr"), and whose end_line /
 *      end_column come from slay's per-token endcol.
 *   2. A guidance fragment that does NOT carry a rewrite block →
 *      engine emits violations whose `rewrite` field stays nullptr.
 *
 * Bootstrap shape mirrors test_naudit_engine.c per the relaxed
 * test-file convention.
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
 * Load `guidance_ok.bnf` (which carries the file-meta envelope plus
 * a placeholder rule) and patch the rule's bnf_fragment +
 * violation_nt to the given values. Returns the loaded guidance.
 *
 * The fragment can include `rewrite { template: ... }` blocks —
 * the audit-rule-file loader treats every line in the rule's BNF
 * body opaquely (collecting BNF_LINE + INDENT_LINE text), so the
 * rewrite block is forwarded to slay's BNF tokenizer as part of the
 * `bnf_fragment` string.
 */
static n00b_audit_guidance_t *
load_with_fragment(const char *fragment, const char *violation_nt)
{
    n00b_string_t *p = fixture_path("guidance_ok.bnf");
    auto           r = n00b_audit_load_guidance(p);
    if (n00b_result_is_err(r)) {
        fprintf(stderr,
                "  load_with_fragment: guidance load err=%d\n",
                n00b_result_get_err(r));
    }
    assert(n00b_result_is_ok(r));
    n00b_audit_guidance_t *g = n00b_result_get(r);
    assert(g != nullptr && g->rules != nullptr);
    assert(n00b_list_len(*g->rules) == 1);

    n00b_audit_rule_t *rule = n00b_list_get(*g->rules, 0);
    assert(rule != nullptr);
    rule->bnf_fragment = n00b_string_from_cstr(fragment);
    rule->violation_nt = n00b_string_from_cstr(violation_nt);
    return g;
}

static void
test_violation_has_rewrite(void)
{
    /*
     * Hand-rolled fragment with a rewrite block. The audit-rule-file
     * loader will collect every line of the rule body (including the
     * indented `template: …` and `description: …` lines from inside
     * the rewrite block) as the fragment text; slay's BNF tokenizer
     * then ingests `rewrite { … }` as one contiguous span.
     */
    const char *frag =
        "<n00b_audit_v_null> ::= %\"NULL\"\n"
        "rewrite {\n"
        "    template: nullptr\n"
        "    description: NULL -> nullptr (C23).\n"
        "}\n"
        "<provided_identifier> ::= <n00b_audit_v_null>\n";

    n00b_audit_guidance_t *g = load_with_fragment(frag, "n00b_audit_v_null");

    auto er = n00b_audit_engine_new(g);
    if (n00b_result_is_err(er)) {
        fprintf(stderr,
                "  engine_new err=%d\n", n00b_result_get_err(er));
    }
    assert(n00b_result_is_ok(er));
    n00b_audit_engine_t *eng = n00b_result_get(er);

    auto cr = n00b_audit_engine_check_file(eng, fixture_path("fixture_null.c"));
    assert(n00b_result_is_ok(cr));
    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(cr);
    assert(n00b_list_len(*violations) >= 1);

    n00b_audit_violation_t *v0 = n00b_list_get(*violations, 0);
    assert(v0 != nullptr);
    assert(v0->rewrite != nullptr);
    /* The slay rewrite-block parser strips per-field whitespace and
     * trailing newlines off `template:`, so the value is exactly
     * "nullptr". */
    if (v0->rewrite->u8_bytes != 7
        || memcmp(v0->rewrite->data, "nullptr", 7) != 0) {
        fprintf(stderr,
                "  expected rewrite=\"nullptr\"; got u8=%lld data=\"%.*s\"\n",
                (long long)v0->rewrite->u8_bytes,
                (int)v0->rewrite->u8_bytes,
                v0->rewrite->data);
    }
    assert(v0->rewrite->u8_bytes == 7);
    assert(memcmp(v0->rewrite->data, "nullptr", 7) == 0);

    /* End-line/col should be a non-zero single-line span. */
    assert(v0->end_line >= v0->line);
    assert(v0->end_column > v0->column);

    printf("  [PASS] violation_has_rewrite (rewrite=\"nullptr\", "
           "line=%lld..%lld col=%lld..%lld)\n",
           (long long)v0->line, (long long)v0->end_line,
           (long long)v0->column, (long long)v0->end_column);
}

static void
test_violation_no_rewrite(void)
{
    /* Same NULL-matching fragment but WITHOUT a rewrite block. */
    const char *frag =
        "<n00b_audit_v_null> ::= %\"NULL\"\n"
        "<provided_identifier> ::= <n00b_audit_v_null>\n";

    n00b_audit_guidance_t *g = load_with_fragment(frag, "n00b_audit_v_null");

    auto er = n00b_audit_engine_new(g);
    assert(n00b_result_is_ok(er));
    n00b_audit_engine_t *eng = n00b_result_get(er);

    auto cr = n00b_audit_engine_check_file(eng, fixture_path("fixture_null.c"));
    assert(n00b_result_is_ok(cr));
    n00b_list_t(n00b_audit_violation_t *) *violations = n00b_result_get(cr);
    assert(n00b_list_len(*violations) >= 1);

    n00b_audit_violation_t *v0 = n00b_list_get(*violations, 0);
    assert(v0 != nullptr);
    assert(v0->rewrite == nullptr);

    /* end_line/end_column still populated; they don't depend on the
     * rewrite-block presence. */
    assert(v0->end_line >= v0->line);
    assert(v0->end_column > v0->column);

    printf("  [PASS] violation_no_rewrite (rewrite=nullptr, "
           "line=%lld..%lld col=%lld..%lld)\n",
           (long long)v0->line, (long long)v0->end_line,
           (long long)v0->column, (long long)v0->end_column);
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    test_violation_has_rewrite();
    test_violation_no_rewrite();

    printf("All n00b-audit WP-007 Phase 2 rewrite regression checks passed.\n");
    return 0;
}
