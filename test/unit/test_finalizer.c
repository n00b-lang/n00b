#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/stw.h"
#include "core/atomic.h"
#include "core/buffer.h"
#include "core/data_lock.h"

// ============================================================================
// Test helper type
// ============================================================================

typedef struct {
    uint64_t value;
    void    *next;
} test_obj_t;

#define ARENA_ALLOC(a) .allocator = (n00b_allocator_t *)(a)

// ============================================================================
// 1. Basic finalizer invocation
// ============================================================================

static volatile int finalizer_call_count = 0;

static void
test_finalizer_callback(void *user_data)
{
    volatile int *flag = (volatile int *)user_data;
    (*flag)++;
}

// noinline so the object pointer lives only in this frame, which is
// dead when the caller triggers collection.
static __attribute__((noinline)) void
allocate_with_finalizer(n00b_arena_t *arena, volatile int *flag, int count)
{
    for (int i = 0; i < count; i++) {
        test_obj_t *obj = n00b_alloc(test_obj_t, ARENA_ALLOC(arena));
        obj->value      = 0xDEAD;
        n00b_add_finalizer(obj, test_finalizer_callback, (void *)flag);
    }
}

static void
test_basic_finalizer(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    volatile int flag = 0;

    allocate_with_finalizer(arena, &flag, 1);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    assert(flag == 1);
    printf("  [PASS] basic finalizer invocation\n");
}

// ============================================================================
// 2. Surviving object — finalizer NOT called
// ============================================================================

static void
test_surviving_object(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    volatile int flag = 0;

    test_obj_t *obj = n00b_alloc(test_obj_t, ARENA_ALLOC(arena));
    obj->value      = 0xBEEF;
    n00b_add_finalizer(obj, test_finalizer_callback, (void *)&flag);

    // Keep the reference alive on the stack during GC.
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // Object survived — finalizer must NOT have been called.
    assert(flag == 0);
    // Object should still be readable.
    assert(obj->value == 0xBEEF);

    printf("  [PASS] surviving object finalizer not called\n");
}

// ============================================================================
// 3. Multiple finalizers on different objects
// ============================================================================

static void
test_multiple_finalizers(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    finalizer_call_count = 0;

    allocate_with_finalizer(arena, &finalizer_call_count, 5);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // All 5 should have been finalized.
    assert(finalizer_call_count == 5);

    printf("  [PASS] multiple finalizers\n");
}

// ============================================================================
// 4. Finalizer via n00b_free (explicit free path)
// ============================================================================

static void
test_finalizer_on_free(void)
{
    volatile int flag = 0;

    // Use the default allocator (not a GC arena) so n00b_free works.
    test_obj_t *obj = n00b_alloc(test_obj_t);
    obj->value      = 0xCAFE;
    n00b_add_finalizer(obj, test_finalizer_callback, (void *)&flag);

    // Explicit free should run the finalizer.
    n00b_free(obj);

    assert(flag == 1);

    printf("  [PASS] finalizer on explicit free\n");
}

// ============================================================================
// 5. Lock cleanup (buffer with lock — no crash)
// ============================================================================

static __attribute__((noinline)) void
allocate_buffer_on_arena(n00b_arena_t *arena)
{
    n00b_buffer_t *buf = n00b_alloc(n00b_buffer_t, ARENA_ALLOC(arena));
    n00b_buffer_init(buf, .length = 64, .allocator = (n00b_allocator_t *)arena);
    memset(buf->data, 0xAB, 64);
}

static void
test_lock_cleanup(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 8192, .use_gc = true);

    allocate_buffer_on_arena(arena);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    // If we get here without crashing, the lock was freed safely.
    printf("  [PASS] lock cleanup on collection\n");
}

// ============================================================================
// 6. Finalizer via n00b_alloc _kargs (no separate n00b_add_finalizer call)
// ============================================================================

static __attribute__((noinline)) void
allocate_with_finalizer_kw(n00b_arena_t *arena, volatile int *flag)
{
    test_obj_t *obj = n00b_alloc(test_obj_t,
                                  ARENA_ALLOC(arena),
                                  .finalizer      = test_finalizer_callback,
                                  .finalizer_data = (void *)flag);
    obj->value = 0xF00D;
}

static void
test_finalizer_via_alloc_kw(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    volatile int  flag  = 0;

    allocate_with_finalizer_kw(arena, &flag);

    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();

    assert(flag == 1);
    printf("  [PASS] finalizer via n00b_alloc _kargs\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running finalizer tests...\n");

    test_basic_finalizer();
    test_surviving_object();
    test_multiple_finalizers();
    test_finalizer_on_free();
    test_lock_cleanup();
    test_finalizer_via_alloc_kw();

    printf("All finalizer tests passed.\n");
    return 0;
}
