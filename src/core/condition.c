/**
 * Enhanced condition variable with epoch-based synchronization.
 *
 * The CV has a built-in mutex and supports selective wake via predicate
 * callbacks or state numbers.  The epoch protocol ensures that the
 * notifier's operation completes before any waiters resume.
 */

#define N00B_USE_INTERNAL_API

#include <setjmp.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/condition.h"
#include "core/mutex.h"
#include "core/stw.h"
#include "core/atomic.h"
#include "core/memory_info.h"
#include "core/gc.h"

void
_n00b_condition_init(n00b_condition_t *cv, char *loc)
{
    n00b_lock_init_accounting((void *)cv, N00B_NLT_CV, loc);

    n00b_futex_init(&cv->mutex);
    n00b_futex_init(&cv->wait_queue);
    n00b_futex_init(&cv->notify_epoch);
    cv->ovalue             = nullptr;
    cv->predicate          = nullptr;
    cv->cv_param           = nullptr;
    cv->waiters_to_process = 0;
    cv->wakes_remaining    = 0;

    if (!n00b_in_heap(cv)) {
        _n00b_gc_register_root(&cv->cv_param, 2);
    }
}

void
n00b_condition_set_callback(n00b_condition_t            *cv,
                            n00b_condition_predicate_fn  fn,
                            void                        *param)
{
    n00b_condition_lock(cv);
    cv->predicate = fn;
    cv->cv_param  = param;

    if (!n00b_in_heap(cv)) {
        _n00b_gc_register_root(&cv->cv_param, 1);
    }
    n00b_condition_unlock(cv);
}

int
_n00b_condition_lock(n00b_condition_t *cv, char *loc)
{
    if (n00b_lock_already_owner((void *)cv)) {
        return 1;
    }

    _n00b_mutex_lock((void *)cv, loc);

    return 1;
}

bool
_n00b_condition_unlock(n00b_condition_t *cv, char *loc)
{
    if (!n00b_lock_already_owner((void *)cv)) {
        return false;
    }

    _n00b_mutex_unlock((void *)cv, loc);

    return true;
}

static void *
base_wait(n00b_condition_t *cv,
          n00b_thread_t    *thread,
          int64_t           timeout,
          bool              wake_unlocked,
          char             *loc)
{
    void   *result;
    bool    want_to_wake;
    bool    waking = false;
    bool    timed;
    int64_t last_ts;
    int64_t cur_ts;

    n00b_thread_record_t *rec = thread->record;

    if (timeout <= 0) {
        timeout = 0;
        timed   = false;
    }
    else {
        timed   = true;
        last_ts = n00b_ns_timestamp();
    }

    _n00b_condition_lock(cv, loc);

    if (!rec->cv_info.current_cv || rec->cv_info.current_cv != cv) {
        memset(&rec->cv_info, 0, sizeof(n00b_condition_thread_state_t));
        rec->cv_info.current_cv     = cv;
        rec->cv_info.wait_predicate = N00B_CV_ANY;
    }

    rec->lock_wait_loc = loc;

    int32_t waiters = n00b_atomic_add(&cv->wait_queue, 1);

    n00b_stw_suspend_ctx stw_ctx;
    uint32_t             epoch;

    n00b_thread_suspend(stw_ctx);
    n00b_register_lock_wait(thread, cv, loc);

    _n00b_condition_unlock(cv, loc);

    n00b_futex_wait(&cv->wait_queue, waiters + 1, timeout);
    waiters = n00b_atomic_load(&cv->wait_queue);

    do {
        epoch = n00b_atomic_load(&cv->notify_epoch);
        while (!(waiters & N00B_CV_NOTIFY_IN_PROGRESS)) {
            if (timed) {
                cur_ts = n00b_ns_timestamp();
                timeout -= (cur_ts - last_ts);
                last_ts = cur_ts;

                if (timeout < 0) {
                    n00b_atomic_add(&cv->wait_queue, -1);
                    return (void *)~0ULL;
                }
            }
            n00b_futex_wait(&cv->wait_queue, waiters, timeout);
            waiters = n00b_atomic_load(&cv->wait_queue);
        }

        if (cv->predicate) {
            want_to_wake = (*cv->predicate)(cv->pvalue,
                                            rec->cv_info.wait_predicate,
                                            cv->ovalue,
                                            cv->cv_param,
                                            rec->cv_info.thread_param);
        }
        else {
            want_to_wake = (cv->pvalue == rec->cv_info.wait_predicate
                            || rec->cv_info.wait_predicate == N00B_CV_ANY);
        }

        if (want_to_wake) {
            cv->wakes_remaining--;
            waking = true;
            result = cv->ovalue;
        }

        int32_t waiter_ct = --cv->waiters_to_process;

        if (waiter_ct) {
            n00b_futex_wake(&cv->wait_queue, false);
        }
        else {
            n00b_atomic_add(&cv->notify_epoch, 1);
            n00b_futex_wake(&cv->notify_epoch, false);
        }

        n00b_barrier();

        if (!waking) {
            // A waiter that does not match the current notify round must wait
            // for the notifier to finish before re-entering the wait loop.
            // Otherwise it can consume `waiters_to_process` multiple times.
            n00b_futex_wait_for_value(&cv->notify_epoch, epoch + 2);
            waiters = n00b_atomic_load(&cv->wait_queue);
        }
    } while (!waking);

    // Wait for the notifier to complete (epoch+2).
    n00b_futex_wait_for_value(&cv->notify_epoch, epoch + 2);
    _n00b_condition_lock(cv, loc);
    n00b_wait_done(thread);
    n00b_thread_resume(stw_ctx);

    int nw = n00b_atomic_add(&cv->wait_queue, -1);
    (void)nw;

    if (wake_unlocked) {
        _n00b_condition_unlock(cv, loc);
    }

    return result;
}

static int32_t
_internal_cv_notify(n00b_condition_t *cv,
                    int64_t           pvalue,
                    void             *ovalue,
                    int32_t           max,
                    bool              unlock,
                    char             *loc)
{
    n00b_thread_t       *thread = n00b_thread_self();
    n00b_stw_suspend_ctx stw_ctx;
    int32_t              result;

    _n00b_condition_lock(cv, loc);
    cv->pvalue             = pvalue;
    cv->ovalue             = ovalue;
    cv->wakes_remaining    = max;
    result                 = n00b_atomic_load(&cv->wait_queue);
    cv->waiters_to_process = result & ~N00B_STW;

    if (!cv->waiters_to_process) {
        if (unlock) {
            _n00b_condition_unlock(cv, loc);
        }
        return 0;
    }

    n00b_atomic_or(&cv->wait_queue, N00B_CV_NOTIFY_IN_PROGRESS);

    uint32_t epoch = n00b_atomic_load(&cv->notify_epoch);

    // Wake all queued waiters for this notify round so we cannot lose progress
    // if one thread has not yet entered the kernel wait when notify starts.
    n00b_futex_wake(&cv->wait_queue, true);
    _n00b_condition_unlock(cv, loc);

    n00b_thread_suspend(stw_ctx);
    n00b_register_lock_wait(thread, cv, loc);

    n00b_futex_wait_for_value(&cv->notify_epoch, epoch + 1);
    _n00b_condition_lock(cv, loc);

    n00b_wait_done(thread);

    n00b_barrier();
    n00b_atomic_and(&cv->wait_queue, ~N00B_CV_NOTIFY_IN_PROGRESS);
    n00b_atomic_add(&cv->notify_epoch, 1);
    n00b_futex_wake(&cv->notify_epoch, true);

    n00b_thread_resume(stw_ctx);

    if (unlock) {
        _n00b_condition_unlock(cv, loc);
    }
    n00b_mac_barrier();

    return result;
}

void *
_n00b_condition_wait(n00b_condition_t *cv, char *loc) _kargs
{
    int64_t predicate   = ~0LL;
    int64_t timeout     = 0;
    bool    auto_unlock = false;
    void   *wake_param  = nullptr;
}
{
    void                 *result;
    n00b_thread_t        *thread = n00b_thread_self();
    n00b_thread_record_t *rec    = thread->record;

    rec->cv_info.current_cv     = cv;
    rec->cv_info.wait_predicate = predicate;
    rec->cv_info.thread_param   = wake_param;
    rec->cv_info.wait_loc       = loc;

    result = base_wait(cv, thread, timeout, auto_unlock, loc);

    return result;
}

int32_t
_n00b_condition_notify(n00b_condition_t *cv, char *loc) _kargs
{
    int64_t predicate   = 0;
    int64_t max         = 1;
    void   *value       = nullptr;
    bool    all         = false;
    bool    auto_unlock = false;
}
{
    if (all || max <= 0) {
        max = 0x7fffffff;
    }

    return _internal_cv_notify(cv, predicate, value, (int32_t)max, auto_unlock, loc);
}
