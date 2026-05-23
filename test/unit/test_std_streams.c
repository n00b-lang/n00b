/*
 * test_std_streams.c — Regression tests for n00b_stdin() /
 * n00b_stdout() / n00b_stderr() public accessors.
 *
 * The runtime manages fds 0, 1, and 2 eagerly during `n00b_init`
 * (see `src/core/init.c`).  These tests verify:
 *
 *   1. Each accessor returns a non-null `n00b_conduit_fd_owner_t *`
 *      after `n00b_init`.
 *   2. The returned pointers match the runtime's underlying fields
 *      (the accessors are field-wrappers, not factories).
 *   3. The managed FD numbers match the expected POSIX file
 *      descriptors (0, 1, 2).
 *   4. Multiple calls to each accessor return the same pointer
 *      (idempotency / stable identity).
 *   5. The three accessors return distinct owners (stdin != stdout
 *      != stderr).
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/std_streams.h"
#include "conduit/fd_managed.h"

// ============================================================================
// 1. Each accessor returns non-null.
// ============================================================================

static void
test_accessors_nonnull(void)
{
    n00b_conduit_fd_owner_t *in_owner  = n00b_stdin();
    n00b_conduit_fd_owner_t *out_owner = n00b_stdout();
    n00b_conduit_fd_owner_t *err_owner = n00b_stderr();

    assert(in_owner  != nullptr);
    assert(out_owner != nullptr);
    assert(err_owner != nullptr);

    printf("  [PASS] std_streams: accessors non-null\n");
}

// ============================================================================
// 2. Accessor pointers match the runtime fields.
// ============================================================================

static void
test_accessors_match_runtime(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    assert(rt != nullptr);

    assert(n00b_stdin()  == rt->stdin_owner);
    assert(n00b_stdout() == rt->stdout_owner);
    assert(n00b_stderr() == rt->stderr_owner);

    printf("  [PASS] std_streams: accessors match runtime fields\n");
}

// ============================================================================
// 3. The managed fd numbers are 0, 1, 2.
// ============================================================================

static void
test_accessor_fds(void)
{
    assert(n00b_stdin()->fd  == 0);
    assert(n00b_stdout()->fd == 1);
    assert(n00b_stderr()->fd == 2);

    printf("  [PASS] std_streams: managed fds = 0/1/2\n");
}

// ============================================================================
// 4. Multiple calls return the same pointer (stable identity).
// ============================================================================

static void
test_accessors_stable(void)
{
    n00b_conduit_fd_owner_t *in_a  = n00b_stdin();
    n00b_conduit_fd_owner_t *in_b  = n00b_stdin();
    n00b_conduit_fd_owner_t *out_a = n00b_stdout();
    n00b_conduit_fd_owner_t *out_b = n00b_stdout();
    n00b_conduit_fd_owner_t *err_a = n00b_stderr();
    n00b_conduit_fd_owner_t *err_b = n00b_stderr();

    assert(in_a  == in_b);
    assert(out_a == out_b);
    assert(err_a == err_b);

    printf("  [PASS] std_streams: accessors are stable across calls\n");
}

// ============================================================================
// 5. The three accessors return distinct owners.
// ============================================================================

static void
test_accessors_distinct(void)
{
    n00b_conduit_fd_owner_t *in_owner  = n00b_stdin();
    n00b_conduit_fd_owner_t *out_owner = n00b_stdout();
    n00b_conduit_fd_owner_t *err_owner = n00b_stderr();

    assert(in_owner  != out_owner);
    assert(in_owner  != err_owner);
    assert(out_owner != err_owner);

    printf("  [PASS] std_streams: stdin/stdout/stderr are distinct owners\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running std_streams tests...\n");

    test_accessors_nonnull();
    test_accessors_match_runtime();
    test_accessor_fds();
    test_accessors_stable();
    test_accessors_distinct();

    printf("All std_streams tests passed.\n");
    n00b_shutdown();
    return 0;
}
