/*
 * WP-011 Phase 1 — unit tests for the exemption foundation.
 *
 * Coverage:
 *   1. Rule content-hash determinism:
 *      hash(X) == hash(X) across invocations.
 *   2. Rule content-hash sensitivity:
 *      hash(production X) != hash(production Y) when the bytes
 *      differ in any token.
 *   3. Rule content-hash comment + whitespace invariance:
 *      hash(X) == hash(X with comments and leading whitespace
 *      added) — the canonicalization strips both.
 *   4. Region fingerprint determinism.
 *   5. Region fingerprint whitespace invariance:
 *      fp("  int *p = NULL;  ") == fp("int * p = NULL;").
 *   6. Region fingerprint token-text sensitivity.
 *   7. Exemption-file loader smoke test:
 *      hand-crafted exemption fixture with a known
 *      (rule_id, region_fingerprint) pair loads successfully and
 *      its `n00b_audit_exemption_match` returns true for a
 *      synthetic violation built from the matching fields.
 *   8. Mismatched hash / fingerprint → match returns false.
 *
 * Bootstrap shape mirrors test_naudit_engine.c per the relaxed
 * test-file convention: libc `<assert.h>` + `<stdio.h>` are
 * allowed for harness scaffolding (NCC.md "NO LIBC ALLOWED"
 * exemption for test files).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "text/strings/string_ops.h"

#include "naudit/naudit.h"
#include "naudit/exemption.h"
#include "naudit/rule.h"
#include "naudit/violation.h"

#ifndef N00B_AUDIT_TEST_FIXTURE_DIR
#error "N00B_AUDIT_TEST_FIXTURE_DIR must be set by the build"
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

/* ---------------------------------------------------------------- */
/* Rule content-hash tests                                          */
/* ---------------------------------------------------------------- */

static void
test_rule_hash_determinism(void)
{
    n00b_string_t *bnf = n00b_string_from_cstr(
        "<my_violation_nt> ::= %\"NULL\"\n"
        "<provided_identifier> ::= <my_violation_nt>\n");
    n00b_string_t *h1 = n00b_audit_compute_rule_content_hash(bnf);
    n00b_string_t *h2 = n00b_audit_compute_rule_content_hash(bnf);
    assert(h1 != nullptr);
    assert(h2 != nullptr);
    assert(h1->u8_bytes == 32);
    assert(h2->u8_bytes == 32);
    assert(n00b_unicode_str_eq(h1, h2));
    printf("  [PASS] rule_hash_determinism hash=%.*s\n",
           (int)h1->u8_bytes, h1->data);
}

static void
test_rule_hash_sensitivity(void)
{
    n00b_string_t *bnf_a = n00b_string_from_cstr(
        "<my_violation_nt> ::= %\"NULL\"\n");
    n00b_string_t *bnf_b = n00b_string_from_cstr(
        "<my_violation_nt> ::= %\"nullptr\"\n");
    n00b_string_t *ha = n00b_audit_compute_rule_content_hash(bnf_a);
    n00b_string_t *hb = n00b_audit_compute_rule_content_hash(bnf_b);
    assert(!n00b_unicode_str_eq(ha, hb));
    printf("  [PASS] rule_hash_sensitivity (NULL vs nullptr differ)\n");
}

static void
test_rule_hash_comment_whitespace_invariance(void)
{
    n00b_string_t *plain = n00b_string_from_cstr(
        "<my_violation_nt> ::= %\"NULL\"\n"
        "<provided_identifier> ::= <my_violation_nt>\n");
    n00b_string_t *decorated = n00b_string_from_cstr(
        "# leading comment line\n"
        "    <my_violation_nt> ::= %\"NULL\"  \n"
        "\n"
        "  # another comment\n"
        "<provided_identifier> ::= <my_violation_nt>\n");
    n00b_string_t *hp = n00b_audit_compute_rule_content_hash(plain);
    n00b_string_t *hd = n00b_audit_compute_rule_content_hash(decorated);
    assert(n00b_unicode_str_eq(hp, hd));
    printf("  [PASS] rule_hash_comment_whitespace_invariance\n");
}

/* ---------------------------------------------------------------- */
/* Region fingerprint tests                                         */
/* ---------------------------------------------------------------- */

static void
test_region_fingerprint_determinism(void)
{
    n00b_string_t *r = n00b_string_from_cstr("int *p = NULL;");
    n00b_string_t *f1 = n00b_audit_compute_region_fingerprint(r);
    n00b_string_t *f2 = n00b_audit_compute_region_fingerprint(r);
    assert(f1->u8_bytes == 32);
    assert(n00b_unicode_str_eq(f1, f2));
    printf("  [PASS] region_fingerprint_determinism fp=%.*s\n",
           (int)f1->u8_bytes, f1->data);
}

static void
test_region_fingerprint_ws_invariance(void)
{
    n00b_string_t *tight = n00b_string_from_cstr("int *p = NULL;");
    n00b_string_t *loose = n00b_string_from_cstr(
        "   int  *p\t=\tNULL;\t  ");
    n00b_string_t *ft = n00b_audit_compute_region_fingerprint(tight);
    n00b_string_t *fl = n00b_audit_compute_region_fingerprint(loose);
    /*
     * "int *p = NULL;" -> "int *p = NULL;"
     * "   int  *p\t=\tNULL;\t  " ->
     *   trim per line -> "int  *p\t=\tNULL;"
     *   collapse internal ws -> "int *p = NULL;"
     * Both should fingerprint identically.
     */
    assert(n00b_unicode_str_eq(ft, fl));
    printf("  [PASS] region_fingerprint_ws_invariance\n");
}

static void
test_region_fingerprint_text_sensitivity(void)
{
    n00b_string_t *r_null = n00b_string_from_cstr("int *p = NULL;");
    n00b_string_t *r_nullptr = n00b_string_from_cstr("int *p = nullptr;");
    n00b_string_t *fn = n00b_audit_compute_region_fingerprint(r_null);
    n00b_string_t *fp = n00b_audit_compute_region_fingerprint(r_nullptr);
    assert(!n00b_unicode_str_eq(fn, fp));
    printf("  [PASS] region_fingerprint_text_sensitivity\n");
}

/* ---------------------------------------------------------------- */
/* Exemption-file loader + match tests                              */
/* ---------------------------------------------------------------- */

static void
test_load_exemption_match(void)
{
    /*
     * Hand-craft an exemption file with one entry; build a
     * synthetic violation whose hash + fingerprint match the
     * file's. The match predicate must return true.
     *
     * The fixture file lives at fixture path so it's reproducible.
     * Its rule_id + region_fingerprint values are derived from
     * `n00b_audit_compute_*` on a known input — we compute them
     * here, write the file on the fly, and load it back.
     */
    n00b_string_t *bnf = n00b_string_from_cstr(
        "<v> ::= %\"NULL\"\n");
    n00b_string_t *hash = n00b_audit_compute_rule_content_hash(bnf);

    n00b_string_t *region = n00b_string_from_cstr("int *p = NULL;");
    n00b_string_t *fp     = n00b_audit_compute_region_fingerprint(region);

    n00b_string_t *path = fixture_path("exemption_match_dynamic.bnf");
    FILE *fh = fopen(path->data, "w");
    assert(fh != nullptr);
    fprintf(fh, "@schema_version 1\n\n");
    fprintf(fh, "@exemption test_match_0001\n");
    fprintf(fh, "@version 1\n");
    fprintf(fh, "@rule_id %.*s\n", (int)hash->u8_bytes, hash->data);
    fprintf(fh, "@rule_name n00b.test.synthetic\n");
    fprintf(fh, "@region_fingerprint %.*s\n",
            (int)fp->u8_bytes, fp->data);
    fprintf(fh, "@rationale dynamic-test-only\n");
    fprintf(fh, "@signer_id placeholder\n");
    fclose(fh);

    auto lr = n00b_audit_load_exemptions(path);
    assert(n00b_result_is_ok(lr));
    n00b_list_t(n00b_audit_exemption_t *) *list = n00b_result_get(lr);
    assert(list != nullptr);
    assert(n00b_list_len(*list) == 1);

    n00b_audit_exemption_t *ex = n00b_list_get(*list, 0);
    assert(ex != nullptr);
    assert(ex->version == 1);
    assert(n00b_unicode_str_eq(ex->rule_id, hash));
    assert(n00b_unicode_str_eq(ex->region_fingerprint, fp));

    /* Build a synthetic violation whose rule carries the same hash. */
    n00b_audit_rule_t      *rule = n00b_alloc(n00b_audit_rule_t);
    rule->id                 = n00b_string_from_cstr("n00b.test.synthetic");
    rule->title              = n00b_string_from_cstr("placeholder");
    rule->section            = n00b_string_from_cstr("section x");
    rule->bnf_fragment       = bnf;
    rule->violation_nt       = n00b_string_from_cstr("v");
    rule->rationale          = n00b_string_from_cstr("placeholder");
    rule->bad_example        = n00b_string_from_cstr("placeholder");
    rule->good_example       = n00b_string_from_cstr("placeholder");
    rule->guidance           = n00b_string_from_cstr("placeholder");
    rule->applies_to_include = nullptr;
    rule->applies_to_exclude = nullptr;
    rule->language           = nullptr;
    rule->filter_name        = nullptr;
    rule->captures           = nullptr;
    rule->content_hash       = hash;

    n00b_audit_violation_t *v = n00b_alloc(n00b_audit_violation_t);
    v->file       = n00b_string_from_cstr("/tmp/test.c");
    v->line       = 1;
    v->column     = 1;
    v->end_line   = 1;
    v->end_column = 15;
    v->rule       = rule;
    v->rewrite    = nullptr;
    v->region_fingerprint = fp;

    assert(n00b_audit_exemption_match(ex, v));
    printf("  [PASS] load_exemption_match (rule_id + fp align)\n");

    /* Mismatched hash → no match. */
    n00b_string_t *bad_hash = n00b_string_from_cstr(
        "ffffffffffffffffffffffffffffffff");
    rule->content_hash = bad_hash;
    assert(!n00b_audit_exemption_match(ex, v));

    /* Restore hash; mismatched fingerprint → no match. */
    rule->content_hash    = hash;
    v->region_fingerprint = n00b_string_from_cstr(
        "00000000000000000000000000000000");
    assert(!n00b_audit_exemption_match(ex, v));
    printf("  [PASS] load_exemption_no_match (hash / fp mismatch)\n");

    /* Cleanup. */
    remove(path->data);
}

static void
test_load_exemption_mismatch_fixture(void)
{
    /*
     * Load the checked-in mismatch fixture (wrong rule_id + wrong
     * fingerprint). Match against a synthetic violation with a
     * known hash + fp. Should NOT match.
     */
    n00b_string_t *path = fixture_path("exemption_mismatch.bnf");
    auto lr = n00b_audit_load_exemptions(path);
    assert(n00b_result_is_ok(lr));
    n00b_list_t(n00b_audit_exemption_t *) *list = n00b_result_get(lr);
    assert(list != nullptr);
    assert(n00b_list_len(*list) >= 1);

    n00b_audit_exemption_t *ex = n00b_list_get(*list, 0);
    assert(ex != nullptr);

    /* Build a violation that carries DIFFERENT values. */
    n00b_audit_rule_t      *rule = n00b_alloc(n00b_audit_rule_t);
    rule->content_hash = n00b_string_from_cstr(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    n00b_audit_violation_t *v = n00b_alloc(n00b_audit_violation_t);
    v->rule               = rule;
    v->region_fingerprint = n00b_string_from_cstr(
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

    assert(!n00b_audit_exemption_match(ex, v));
    printf("  [PASS] load_exemption_mismatch_fixture (no match)\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    test_rule_hash_determinism();
    test_rule_hash_sensitivity();
    test_rule_hash_comment_whitespace_invariance();
    test_region_fingerprint_determinism();
    test_region_fingerprint_ws_invariance();
    test_region_fingerprint_text_sensitivity();
    test_load_exemption_match();
    test_load_exemption_mismatch_fixture();

    printf("All naudit exemption WP-011 unit checks passed.\n");
    return 0;
}
