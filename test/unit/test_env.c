/* Round-trip + edge-case tests for the n00b_init-backed env cache
 * and the n00b_getenv / n00b_putenv accessors. */
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/env.h"
#include "core/runtime.h"

extern char **environ;

static void
test_setup_envp_len(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    /* setup_envp should have walked envp as char ** and recorded a
     * non-zero len on any host with at least one env entry.  This is
     * the regression bait for the long-standing bytewise-walk bug. */
    int posix_count = 0;
    char **p;
    for (p = environ; *p; p++) {
        posix_count++;
    }
    assert((int)rt->envp.len == posix_count);
    printf("  [PASS] setup_envp_len (entries=%d)\n", posix_count);
}

static void
test_getenv_known(void)
{
    /* "PATH" exists on every reasonable test host.  Skip if not. */
    const char *libc_value = getenv("PATH");
    if (!libc_value) {
        printf("  [SKIP] getenv_known (no PATH in env)\n");
        return;
    }

    n00b_string_t *value = n00b_getenv(n00b_string_from_cstr("PATH"));
    assert(value != nullptr);
    assert(strcmp(value->data, libc_value) == 0);
    printf("  [PASS] getenv_known\n");
}

static void
test_getenv_missing(void)
{
    assert(n00b_getenv(n00b_string_from_cstr(
                          "N00B_TEST_ENV_DEFINITELY_NOT_SET_12345"))
           == nullptr);
    /* nullptr and empty inputs short-circuit to nullptr. */
    assert(n00b_getenv(nullptr) == nullptr);
    assert(n00b_getenv(n00b_string_from_cstr("")) == nullptr);
    printf("  [PASS] getenv_missing\n");
}

/* Pick a name unlikely to be set in the parent env so the "grows by
 * one" assertion is meaningful without a meson-side unsetenv. */
#define TEST_NAME "N00B_TEST_PUTENV_FRESH_BAB1B0"

static void
test_putenv_grows_and_libc_sees_it(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    /* Sanity: the chosen name must not already be set. */
    assert(getenv(TEST_NAME) == nullptr);
    size_t before = rt->envp.len;

    assert(n00b_putenv(n00b_string_from_cstr(TEST_NAME),
                       n00b_string_from_cstr("hello"))
           == true);
    assert(rt->envp.len == before + 1);

    n00b_string_t *got = n00b_getenv(n00b_string_from_cstr(TEST_NAME));
    assert(got != nullptr);
    assert(strcmp(got->data, "hello") == 0);

    /* libc should observe the same value through environ/getenv. */
    const char *via_libc = getenv(TEST_NAME);
    assert(via_libc != nullptr);
    assert(strcmp(via_libc, "hello") == 0);

    /* environ should still be null-terminated after the grow. */
    int seen = 0;
    char **p;
    for (p = environ; *p; p++) {
        seen++;
    }
    assert((size_t)seen == rt->envp.len);
    printf("  [PASS] putenv_grows_and_libc_sees_it\n");
}

static void
test_putenv_replaces_in_place(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    size_t          before = rt->envp.len;

    assert(n00b_putenv(n00b_string_from_cstr(TEST_NAME),
                       n00b_string_from_cstr("world"))
           == true);
    assert(rt->envp.len == before); /* replace, not grow */

    n00b_string_t *got = n00b_getenv(n00b_string_from_cstr(TEST_NAME));
    assert(got != nullptr);
    assert(strcmp(got->data, "world") == 0);
    printf("  [PASS] putenv_replaces_in_place\n");
}

static void
test_putenv_rejects_invalid(void)
{
    /* null name */
    assert(n00b_putenv(nullptr, n00b_string_from_cstr("x")) == false);
    /* empty name */
    assert(n00b_putenv(n00b_string_from_cstr(""),
                       n00b_string_from_cstr("x"))
           == false);
    /* name with '=' */
    assert(n00b_putenv(n00b_string_from_cstr("BAD=NAME"),
                       n00b_string_from_cstr("x"))
           == false);
    /* null value */
    assert(n00b_putenv(n00b_string_from_cstr("N00B_TEST_VALID_NAME"),
                       nullptr)
           == false);
    printf("  [PASS] putenv_rejects_invalid\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running env tests...\n");
    test_setup_envp_len();
    test_getenv_known();
    test_getenv_missing();
    test_putenv_grows_and_libc_sees_it();
    test_putenv_replaces_in_place();
    test_putenv_rejects_invalid();
    printf("All env tests passed.\n");

    n00b_shutdown();
    return 0;
}
