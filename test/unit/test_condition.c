#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/condition.h"
#include "core/atomic.h"

// ============================================================================
// 1. Basic wait/notify
// ============================================================================

static n00b_condition_t basic_cv;
static _Atomic int      basic_ready;
static _Atomic int      basic_result;

static void *
basic_waiter(void *arg)
{
    (void)arg;
    n00b_thread_init();
    atomic_store(&basic_ready, 1);

    void *result = n00b_condition_wait(&basic_cv);
    atomic_store(&basic_result, (int)(uintptr_t)result);

    n00b_condition_unlock(&basic_cv);
    n00b_thread_destroy();
    return nullptr;
}

static void
wait_for_waiters(n00b_condition_t *cv, int32_t expected)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms

    for (int i = 0; i < 5000; i++) {
        int32_t waiters = n00b_atomic_load(&cv->wait_queue);
        waiters &= ~N00B_CV_NOTIFY_IN_PROGRESS;
        if (waiters >= expected) {
            return;
        }
        nanosleep(&ts, nullptr);
    }

    assert(!"timed out waiting for condition waiters");
}

static void
wait_for_flag(_Atomic int *flag)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; // 1ms

    for (int i = 0; i < 5000; i++) {
        if (atomic_load(flag) != 0) {
            return;
        }
        nanosleep(&ts, nullptr);
    }

    assert(!"timed out waiting for wake flag");
}

static void
test_basic_wait_notify(void)
{
    memset(&basic_cv, 0, sizeof(basic_cv));
    n00b_condition_init(&basic_cv);
    atomic_store(&basic_ready, 0);
    atomic_store(&basic_result, -1);

    pthread_t waiter;
    pthread_create(&waiter, nullptr, basic_waiter, nullptr);

    wait_for_waiters(&basic_cv, 1);

    n00b_condition_notify(&basic_cv, .value = (void *)42);
    n00b_condition_unlock(&basic_cv);

    pthread_join(waiter, nullptr);

    assert(atomic_load(&basic_result) == 42);

    printf("  [PASS] basic wait/notify\n");
}

// ============================================================================
// 2. Predicate-based selective wake
// ============================================================================

static n00b_condition_t pred_cv;
static _Atomic int      pred_ready_1;
static _Atomic int      pred_ready_2;
static _Atomic int      pred_woke_1;
static _Atomic int      pred_woke_2;

static void *
pred_waiter_1(void *arg)
{
    (void)arg;
    n00b_thread_init();
    atomic_store(&pred_ready_1, 1);

    void *result = n00b_condition_wait(&pred_cv, .predicate = 1);
    (void)result;
    atomic_store(&pred_woke_1, 1);

    n00b_condition_unlock(&pred_cv);
    n00b_thread_destroy();
    return nullptr;
}

static void *
pred_waiter_2(void *arg)
{
    (void)arg;
    n00b_thread_init();
    atomic_store(&pred_ready_2, 1);

    void *result = n00b_condition_wait(&pred_cv, .predicate = 2);
    (void)result;
    atomic_store(&pred_woke_2, 1);

    n00b_condition_unlock(&pred_cv);
    n00b_thread_destroy();
    return nullptr;
}

static void
test_predicate_wake(void)
{
    memset(&pred_cv, 0, sizeof(pred_cv));
    n00b_condition_init(&pred_cv);
    atomic_store(&pred_ready_1, 0);
    atomic_store(&pred_ready_2, 0);
    atomic_store(&pred_woke_1, 0);
    atomic_store(&pred_woke_2, 0);

    pthread_t w1, w2;
    pthread_create(&w1, nullptr, pred_waiter_1, nullptr);
    pthread_create(&w2, nullptr, pred_waiter_2, nullptr);

    wait_for_waiters(&pred_cv, 2);

    // Notify only predicate=1.
    n00b_condition_notify(&pred_cv, .predicate = 1, .all = true);
    n00b_condition_unlock(&pred_cv);

    // Waiter 1 should have woken, waiter 2 should still be asleep.
    wait_for_flag(&pred_woke_1);
    assert(atomic_load(&pred_woke_2) == 0);

    // Now wake waiter 2.
    n00b_condition_notify(&pred_cv, .predicate = 2, .all = true);
    n00b_condition_unlock(&pred_cv);

    pthread_join(w1, nullptr);
    pthread_join(w2, nullptr);

    assert(atomic_load(&pred_woke_2) == 1);

    printf("  [PASS] predicate-based selective wake\n");
}

// ============================================================================
// 3. Timeout
// ============================================================================

static void
test_timeout(void)
{
    n00b_condition_t cv = {0};
    n00b_condition_init(&cv);

    // Wait with a short timeout — nobody will notify.
    void *result = n00b_condition_wait(&cv, .timeout = 10000000); // 10ms
    n00b_condition_unlock(&cv);

    // Timeout returns ~0ULL.
    assert(result == (void *)~0ULL);

    printf("  [PASS] timeout\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_condition:\n");
    fflush(stdout);
    test_basic_wait_notify();
    fflush(stdout);
    test_predicate_wake();
    fflush(stdout);
    test_timeout();
    fflush(stdout);

    printf("All condition variable tests passed.\n");
    n00b_shutdown();
    return 0;
}
