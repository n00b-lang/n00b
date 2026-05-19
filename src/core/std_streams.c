/*
 * std_streams.c — n00b_stdin() / n00b_stdout() / n00b_stderr().
 *
 * Thin accessors over the runtime's eagerly-managed FD owners for
 * process fds 0, 1, and 2.  The owners themselves are created in
 * `src/core/init.c` immediately after the conduit service spins up;
 * see the `n00b_conduit_fd_manage` call sequence there for the
 * lifecycle.
 *
 * These accessors are the public surface so callers can write
 * `n00b_stdin()` instead of fishing through `n00b_get_runtime()`.
 */

#include "core/std_streams.h"
#include "core/runtime.h"
#include "core/rt_access.h"

n00b_conduit_fd_owner_t *
n00b_stdin(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    return rt ? rt->stdin_owner : nullptr;
}

n00b_conduit_fd_owner_t *
n00b_stdout(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    return rt ? rt->stdout_owner : nullptr;
}

n00b_conduit_fd_owner_t *
n00b_stderr(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    return rt ? rt->stderr_owner : nullptr;
}
