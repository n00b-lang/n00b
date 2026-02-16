/**
 * @file futex.h
 * @brief Platform-specific futex support for Linux and macOS.
 *
 * Provides portable futex_wait and futex_wake operations.
 */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline int
futex_wait(uint32_t *futex, uint32_t val)
{
    return syscall(SYS_futex, futex, FUTEX_WAIT_PRIVATE, val, nullptr, nullptr, 0);
}

static inline int
futex_wake(uint32_t *futex, bool all)
{
    return syscall(SYS_futex, futex, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, nullptr, nullptr, 0);
}

#elif defined(__APPLE__)
extern int __ulock_wait2(uint32_t, void *, uint64_t, uint64_t, uint64_t);
extern int __ulock_wake(uint32_t, void *, uint64_t);

#define ULOCK_COMPARE_AND_WAIT 1
#define ULOCK_WAKE_ALL         0x00000100
#define ULOCK_WAKE_THREAD      0x00000200

static inline int
futex_wait(uint32_t *futex, uint32_t val)
{
    return __ulock_wait2(ULOCK_COMPARE_AND_WAIT, futex, (uint64_t)val, 0, 0);
}

static inline int
futex_wake(uint32_t *futex, bool all)
{
    uint32_t op = ULOCK_COMPARE_AND_WAIT | (all ? ULOCK_WAKE_ALL : ULOCK_WAKE_THREAD);
    return __ulock_wake(op, futex, 0);
}

#else
#error "Unsupported platform - futex not available"
#endif

/**
 * @brief Wait until atomic value equals expected value.
 * @param futex Atomic variable to wait on
 * @param val Expected value
 */
static inline void
futex_wait_for_value(_Atomic uint32_t *futex, uint32_t val)
{
    uint32_t cur = atomic_load_explicit(futex, memory_order_acquire);
    while (cur != val) {
        futex_wait((uint32_t *)futex, cur);
        cur = atomic_load_explicit(futex, memory_order_acquire);
    }
}
