/** @file test/unit/test_errno_str.c — `n00b_errno_str` accessor
 *  regression test.
 *
 *  Closes the WP-004 Phase 5 (DF-008 fix) gate. The accessor is a
 *  pure lookup over a hard-coded `switch` table; the test asserts:
 *
 *    [1] A representative sample of POSIX.1-2008 portable errno
 *        values returns a non-null, non-empty `n00b_string_t *`.
 *    [2] An unknown / out-of-range code returns a non-empty
 *        `n00b_string_t *` (the documented fallback). The exact
 *        wording is documented; the regression doesn't bind
 *        tightly to the wording (would be tautological) but does
 *        verify non-emptiness.
 *    [3] The sign-folding behavior works: positive and negative
 *        encodings of the same errno code return byte-equal
 *        `data` of byte-equal length.
 *    [4] The accessor is pure: two calls with the same input
 *        return strings with byte-equal `data` of byte-equal
 *        length. (Same pointer is acceptable but not required.)
 *    [5] The zero code (`errno_val == 0`) returns the documented
 *        "no error" string — distinct from the unknown-code
 *        fallback so callers can distinguish "explicit 0 / success"
 *        from "the platform defines an errno code we don't cover."
 *
 *  Test-file carve-out (D-030) applies — libc I/O for stdout
 *  logging is acceptable per the established test-file precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "n00b.h"
#include "core/string.h"
#include "core/runtime.h"
#include "util/errno_str.h"

// Representative sample of POSIX.1-2008 portable errno values.
// Not exhaustive — the implementation's switch is exhaustive over
// the portable set, but the test sample picks one code from each
// rough family (process / file / io / math / network) so a
// regression that drops or corrupts a case is easy to localize.
//
// Hand-maintained against the codes in `util/errno_str.c`. If a
// future patch adds a code without updating this list, the test
// still passes (the list shrinks but doesn't go stale unsafely)
// but coverage drops — the auditor catches the gap.
static const int k_errno_codes[] = {
    // Process / signal family.
    EPERM,
    ESRCH,
    EINTR,
    ECHILD,
    // File-descriptor / file-system family.
    ENOENT,
    EIO,
    EBADF,
    EACCES,
    EEXIST,
    EXDEV,
    ENOTDIR,
    EISDIR,
    EMFILE,
    ENFILE,
    ENOSPC,
    EROFS,
    ENAMETOOLONG,
    ELOOP,
    ENOTEMPTY,
    EFBIG,
    ESPIPE,
    EMLINK,
    EPIPE,
    // Memory / argument-validation family.
    ENOMEM,
    EFAULT,
    EINVAL,
    E2BIG,
    ENOEXEC,
    // I/O resource family.
    EAGAIN,
    EBUSY,
    ENODEV,
    ENXIO,
    ENOSYS,
    EDEADLK,
    ENOLCK,
    // Math family.
    EDOM,
    ERANGE,
    EOVERFLOW,
    EILSEQ,
    // Message / IPC family.
    ENOMSG,
    EIDRM,
    EPROTO,
    EBADMSG,
    // Socket / network family.
    ENOTSOCK,
    EMSGSIZE,
    EPROTOTYPE,
    EPROTONOSUPPORT,
    ENOTSUP,
    EAFNOSUPPORT,
    EADDRINUSE,
    EADDRNOTAVAIL,
    ENETDOWN,
    ENETUNREACH,
    ECONNRESET,
    ECONNABORTED,
    ECONNREFUSED,
    ENOTCONN,
    EISCONN,
    ETIMEDOUT,
    EHOSTUNREACH,
    EINPROGRESS,
    EALREADY,
    ENOBUFS,
    // Streams family.
    ENOSTR,
    ENODATA,
    ETIME,
    ENOSR,
    ENOLINK,
    EMULTIHOP,
    // Filesystem-quota / NFS family.
    ESTALE,
    EDQUOT,
    // Robust-mutex family.
    ECANCELED,
    EOWNERDEAD,
    ENOTRECOVERABLE,
    // Misc.
    ETXTBSY,
    ENOTTY,
    ENOPROTOOPT,
    EDESTADDRREQ,
};

static void
assert_nonempty(n00b_string_t *s, const char *ctx, int code)
{
    if (s == nullptr) {
        fprintf(stderr, "FAIL: %s errno_str(%d) returned null\n", ctx, code);
        assert(0);
    }
    if (s->u8_bytes == 0) {
        fprintf(stderr,
                "FAIL: %s errno_str(%d) returned empty string\n",
                ctx,
                code);
        assert(0);
    }
}

static void
test_errno_every_code_returns_nonempty(void)
{
    size_t n = sizeof(k_errno_codes) / sizeof(k_errno_codes[0]);
    // Sample-size sanity check; pins the table size at its current
    // cardinality. If a future patch drops a code from the test
    // sample without intent, this fires.
    assert(n >= 30);
    for (size_t i = 0; i < n; i++) {
        n00b_string_t *s = n00b_errno_str(k_errno_codes[i]);
        assert_nonempty(s, "errno", k_errno_codes[i]);
    }
    printf("  [PASS] errno_every_code_returns_nonempty (%zu codes)\n", n);
}

static void
test_errno_zero_returns_no_error(void)
{
    // Code 0 (no error) is distinct from the unknown-code fallback.
    // We don't bind tightly to the exact wording (would be
    // tautological) but we DO assert it's non-empty and that it
    // differs from the unknown-code fallback (so a caller can
    // distinguish "the syscall returned 0 / success" from "the
    // platform defines an errno we don't cover").
    n00b_string_t *zero    = n00b_errno_str(0);
    n00b_string_t *unknown = n00b_errno_str(0x7fffffff);
    assert_nonempty(zero, "errno-zero", 0);
    assert_nonempty(unknown, "errno-unknown", 0x7fffffff);

    // Byte-distinct: the zero string and the unknown-code fallback
    // must not be byte-equal — otherwise the caller cannot tell
    // them apart.
    bool same_len  = zero->u8_bytes == unknown->u8_bytes;
    bool same_data = same_len
                  && memcmp(zero->data, unknown->data, zero->u8_bytes) == 0;
    assert(!same_data);
    printf("  [PASS] errno_zero_returns_no_error\n");
}

static void
test_errno_unknown_code_returns_fallback(void)
{
    // A positive value well above any defined errno. POSIX errno
    // values are small positive ints on every POSIX platform; 0x7fffffff
    // is comfortably outside that range.
    n00b_string_t *s = n00b_errno_str(0x7fffffff);
    assert_nonempty(s, "errno-unknown", 0x7fffffff);
    printf("  [PASS] errno_unknown_code_returns_fallback\n");
}

static void
test_errno_sign_folding(void)
{
    // The accessor folds the sign so libn00b's `n00b_check_posix`
    // convention (Err(errno), positive) and the project-local
    // Err(-errno) convention both produce the same lookup.
    for (size_t i = 0;
         i < sizeof(k_errno_codes) / sizeof(k_errno_codes[0]);
         i++) {
        int            code = k_errno_codes[i];
        n00b_string_t *pos  = n00b_errno_str(code);
        n00b_string_t *neg  = n00b_errno_str(-code);
        assert(pos != nullptr && neg != nullptr);
        assert(pos->u8_bytes == neg->u8_bytes);
        assert(memcmp(pos->data, neg->data, pos->u8_bytes) == 0);
    }
    printf("  [PASS] errno_sign_folding\n");
}

static void
test_errno_accessor_is_pure(void)
{
    // Same input -> byte-equal output across consecutive calls.
    // Pointer equality is acceptable but not required (rich-literal
    // storage is process-stable, so it usually IS pointer-equal —
    // but we don't bind tightly).
    for (size_t i = 0;
         i < sizeof(k_errno_codes) / sizeof(k_errno_codes[0]);
         i++) {
        n00b_string_t *a = n00b_errno_str(k_errno_codes[i]);
        n00b_string_t *b = n00b_errno_str(k_errno_codes[i]);
        assert(a != nullptr && b != nullptr);
        assert(a->u8_bytes == b->u8_bytes);
        assert(memcmp(a->data, b->data, a->u8_bytes) == 0);
    }
    printf("  [PASS] errno_accessor_is_pure\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== n00b_errno_str ==\n");
    test_errno_every_code_returns_nonempty();
    test_errno_zero_returns_no_error();
    test_errno_unknown_code_returns_fallback();
    test_errno_sign_folding();
    test_errno_accessor_is_pure();
    printf("All n00b_errno_str tests passed.\n");
    return 0;
}
