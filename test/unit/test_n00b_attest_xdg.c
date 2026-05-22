/** @file test/unit/test_n00b_attest_xdg.c — XDG path resolver regression.
 *
 *  WP-005 mid-stream cleanup lift regression test for the shared
 *  XDG path resolver in include/attest/n00b_attest_xdg.h.
 *
 *  Assertions:
 *    [1] With XDG_CONFIG_HOME=/tmp/test-xdg-config:
 *        `n00b_attest_xdg_path(r"foo")` →
 *        `/tmp/test-xdg-config/n00b-attest/foo`.
 *    [2] With XDG_CONFIG_HOME unset and HOME=/tmp/test-xdg-home:
 *        `n00b_attest_xdg_path(r"foo")` →
 *        `/tmp/test-xdg-home/.config/n00b-attest/foo`.
 *    [3] Composite suffix: `signing-identities/myid.cert.pem`
 *        roundtrips through the helper unchanged.
 *    [4] nullptr / empty suffix returns nullptr.
 *
 *  Test-file conventions per D-030 (libc I/O for log output and
 *  `<assert.h>` for fail-fast asserts is intentional).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include <attest/n00b_attest_xdg.h>

static void
assert_string_eq(n00b_string_t *got, const char *expected)
{
    assert(got != NULL);
    size_t exp_len = strlen(expected);
    if ((size_t)got->u8_bytes != exp_len
        || memcmp(got->data, expected, exp_len) != 0) {
        fprintf(stderr,
                "  [FAIL] path mismatch\n"
                "    got:      %.*s (%lld bytes)\n"
                "    expected: %s (%zu bytes)\n",
                (int)got->u8_bytes, got->data, (long long)got->u8_bytes,
                expected, exp_len);
        assert(0);
    }
}

static void
test_xdg_config_home_set(void)
{
    fprintf(stderr, "[xdg] XDG_CONFIG_HOME set\n");
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);
    n00b_string_t *suffix = n00b_string_from_cstr("foo");
    n00b_string_t *path   = n00b_attest_xdg_path(suffix);
    assert_string_eq(path, "/tmp/test-xdg-config/n00b-attest/foo");
    fprintf(stderr, "  [PASS] %.*s\n",
            (int)path->u8_bytes, path->data);
    unsetenv("XDG_CONFIG_HOME");
}

static void
test_xdg_config_home_unset(void)
{
    fprintf(stderr, "[xdg] XDG_CONFIG_HOME unset, HOME set\n");
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/tmp/test-xdg-home", 1);
    n00b_string_t *suffix = n00b_string_from_cstr("foo");
    n00b_string_t *path   = n00b_attest_xdg_path(suffix);
    assert_string_eq(path, "/tmp/test-xdg-home/.config/n00b-attest/foo");
    fprintf(stderr, "  [PASS] %.*s\n",
            (int)path->u8_bytes, path->data);
}

static void
test_composite_suffix(void)
{
    fprintf(stderr, "[xdg] composite suffix (signing-identities/...)\n");
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);
    n00b_string_t *suffix = n00b_string_from_cstr(
        "signing-identities/myid.cert.pem");
    n00b_string_t *path   = n00b_attest_xdg_path(suffix);
    assert_string_eq(
        path,
        "/tmp/test-xdg-config/n00b-attest/signing-identities/myid.cert.pem");
    fprintf(stderr, "  [PASS] %.*s\n",
            (int)path->u8_bytes, path->data);
    unsetenv("XDG_CONFIG_HOME");
}

static void
test_null_and_empty_suffix(void)
{
    fprintf(stderr, "[xdg] nullptr / empty suffix returns nullptr\n");
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);
    n00b_string_t *path = n00b_attest_xdg_path(NULL);
    assert(path == NULL);
    n00b_string_t *empty = n00b_string_from_cstr("");
    path = n00b_attest_xdg_path(empty);
    assert(path == NULL);
    fprintf(stderr, "  [PASS] nullptr / empty rejected\n");
    unsetenv("XDG_CONFIG_HOME");
}

int
main(int argc, char **argv)
{
    n00b_init_simple(argc, argv);

    test_xdg_config_home_set();
    test_xdg_config_home_unset();
    test_composite_suffix();
    test_null_and_empty_suffix();

    fprintf(stderr, "All n00b_attest_xdg regression tests passed.\n");
    return 0;
}
