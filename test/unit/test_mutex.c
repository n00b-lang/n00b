#include <stdio.h>
#include <assert.h>
#include <pthread.h>

#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/thread.h"
#include "core/mutex.h"
#include "core/atomic.h"

// ============================================================================
// 1. Basic lock/unlock
// ============================================================================

static void
test_basic_lock_unlock(void)
{
    n00b_mutex_t mtx = {0};
    n00b_mutex_init(&mtx);

    n00b_mutex_lock(&mtx);
    n00b_mutex_unlock(&mtx);

    printf("  [PASS] basic lock/unlock\n");
}

// ============================================================================
// 2. Recursive (nested) locking
// ============================================================================

static void
test_recursive_locking(void)
{
    n00b_mutex_t mtx = {0};
    n00b_mutex_init(&mtx);

    n00b_mutex_lock(&mtx);
    n00b_mutex_lock(&mtx);   // Should succeed (recursive).
    n00b_mutex_unlock(&mtx); // Still locked (nesting=1).
    n00b_mutex_unlock(&mtx); // Fully unlocked.

    // Lock again to verify it's truly free.
    n00b_mutex_lock(&mtx);
    n00b_mutex_unlock(&mtx);

    printf("  [PASS] recursive locking\n");
}

// ============================================================================
// 3. Contention between two threads
// ============================================================================

static n00b_mutex_t contention_mtx;
static _Atomic int  contention_counter;

static void *
contention_worker(void *arg)
{
    (void)arg;
    n00b_thread_init();

    for (int i = 0; i < 10000; i++) {
        n00b_mutex_lock(&contention_mtx);
        n00b_atomic_add(&contention_counter, 1);
        n00b_mutex_unlock(&contention_mtx);
    }

    n00b_thread_destroy();
    return nullptr;
}

static void
test_contention(void)
{
    memset(&contention_mtx, 0, sizeof(contention_mtx));
    n00b_mutex_init(&contention_mtx);
    atomic_store(&contention_counter, 0);

    pthread_t t1, t2;
    pthread_create(&t1, nullptr, contention_worker, nullptr);
    pthread_create(&t2, nullptr, contention_worker, nullptr);
    pthread_join(t1, nullptr);
    pthread_join(t2, nullptr);

    assert(atomic_load(&contention_counter) == 20000);

    printf("  [PASS] contention (2 threads, counter=%d)\n",
           atomic_load(&contention_counter));
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_mutex:\n");
    test_basic_lock_unlock();
    test_recursive_locking();
    test_contention();

    printf("All mutex tests passed.\n");
    n00b_shutdown();
    return 0;
}
