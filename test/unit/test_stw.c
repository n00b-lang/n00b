#define N00B_USE_INTERNAL_API

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"

static _Atomic uint32_t worker_resume_bits;
static _Atomic uint32_t worker_blocking_stage;
static _Atomic uint32_t worker_blocking_bits;
static _Atomic uint32_t worker_blocking_after_bits;

enum {
    WORKER_BLOCKING_INIT = 0,
    WORKER_BLOCKING_READY,
    WORKER_BLOCKING_RELEASE,
};

static uint32_t
self_lock_bits(void)
{
    return n00b_atomic_load(&n00b_thread_self()->self_lock);
}

static void
assert_no_stw_bits(void)
{
    uint32_t bits = self_lock_bits();

    assert((bits & (N00B_STW | N00B_BLOCKING | N00B_SUSPEND)) == 0);
}

static void
test_suspend_resume_clears_suspend(void)
{
    n00b_stw_suspend_ctx stw_ctx = {0};

    assert_no_stw_bits();

    n00b_thread_suspend(stw_ctx);
    assert(self_lock_bits() & N00B_SUSPEND);

    n00b_thread_resume(stw_ctx);
    assert_no_stw_bits();

    printf("  [PASS] suspend/resume clears SUSPEND\n");
}

static void
test_stw_owner_not_left_suspended(void)
{
    assert_no_stw_bits();

    n00b_stop_the_world();
    assert(n00b_world_is_stopped());
    assert((self_lock_bits() & N00B_SUSPEND) == 0);
    n00b_restart_the_world();

    assert_no_stw_bits();

    printf("  [PASS] STW owner is not left suspended\n");
}

static void
test_wait_for_stw_release_clears_blocking_without_owner(void)
{
    assert_no_stw_bits();

    n00b_wait_for_stw_release();

    assert_no_stw_bits();

    printf("  [PASS] wait_for_stw_release clears BLOCKING without owner\n");
}

static void *
worker_checkin_during_stw(void *arg)
{
    (void)arg;

    atomic_store(&worker_blocking_stage, WORKER_BLOCKING_READY);

    while (atomic_load(&worker_blocking_stage) == WORKER_BLOCKING_READY) {
        n00b_thread_checkin();
    }

    atomic_store(&worker_blocking_after_bits, self_lock_bits());

    return nullptr;
}

static void
test_stw_checkin_sets_and_clears_blocking(void)
{
    atomic_store(&worker_blocking_stage, WORKER_BLOCKING_INIT);
    atomic_store(&worker_blocking_bits, UINT32_MAX);
    atomic_store(&worker_blocking_after_bits, UINT32_MAX);

    auto result = n00b_thread_spawn(worker_checkin_during_stw, nullptr);
    assert(n00b_result_is_ok(result));

    n00b_thread_t *thread = n00b_result_get(result);

    while (atomic_load(&worker_blocking_stage) != WORKER_BLOCKING_READY) {
    }

    n00b_stop_the_world();

    uint32_t bits = n00b_atomic_load(&thread->self_lock);
    atomic_store(&worker_blocking_bits, bits);

    assert(bits & N00B_STW);
    assert(bits & N00B_BLOCKING);

    atomic_store(&worker_blocking_stage, WORKER_BLOCKING_RELEASE);
    n00b_restart_the_world();

    n00b_thread_join(thread);

    bits = atomic_load(&worker_blocking_after_bits);
    assert((bits & (N00B_STW | N00B_BLOCKING | N00B_SUSPEND)) == 0);

    printf("  [PASS] STW checkin sets and clears BLOCKING\n");
}

static void *
worker_suspend_resume(void *arg)
{
    (void)arg;

    n00b_stw_suspend_ctx stw_ctx = {0};

    n00b_thread_suspend(stw_ctx);
    n00b_thread_resume(stw_ctx);

    atomic_store(&worker_resume_bits, self_lock_bits());

    return nullptr;
}

static void
test_spawned_thread_resume_clears_suspend(void)
{
    atomic_store(&worker_resume_bits, UINT32_MAX);

    auto result = n00b_thread_spawn(worker_suspend_resume, nullptr);
    assert(n00b_result_is_ok(result));

    n00b_thread_t *thread = n00b_result_get(result);
    n00b_thread_join(thread);

    uint32_t bits = atomic_load(&worker_resume_bits);
    assert((bits & (N00B_STW | N00B_BLOCKING | N00B_SUSPEND)) == 0);

    printf("  [PASS] spawned thread resume clears SUSPEND\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_stw:\n");
    test_suspend_resume_clears_suspend();
    test_stw_owner_not_left_suspended();
    test_wait_for_stw_release_clears_blocking_without_owner();
    test_stw_checkin_sets_and_clears_blocking();
    test_spawned_thread_resume_clears_suspend();
    printf("All STW tests passed.\n");

    n00b_shutdown();
    return 0;
}
