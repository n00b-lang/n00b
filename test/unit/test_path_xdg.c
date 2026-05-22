/** @file test/unit/test_path_xdg.c — libn00b path XDG + variadic +
 *  canonical regression.
 *
 *  Always-runs unit coverage for the libn00b core path-module
 *  enhancements lifted pre-WP-005 P5 from a redundant n00b-attest
 *  wrapper. Exercises:
 *
 *    [1]  `n00b_xdg_config_home` — set / unset / empty matrix.
 *    [2]  `n00b_xdg_data_home`   — set / unset matrix.
 *    [3]  `n00b_xdg_cache_home`  — set / unset matrix.
 *    [4]  `n00b_xdg_state_home`  — set / unset matrix.
 *    [5]  `n00b_xdg_runtime_dir` — unset returns nullptr.
 *    [6]  `n00b_xdg_config_path` — composes
 *         $XDG_CONFIG_HOME/<app>/<sub>/<leaf>.
 *    [7]  `n00b_path_join_v`     — simple + absolute-re-root.
 *    [8]  `n00b_path_canonical`  — tilde + env-var + already-
 *         absolute composability.
 *
 *  Test-file conventions per D-030 (libc I/O for log output and
 *  <assert.h> for fail-fast asserts is intentional).
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "util/path.h"
#include "core/string.h"
#include "core/runtime.h"

static void
assert_string_eq(n00b_string_t *got, const char *expected, const char *tag)
{
    if (got == NULL) {
        fprintf(stderr, "  [FAIL %s] got nullptr, expected '%s'\n",
                tag, expected);
        assert(0);
    }
    size_t exp_len = strlen(expected);
    if ((size_t)got->u8_bytes != exp_len
        || memcmp(got->data, expected, exp_len) != 0) {
        fprintf(stderr,
                "  [FAIL %s] mismatch\n"
                "    got:      '%.*s' (%lld bytes)\n"
                "    expected: '%s' (%zu bytes)\n",
                tag,
                (int)got->u8_bytes, got->data, (long long)got->u8_bytes,
                expected, exp_len);
        assert(0);
    }
}

static void
test_xdg_homes_set(void)
{
    fprintf(stderr, "[xdg] *_home() with vars set\n");
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);
    setenv("XDG_DATA_HOME",   "/tmp/test-xdg-data",   1);
    setenv("XDG_CACHE_HOME",  "/tmp/test-xdg-cache",  1);
    setenv("XDG_STATE_HOME",  "/tmp/test-xdg-state",  1);

    assert_string_eq(n00b_xdg_config_home(), "/tmp/test-xdg-config",
                     "config_home set");
    assert_string_eq(n00b_xdg_data_home(),   "/tmp/test-xdg-data",
                     "data_home set");
    assert_string_eq(n00b_xdg_cache_home(),  "/tmp/test-xdg-cache",
                     "cache_home set");
    assert_string_eq(n00b_xdg_state_home(),  "/tmp/test-xdg-state",
                     "state_home set");
    fprintf(stderr, "  [PASS]\n");
}

static void
test_xdg_homes_unset(void)
{
    fprintf(stderr, "[xdg] *_home() with vars unset, HOME fallback\n");
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", "/tmp/test-xdg-home", 1);

    assert_string_eq(n00b_xdg_config_home(),
                     "/tmp/test-xdg-home/.config",
                     "config_home fallback");
    assert_string_eq(n00b_xdg_data_home(),
                     "/tmp/test-xdg-home/.local/share",
                     "data_home fallback");
    assert_string_eq(n00b_xdg_cache_home(),
                     "/tmp/test-xdg-home/.cache",
                     "cache_home fallback");
    assert_string_eq(n00b_xdg_state_home(),
                     "/tmp/test-xdg-home/.local/state",
                     "state_home fallback");
    fprintf(stderr, "  [PASS]\n");
}

static void
test_xdg_homes_empty(void)
{
    fprintf(stderr, "[xdg] *_home() with empty vars = treated as unset\n");
    setenv("XDG_CONFIG_HOME", "", 1);
    setenv("HOME", "/tmp/test-xdg-home", 1);

    // Empty env var = treated as unset per the freedesktop.org spec.
    assert_string_eq(n00b_xdg_config_home(),
                     "/tmp/test-xdg-home/.config",
                     "config_home empty == unset");
    unsetenv("XDG_CONFIG_HOME");
    fprintf(stderr, "  [PASS]\n");
}

static void
test_xdg_runtime_dir_unset(void)
{
    fprintf(stderr, "[xdg] runtime_dir unset returns nullptr\n");
    unsetenv("XDG_RUNTIME_DIR");
    n00b_string_t *r = n00b_xdg_runtime_dir();
    assert(r == NULL);
    fprintf(stderr, "  [PASS]\n");

    setenv("XDG_RUNTIME_DIR", "/tmp/test-xdg-runtime", 1);
    r = n00b_xdg_runtime_dir();
    assert_string_eq(r, "/tmp/test-xdg-runtime", "runtime_dir set");
    unsetenv("XDG_RUNTIME_DIR");

    // Empty also → nullptr per spec convention.
    setenv("XDG_RUNTIME_DIR", "", 1);
    r = n00b_xdg_runtime_dir();
    assert(r == NULL);
    unsetenv("XDG_RUNTIME_DIR");
    fprintf(stderr, "  [PASS] runtime_dir set / empty\n");
}

static void
test_xdg_config_path(void)
{
    fprintf(stderr, "[xdg] config_path(app, sub, leaf)\n");
    setenv("XDG_CONFIG_HOME", "/tmp/test-xdg-config", 1);

    n00b_string_t *p = n00b_xdg_config_path(r"app", r"sub", r"leaf.txt");
    assert_string_eq(p, "/tmp/test-xdg-config/app/sub/leaf.txt",
                     "config_path app/sub/leaf");

    // Single-app, no tail.
    p = n00b_xdg_config_path(r"n00b-attest", r"registries.json");
    assert_string_eq(p, "/tmp/test-xdg-config/n00b-attest/registries.json",
                     "config_path registries.json");
    unsetenv("XDG_CONFIG_HOME");
    fprintf(stderr, "  [PASS]\n");
}

static void
test_path_join_v(void)
{
    fprintf(stderr, "[xdg] path_join_v\n");

    n00b_string_t *p = n00b_path_join_v(r"a", r"b", r"c");
    assert_string_eq(p, "a/b/c", "simple join");

    // Absolute piece re-roots the join (matches n00b_path_simple_join).
    p = n00b_path_join_v(r"a", r"/abs", r"leaf");
    assert_string_eq(p, "/abs/leaf", "absolute re-root");

    // Single piece, no variadic tail.
    p = n00b_path_join_v(r"only");
    assert_string_eq(p, "only", "single piece");

    fprintf(stderr, "  [PASS]\n");
}

static void
test_path_canonical(void)
{
    fprintf(stderr, "[xdg] path_canonical\n");

    setenv("HOME", "/tmp/test-xdg-home", 1);

    // Tilde expansion.
    n00b_string_t *p = n00b_path_canonical(r"~/foo");
    assert_string_eq(p, "/tmp/test-xdg-home/foo", "tilde");

    // Env-var expansion (HOME → tilde fallback path).
    setenv("CANONICAL_TEST_DIR", "/tmp/canon-test", 1);
    p = n00b_path_canonical(r"$CANONICAL_TEST_DIR/bar");
    assert_string_eq(p, "/tmp/canon-test/bar", "$VAR");

    // Braced env-var expansion.
    p = n00b_path_canonical(r"${CANONICAL_TEST_DIR}/baz");
    assert_string_eq(p, "/tmp/canon-test/baz", "${VAR}");
    unsetenv("CANONICAL_TEST_DIR");

    // Already absolute, with all magic off.
    p = n00b_path_canonical(r"/already/abs",
                            .expand_env_vars = false,
                            .expand_tilde    = false,
                            .make_absolute   = false);
    assert_string_eq(p, "/already/abs", "all-off passthrough");

    // make_absolute=true rebases relative paths under cwd.
    // We don't pin cwd here (test-runner-dependent), so just assert
    // the result starts with '/'.
    p = n00b_path_canonical(r"relative/leaf",
                            .expand_env_vars = false,
                            .expand_tilde    = false,
                            .make_absolute   = true);
    assert(p != NULL && p->u8_bytes > 0 && p->data[0] == '/');

    fprintf(stderr, "  [PASS]\n");
}

int
main(int argc, char **argv)
{
    n00b_init_simple(argc, argv);

    test_xdg_homes_set();
    test_xdg_homes_unset();
    test_xdg_homes_empty();
    test_xdg_runtime_dir_unset();
    test_xdg_config_path();
    test_path_join_v();
    test_path_canonical();

    fprintf(stderr, "All path_xdg regression tests passed.\n");
    return 0;
}
