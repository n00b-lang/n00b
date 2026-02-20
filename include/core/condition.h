/**
 * @file condition.h
 * @brief Enhanced condition variable with epoch-based synchronization.
 *
 * Unlike POSIX condition variables, this implementation:
 * - Has a built-in mutex (no separate mutex to manage)
 * - Supports selective wake via predicates or state numbers
 * - Passes a result value from notifier to waiters
 * - Uses epoch-based synchronization to ensure notify completes before
 *   waiters resume
 *
 * @details The epoch protocol works as follows:
 *   1. Notifier sets `N00B_CV_NOTIFY_IN_PROGRESS` on `wait_queue`, wakes one waiter
 *   2. Each waiter checks predicate, decrements `waiters_to_process`
 *   3. Last waiter increments `notify_epoch` (epoch+1), wakes notifier
 *   4. Notifier wakes, increments `notify_epoch` again (epoch+2), wakes all waiters
 *   5. Waiters see epoch+2, re-acquire lock, return
 */
#pragma once

#include <stdatomic.h>
#include "n00b.h"
#include "core/lock_common.h"
#include "core/futex.h"
#include "core/macros.h"

/**
 * @brief Predicate callback for selective wake.
 *
 * @param actual_pred  The predicate value set by the notifier.
 * @param thread_pred  The predicate value the waiter is waiting for.
 * @param output       The output value from the notification.
 * @param cv_param     CV-wide user parameter (set via n00b_condition_set_callback).
 * @param thread_param Per-thread parameter (set via the wait call).
 * @return             true if this waiter should wake.
 */
typedef bool (*n00b_condition_predicate_fn)(uint64_t, uint64_t,
                                            void *, void *, void *);

struct n00b_condition_t {
    N00B_COMMON_LOCK_BASE;
    n00b_futex_t                mutex;
    _Atomic uint32_t            should_wake;
    void                       *ovalue;
    uint64_t                    pvalue;
    n00b_condition_predicate_fn predicate;
    void                       *cv_param;
    n00b_futex_t                notify_epoch;
    int32_t                     waiters_to_process;
    int32_t                     wakes_remaining;
    n00b_futex_t                wait_queue;
};

// n00b_condition_thread_state_t is defined in thread.h (to break
// the circular dependency: thread_record embeds it by value).

extern void _n00b_condition_init(n00b_condition_t *, char *);
extern int  _n00b_condition_lock(n00b_condition_t *, char *);
extern bool _n00b_condition_unlock(n00b_condition_t *, char *);
extern void
n00b_condition_set_callback(n00b_condition_t *, n00b_condition_predicate_fn, void *);

#define n00b_condition_init(x)   _n00b_condition_init((x), N00B_LOC_STRING())
#define n00b_condition_lock(x)   _n00b_condition_lock((x), N00B_LOC_STRING())
#define n00b_condition_unlock(x) _n00b_condition_unlock((x), N00B_LOC_STRING())

/**
 * @brief Wait on a condition variable.
 * @param cv  Condition variable to wait on.
 * @param loc Source location (auto-filled by macro).
 *
 * @kw predicate   Predicate value to match (default: N00B_CV_ANY = match anything).
 * @kw timeout     Timeout in nanoseconds (0 = no timeout).
 * @kw auto_unlock If true, automatically unlock the CV after waking.
 * @kw wake_param  Per-thread parameter passed to predicate callback.
 *
 * @return The output value from the notification, or `(void *)~0ULL` on timeout.
 */
extern void *
_n00b_condition_wait(n00b_condition_t *cv, char *loc) _kargs
{
    int64_t predicate   = ~0LL;
    int64_t timeout     = 0;
    bool    auto_unlock = false;
    void   *wake_param  = nullptr;
};

/**
 * @brief Notify waiters on a condition variable.
 * @param cv  Condition variable to notify.
 * @param loc Source location (auto-filled by macro).
 *
 * @kw predicate   Predicate value for selective wake.
 * @kw max         Maximum number of waiters to wake (default: 1).
 * @kw value       Output value to pass to woken waiters.
 * @kw all         If true, wake all matching waiters.
 * @kw auto_unlock If true, automatically unlock the CV after notify completes.
 *
 * @return Number of waiters that were in the queue at notify time.
 */
extern int32_t
_n00b_condition_notify(n00b_condition_t *cv, char *loc) _kargs
{
    int64_t predicate   = 0;
    int64_t max         = 1;
    void   *value       = nullptr;
    bool    all         = false;
    bool    auto_unlock = false;
};

#define n00b_condition_notify(cv, ...)                                                          \
    _n00b_condition_notify((cv), N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))
#define n00b_condition_wait(cv, ...)                                                            \
    _n00b_condition_wait((cv), N00B_LOC_STRING() __VA_OPT__(, __VA_ARGS__))

#define n00b_condition_notify_one(cv) n00b_condition_notify(cv)
#define n00b_condition_notify_all(cv) n00b_condition_notify(cv, .all = true)

#define N00B_CV_NOTIFY_IN_PROGRESS 0x40000000u
#define N00B_CV_ANY                (~0ULL)

/**
 * @brief Check whether any threads are currently waiting on a CV.
 * @param t Condition variable to check.
 * @return  true if there are waiters.
 */
static inline bool
n00b_condition_has_waiters(n00b_condition_t *t)
{
    return atomic_load(&t->wait_queue) != 0;
}
