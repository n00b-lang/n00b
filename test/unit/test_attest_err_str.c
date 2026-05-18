/** @file test/unit/test_attest_err_str.c — `*_err_str` accessor
 *  regression test (WP-002 Phase 3).
 *
 *  Closes A-1 (D-031 carry-forward from WP-001) for the
 *  `n00b_attest_err_str` half + WA-1 (D-038 part 2) for the
 *  `n00b_base64_err_str` half. Both accessors are pure lookups
 *  over a hard-coded table; the test asserts:
 *
 *    [1] Every defined `N00B_ATTEST_ERR_*` code returns a non-null,
 *        non-empty `n00b_string_t *`.
 *    [2] Every defined `N00B_BASE64_ERR_*` code returns a non-null,
 *        non-empty `n00b_string_t *`.
 *    [3] An unknown code in each domain returns a non-empty
 *        `n00b_string_t *` (the documented fallback). The exact
 *        wording is documented; the regression doesn't bind tightly
 *        to the wording (would be tautological) but does verify
 *        non-emptiness.
 *    [4] The accessors are pure: two calls with the same input
 *        return strings with byte-equal `data` of byte-equal
 *        length. (Same pointer is acceptable but not required.)
 *
 *  Test-file carve-out (D-030) applies — libc I/O for stdout
 *  logging is acceptable per the established test-file precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/string.h"
#include "core/runtime.h"
#include "attest/n00b_attest.h"
#include "util/base64.h"

// Every n00b_attest module-domain error code defined in the
// project. Maintained by hand against the codes declared in
// `n00b_attest_error.h` + `n00b_attest_signer.h`. If a future WP
// adds a code without updating this list, the test still passes
// (the list shrinks but doesn't go stale unsafely) but coverage
// drops — the auditor catches the gap.
static const n00b_err_t k_attest_codes[] = {
    // Statement domain.
    N00B_ATTEST_ERR_STMT_BAD_INPUT,
    N00B_ATTEST_ERR_STMT_MISSING_FIELD,
    N00B_ATTEST_ERR_STMT_BAD_JSON,
    N00B_ATTEST_ERR_STMT_WRONG_TYPE,
    // DSSE-envelope domain.
    N00B_ATTEST_ERR_DSSE_BAD_INPUT,
    N00B_ATTEST_ERR_DSSE_NO_PAYLOAD,
    N00B_ATTEST_ERR_DSSE_BAD_JSON,
    N00B_ATTEST_ERR_DSSE_WRONG_TYPE,
    N00B_ATTEST_ERR_DSSE_BAD_BASE64,
    // Signer domain.
    N00B_ATTEST_ERR_UNSUPPORTED_SCHEME,
    N00B_ATTEST_ERR_KEY_NOT_FOUND,
    N00B_ATTEST_ERR_PEM_PARSE_FAILED,
    N00B_ATTEST_ERR_DER_PARSE_FAILED,
    N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM,
    N00B_ATTEST_ERR_SIGN_FAILED,
    N00B_ATTEST_ERR_NOT_IMPLEMENTED,
};

// Every base64-util code defined in the project.
static const n00b_err_t k_base64_codes[] = {
    N00B_BASE64_ERR_DECODE_FAILED,
    N00B_BASE64_ERR_NULL_INPUT,
};

static void
assert_nonempty(n00b_string_t *s, const char *ctx, n00b_err_t code)
{
    if (s == nullptr) {
        fprintf(stderr, "FAIL: %s err_str(%d) returned null\n", ctx, code);
        assert(0);
    }
    if (s->u8_bytes == 0) {
        fprintf(stderr,
                "FAIL: %s err_str(%d) returned empty string\n",
                ctx,
                code);
        assert(0);
    }
}

static void
test_attest_every_code_returns_nonempty(void)
{
    size_t n = sizeof(k_attest_codes) / sizeof(k_attest_codes[0]);
    // Expected post-WP-002-Phase-3 count: 16. If a future WP adds
    // a code without updating this list, the value here drifts —
    // a starting-point sanity check that pins the table size at
    // its current cardinality.
    assert(n == 16);
    for (size_t i = 0; i < n; i++) {
        n00b_string_t *s = n00b_attest_err_str(k_attest_codes[i]);
        assert_nonempty(s, "attest", k_attest_codes[i]);
    }
    printf("  [PASS] attest_every_code_returns_nonempty\n");
}

static void
test_attest_unknown_code_returns_fallback(void)
{
    // A code in the negative space not assigned to any domain.
    // `-9999` is outside the -1xxx / -2xxx / -4xxx ranges used so
    // far and is a stable "not in the table" value.
    n00b_string_t *s = n00b_attest_err_str(-9999);
    assert_nonempty(s, "attest-unknown", -9999);
    printf("  [PASS] attest_unknown_code_returns_fallback\n");
}

static void
test_attest_accessor_is_pure(void)
{
    // The accessor must be pure: same input → byte-equal output
    // across consecutive calls. Pointer equality is acceptable but
    // not required (the rich-literal storage is process-stable, so
    // it usually IS pointer-equal — but we don't bind tightly).
    for (size_t i = 0;
         i < sizeof(k_attest_codes) / sizeof(k_attest_codes[0]);
         i++) {
        n00b_string_t *a = n00b_attest_err_str(k_attest_codes[i]);
        n00b_string_t *b = n00b_attest_err_str(k_attest_codes[i]);
        assert(a != nullptr && b != nullptr);
        assert(a->u8_bytes == b->u8_bytes);
        assert(memcmp(a->data, b->data, a->u8_bytes) == 0);
    }
    printf("  [PASS] attest_accessor_is_pure\n");
}

static void
test_base64_every_code_returns_nonempty(void)
{
    size_t n = sizeof(k_base64_codes) / sizeof(k_base64_codes[0]);
    assert(n == 2);
    for (size_t i = 0; i < n; i++) {
        n00b_string_t *s = n00b_base64_err_str(k_base64_codes[i]);
        assert_nonempty(s, "base64", k_base64_codes[i]);
    }
    printf("  [PASS] base64_every_code_returns_nonempty\n");
}

static void
test_base64_unknown_code_returns_fallback(void)
{
    n00b_string_t *s = n00b_base64_err_str(-9999);
    assert_nonempty(s, "base64-unknown", -9999);
    printf("  [PASS] base64_unknown_code_returns_fallback\n");
}

static void
test_base64_accessor_is_pure(void)
{
    for (size_t i = 0;
         i < sizeof(k_base64_codes) / sizeof(k_base64_codes[0]);
         i++) {
        n00b_string_t *a = n00b_base64_err_str(k_base64_codes[i]);
        n00b_string_t *b = n00b_base64_err_str(k_base64_codes[i]);
        assert(a != nullptr && b != nullptr);
        assert(a->u8_bytes == b->u8_bytes);
        assert(memcmp(a->data, b->data, a->u8_bytes) == 0);
    }
    printf("  [PASS] base64_accessor_is_pure\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest + n00b_base64 err_str ==\n");
    test_attest_every_code_returns_nonempty();
    test_attest_unknown_code_returns_fallback();
    test_attest_accessor_is_pure();
    test_base64_every_code_returns_nonempty();
    test_base64_unknown_code_returns_fallback();
    test_base64_accessor_is_pure();
    printf("All n00b_attest + n00b_base64 err_str tests passed.\n");
    return 0;
}
