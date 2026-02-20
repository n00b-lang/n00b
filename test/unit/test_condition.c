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

    void *result = n00b_condition_wait(&basic_cv);
    atomic_store(&basic_result, (int)(uintptr_t)result);

    n00b_condition_unlock(&basic_cv);
    n00b_thread_destroy();
    return nullptr;
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

    // Give the waiter time to enter wait().
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000}; // 50ms
    nanosleep(&ts, nullptr);

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
static _Atomic int      pred_woke_1;
static _Atomic int      pred_woke_2;

static void *
pred_waiter_1(void *arg)
{
    (void)arg;
    n00b_thread_init();

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
    atomic_store(&pred_woke_1, 0);
    atomic_store(&pred_woke_2, 0);

    pthread_t w1, w2;
    pthread_create(&w1, nullptr, pred_waiter_1, nullptr);
    pthread_create(&w2, nullptr, pred_waiter_2, nullptr);

    struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&ts, nullptr);

    // Notify only predicate=1.
    n00b_condition_notify(&pred_cv, .predicate = 1, .all = true);
    n00b_condition_unlock(&pred_cv);

    nanosleep(&ts, nullptr);

    // Waiter 1 should have woken, waiter 2 should still be asleep.
    assert(atomic_load(&pred_woke_1) == 1);

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
    test_basic_wait_notify();
    test_predicate_wake();
    test_timeout();

    printf("All condition variable tests passed.\n");
    return 0;
}
