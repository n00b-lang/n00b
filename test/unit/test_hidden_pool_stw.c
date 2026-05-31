/*
 * test_hidden_pool_stw.c
 *
 * Repro for the SIGBUS / abort the wax raw_gateway has been hitting
 * under sustained burst load: n00b_visit_possible_pointer faults while
 * the GC mark scans an mmap-tree entry whose backing page has been
 * munmap'd by a concurrent pool_free.
 *
 * Hypothesis under test: foreign (non-n00b-spawned) pthreads that
 * call n00b_thread_init on entry but otherwise skip n00b_thread_checkin
 * can run pool_alloc / pool_free concurrently with a GC mark phase.
 * The pool registers / unregisters pages in the global mmap tree
 * (subject to the pool's own locking), but the GC mark walks the tree
 * without re-acquiring the per-mutator safepoint, and the freed
 * mmap_info_t / unmapped page lands underneath the scanner.
 *
 * Repro shape:
 *   - 4 n00b-spawned alloc/free threads on a hidden+metadata pool
 *   - 4 raw pthreads doing the same (mimicking XPC / libdispatch
 *     workers that hit our public surface from outside n00b's
 *     thread system). Each calls n00b_thread_init() on entry; no
 *     periodic n00b_thread_checkin().
 *   - main thread loops n00b_collect(default_arena) at ~1ms cadence
 *
 * Run with --duration N to soak longer; default 5s.
 *
 * Exit 0 = no crash inside the test window. SIGBUS / abort = the bug
 * we were chasing reproduces standalone, no root, no ES, no wax.
 */

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Unlocks the n00b_thread_init / n00b_thread_destroy kwarg-style
 * decls in core/thread.h, same pattern as test_mutex.c / test_rwlock.c. */
#define __N00B_THREAD_INTERNAL

#include "n00b.h"
#include "core/alloc.h"
#include "core/gc.h"
#include "core/pool.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"

#define N00B_THREADS    4
#define FOREIGN_THREADS 4
#define DEFAULT_DURATION_SECONDS 5
#define COLLECT_PERIOD_US        1000

static _Atomic bool     g_stop = false;
static _Atomic uint64_t g_n00b_thread_ops;
static _Atomic uint64_t g_foreign_thread_ops;
static _Atomic uint64_t g_collect_count;

static n00b_pool_t g_pool;

static n00b_allocator_t *
pool_alloc(void)
{
    return (n00b_allocator_t *)&g_pool;
}

/* Burn a few allocations of varying size against the pool. The mix of
 * small (freelist class) and big (mmap'd page) allocs is what stresses
 * the pool's register / unregister hot path; pure small allocs would
 * never trip the GC-vs-pool race because they don't unregister anything. */
static void
churn_one_round(void)
{
    n00b_alloc_opts_t opts = {.allocator = pool_alloc(), .no_scan = true};

    /* Small allocation: freelist class, no mmap-tree churn. */
    uint8_t *small = n00b_alloc_array_with_opts(uint8_t, 64, &opts);
    /* Mid allocation: still in freelist range. */
    uint8_t *mid   = n00b_alloc_array_with_opts(uint8_t, 256, &opts);
    /* Big allocation: gets its own mmap'd page; n00b_mmap_register_pool_page
     * + matching n00b_mmap_unregister on free is what races. */
    uint8_t *big   = n00b_alloc_array_with_opts(uint8_t, 8192, &opts);

    /* Touch the memory so it's resident — pure mmap without write would
     * not stress phys_footprint, but more importantly the GC scan
     * conservatively dereferences inside pages it reaches, and a clean
     * page that's later unmapped vs a dirty one behave the same to
     * the kernel's protection bits. */
    if (small) small[0] = 1;
    if (mid)   mid[0]   = 2;
    if (big)   big[0]   = 3;

    n00b_free(big);
    n00b_free(mid);
    n00b_free(small);
}

/* n00b-spawned worker: gets full STW participation via every
 * _n00b_alloc_raw / n00b_free checkin. Baseline — should NEVER
 * cause a crash even under heavy GC pressure. */
static void *
n00b_worker(void *arg)
{
    (void)arg;
    while (!atomic_load(&g_stop)) {
        churn_one_round();
        atomic_fetch_add(&g_n00b_thread_ops, 1);
    }
    return nullptr;
}

/* Foreign pthread: simulates an XPC / libdispatch worker. Calls
 * n00b_thread_init() on first entry to attach to the runtime (so
 * STW iteration finds it in rt->threads), but otherwise does the
 * same churn pattern as the n00b worker. */
static void *
foreign_worker(void *arg)
{
    (void)arg;
    /* The raw pthread is initially unknown to n00b: __n00b_thread_self
     * is uninitialised until we call n00b_thread_init. */
    n00b_thread_init();

    while (!atomic_load(&g_stop)) {
        churn_one_round();
        atomic_fetch_add(&g_foreign_thread_ops, 1);
    }

    n00b_thread_destroy();
    return nullptr;
}

/* GC-pressure thread: trigger n00b_collect on the default arena at
 * a fast cadence so the STW window is open frequently relative to
 * the mutator churn rate. */
static void *
collect_worker(void *arg)
{
    n00b_arena_t *arena = (n00b_arena_t *)arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = COLLECT_PERIOD_US * 1000};
    while (!atomic_load(&g_stop)) {
        n00b_collect(arena);
        atomic_fetch_add(&g_collect_count, 1);
        nanosleep(&ts, nullptr);
    }
    return nullptr;
}

int
main(int argc, char **argv)
{
    int duration = DEFAULT_DURATION_SECONDS;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--duration") == 0) {
            duration = atoi(argv[i + 1]);
        }
    }

    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    /* Hidden + external_metadata, matching the rt->user_pool shape
     * the wax raw_gateway uses. This is the configuration whose
     * pool_alloc / pool_free traverse n00b_mmap_register_pool_page +
     * n00b_mmap_unregister and contend with GC mark. */
    n00b_pool_init(&g_pool,
                   .hidden            = true,
                   .external_metadata = true,
                   .name              = "test_hidden_pool_stw");

    /* Spawn n00b-managed workers via n00b_thread_spawn. */
    pthread_t n00b_native[N00B_THREADS];
    for (int i = 0; i < N00B_THREADS; i++) {
        auto r = n00b_thread_spawn(n00b_worker, nullptr);
        assert(n00b_result_is_ok(r));
        (void)n00b_result_get(r);
        /* n00b_thread_spawn returns the n00b_thread_t*, not a pthread_t;
         * we don't need the handle for join — main loop drives stop. */
        n00b_native[i] = 0;
        (void)n00b_native;
    }

    /* Spawn foreign pthreads — raw pthread_create, not n00b_thread_spawn. */
    pthread_t foreign[FOREIGN_THREADS];
    for (int i = 0; i < FOREIGN_THREADS; i++) {
        int rc = pthread_create(&foreign[i], nullptr, foreign_worker, nullptr);
        assert(rc == 0);
    }

    /* Spawn the collect-driver thread (n00b-spawned so its own
     * allocations are well-behaved; only its job is to call
     * n00b_collect at high frequency). */
    auto cr = n00b_thread_spawn(collect_worker, runtime.default_arena);
    assert(n00b_result_is_ok(cr));

    /* Soak. */
    fprintf(stderr,
            "test_hidden_pool_stw: %d n00b threads + %d foreign threads, "
            "collect every ~%dus, %d second soak\n",
            N00B_THREADS, FOREIGN_THREADS, COLLECT_PERIOD_US, duration);
    sleep(duration);
    atomic_store(&g_stop, true);

    /* Join foreign pthreads. n00b workers exit when g_stop flips
     * and the runtime tears down on shutdown. */
    for (int i = 0; i < FOREIGN_THREADS; i++) {
        (void)pthread_join(foreign[i], nullptr);
    }

    fprintf(stderr,
            "test_hidden_pool_stw: PASS — survived %ds\n"
            "  n00b_thread_ops    = %llu\n"
            "  foreign_thread_ops = %llu\n"
            "  collect_count      = %llu\n",
            duration,
            (unsigned long long)atomic_load(&g_n00b_thread_ops),
            (unsigned long long)atomic_load(&g_foreign_thread_ops),
            (unsigned long long)atomic_load(&g_collect_count));

    n00b_shutdown();
    return 0;
}
