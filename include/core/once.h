/**
 * @file once.h
 * @brief Thread-safe one-time initialization for the base library.
 *
 * Provides two APIs for ensuring code runs exactly once:
 *
 * Split-phase (for guarding arbitrary initialization):
 * @code
 *   static base_once_t guard = BASE_ONCE_INIT;
 *   if (base_once_enter(&guard)) {
 *       // ... initialization ...
 *       base_once_complete(&guard);
 *   }
 * @endcode
 *
 * Callback-style (for simple init functions):
 * @code
 *   static base_once_t guard = BASE_ONCE_INIT;
 *   base_once(&guard, my_init_fn);
 * @endcode
 *
 * Both use a 3-state futex pattern:
 *   0 = not started, 1 = in progress, 2 = complete
 */
#pragma once

#include "futex.h"
#include <stdatomic.h>
#include <stdbool.h>

/** @brief State: initialization has not started. */
#define BASE_ONCE_NOT_STARTED 0

/** @brief State: initialization is in progress. */
#define BASE_ONCE_IN_PROGRESS 1

/** @brief State: initialization is complete. */
#define BASE_ONCE_COMPLETE    2

/** @brief Static initializer for @ref base_once_t. */
#define BASE_ONCE_INIT 0

/** @brief One-time initialization guard (a futex under the hood). */
typedef base_futex_t base_once_t;

/**
 * @brief Try to enter the once-guarded section.
 *
 * Returns @c true if the caller is the executor (must call
 * @ref base_once_complete when done). Returns @c false if
 * initialization has already completed.
 *
 * @param once  Pointer to the once guard.
 * @return @c true if the caller should perform initialization.
 */
static inline bool
base_once_enter(base_once_t *once)
{
    while (1) {
        uint32_t state = atomic_load_explicit(once, memory_order_acquire);

        if (state == BASE_ONCE_COMPLETE) {
            return false;
        }

        if (state == BASE_ONCE_NOT_STARTED) {
            if (atomic_compare_exchange_strong_explicit(
                    once,
                    &state,
                    BASE_ONCE_IN_PROGRESS,
                    memory_order_acq_rel,
                    memory_order_acquire)) {
                return true;
            }
            continue;
        }

        // state == BASE_ONCE_IN_PROGRESS — wait
        base_futex_wait(once, BASE_ONCE_IN_PROGRESS, 0);
    }
}

/**
 * @brief Mark the once-guarded section as complete.
 *
 * Must be called by the thread that received @c true from
 * @ref base_once_enter. Wakes all threads waiting on this guard.
 *
 * @param once  Pointer to the once guard.
 */
static inline void
base_once_complete(base_once_t *once)
{
    atomic_store_explicit(once, BASE_ONCE_COMPLETE, memory_order_release);
    base_futex_wake(once, true);
}

/**
 * @brief Callback-style one-time initialization.
 *
 * Calls @p init_fn exactly once. Concurrent callers block until
 * the initialization is complete.
 *
 * @param once     Pointer to the once guard.
 * @param init_fn  Function to call once.
 */
static inline void
base_once(base_once_t *once, void (*init_fn)(void))
{
    uint32_t state = atomic_load_explicit(once, memory_order_acquire);

    if (state == BASE_ONCE_COMPLETE) {
        return;
    }

    if (state == BASE_ONCE_NOT_STARTED) {
        uint32_t expected = BASE_ONCE_NOT_STARTED;
        if (atomic_compare_exchange_strong_explicit(
                once, &expected, BASE_ONCE_IN_PROGRESS,
                memory_order_acq_rel, memory_order_acquire)) {
            init_fn();
            base_once_complete(once);
            return;
        }
        state = expected;
    }

    // Wait for initialization to complete
    while (state != BASE_ONCE_COMPLETE) {
        base_futex_wait(once, state, 0);
        state = atomic_load_explicit(once, memory_order_acquire);
    }
}
