/**
 * @file exit.h
 * @brief Process termination primitive.
 *
 * `n00b_abort()` terminates the process unconditionally.  Used by the
 * assert / require / panic family in `util/` and by any other code that
 * detects a non-recoverable condition.
 *
 * The current implementation sends SIGKILL to its own process and falls
 * back to libc `abort()`.  Future revisions may add coordinated thread
 * cleanup; callers should treat this as a black-box "the process ends
 * here" primitive and not depend on the exact mechanism.
 */
#pragma once

#include <stdbool.h>

[[noreturn]] extern void n00b_abort(void);

extern bool n00b_current_process_is_exiting(void);
