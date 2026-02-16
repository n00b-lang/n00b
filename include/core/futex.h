#pragma once

// IWYU pragma: no_include <sys/errno.h>

#include <errno.h> // IWYU pragma: keep
#include <time.h>
#include "n00b.h"
#include "core/atomic.h"
#include "core/time.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#define n00b_mac_barrier()

extern void n00b_thread_exit(int);

static inline int
n00b_futex_wait_timespec(n00b_futex_t *futex, uint32_t v32, struct timespec *tptr)
{
    int err = syscall(SYS_futex, futex, FUTEX_WAIT_PRIVATE, v32, tptr, nullptr, 0);

    if (err == -1) {
        return errno;
    }

    return 0;
}

static inline int
n00b_futex_wake(n00b_futex_t *futex, bool all)
{
    uint32_t n = all ? INT_MAX : 1;

    return syscall(SYS_futex, futex, FUTEX_WAKE_PRIVATE, n, nullptr, nullptr, 0);
}

static inline bool
n00b_futex_should_continue(int err)
{
    return !err || err == EAGAIN;
}

#elif defined(__APPLE__)
extern int __ulock_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);

#define N00B_LOCK_COMPARE_AND_WAIT          1
#define N00B_LOCK_UNFAIR_LOCK               2
#define N00B_LOCK_COMPARE_AND_WAIT_SHARED   3
#define N00B_LOCK_UNFAIR_LOCK64_SHARED      4
#define N00B_LOCK_COMPARE_AND_WAIT64        5
#define N00B_LOCK_COMPARE_AND_WAIT64_SHARED 6
#define N00B_LOCK_WAKE_ALL                  0x00000100
#define N00B_LOCK_WAKE_THREAD               0x00000200
#define N00B_LOCK_WAKE_ALLOW_NON_OWNER      0x00000400

#define N00B_WAKE_ALL    (N00B_LOCK_COMPARE_AND_WAIT | N00B_LOCK_WAKE_ALL)
#define N00B_WAKE_THREAD (N00B_LOCK_COMPARE_AND_WAIT | N00B_LOCK_WAKE_THREAD)

#define n00b_mac_barrier() n00b_barrier()

static inline int
n00b_futex_wait_timespec(n00b_futex_t *futex, uint32_t v32, struct timespec *tout)
{
    return __ulock_wait2(N00B_LOCK_COMPARE_AND_WAIT,
                         &futex,
                         (uint64_t)v32,
                         tout ? tout->tv_nsec : 0,
                         0);
}

static inline int
n00b_futex_wake(n00b_futex_t *futex, bool all)
{
    return __ulock_wake(all ? N00B_WAKE_ALL : N00B_WAKE_THREAD, futex, 0ULL);
}

static inline bool
n00b_futex_should_continue(int err)
{
    return !err || err == -EINTR || err == -EFAULT;
}

#else
#error "Unsupported platform."
#endif

static inline void
n00b_futex_init(n00b_futex_t *futex)
{
    n00b_atomic_store(futex, 0);
}

#define n00b_thread_checkin()

static inline int
n00b_futex_wait(n00b_futex_t *futex, uint32_t v32, uint64_t nsec)
{
    struct timespec tout   = {.tv_sec = 0, .tv_nsec = nsec};
    int             result = n00b_futex_wait_timespec(futex, v32, &tout);

    n00b_thread_checkin();

    return result;
}

static inline bool
n00b_futex_timed_wait_for_value(volatile n00b_futex_t *futex, uint32_t v32, int64_t timeout)
{
    uint32_t cur       = n00b_atomic_load(futex);
    int64_t  start     = n00b_ns_timestamp();
    int64_t  remaining = timeout;
    int64_t  now;

    if (cur == v32) {
        return true;
    }
    while (true) {
        if (n00b_futex_wait((void *)futex, cur, remaining) == ETIMEDOUT) {
            return false; // Got timeout
        }
        cur = n00b_atomic_load(futex);
        // If some other thread is canceled, the check doesn't matter
        // anyway.
        // TODO: Check for program exiting.
        if (cur == v32) {
            return true;
        }
        now = n00b_ns_timestamp();
        remaining -= (now - start); // Subtract time elapsed.

        if (remaining < 0) {
            return false;
        }
        start = now;
    }
}

static inline void
n00b_futex_wait_for_value(volatile n00b_futex_t *futex, uint32_t v32)
{
    // Always timeout and requeue so that, if the futex transparently
    // moves during GC we are waiting in the right place.
    while (!n00b_futex_timed_wait_for_value(futex, v32, 10000))
        // nada.
        ;
}

static inline void
n00b_futex_wait_on_mask(n00b_futex_t *futex, uint32_t mask)
{
    uint32_t cur = n00b_atomic_load(futex);
    while (!(cur & mask)) {
        n00b_futex_wait(futex, cur, 0);
        cur = n00b_atomic_load(futex);
        // TODO: Check for program exiting.
    }
}
