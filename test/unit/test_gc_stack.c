#define __N00B_THREAD_INTERNAL

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

#include "n00b.h"
#include "core/codegen_abi_inject.h" // GC stack-map types + push/pop/policy API
#include "core/alloc.h"
#include "core/arena.h"
#include "core/gc.h"
#include "core/gc_stack.h"
#include "core/runtime.h"
#include "core/stw.h"
#include "core/thread.h"
#include "core/atomic.h"

#define ARENA_OPTS(a)                                                                          \
    &(n00b_alloc_opts_t)                                                                       \
    {                                                                                          \
        .allocator = (n00b_allocator_t *)(a)                                                   \
    }

#define PTR_SAVE_MASK UINT64_C(0xa5a5a5a5a5a5a5a5)

typedef struct {
    uint64_t value;
    void    *next;
} exact_target_t;

typedef struct {
    n00b_arena_t     *arena;
    _Atomic uint32_t  stage;
    uintptr_t         before_xor;
    uintptr_t         after;
    uint64_t          value;
} fallback_worker_ctx_t;

enum {
    FALLBACK_WORKER_INIT = 0,
    FALLBACK_WORKER_READY,
    FALLBACK_WORKER_RELEASE,
};

static const n00b_gc_stack_slot_t one_root_slots[] = {
    {.root_index = 0, .num_words = 1},
};

static const n00b_gc_stack_map_t one_root_map = {
    .num_roots     = 1,
    .num_slots     = 1,
    .slots         = one_root_slots,
    .function_name = "one_root",
    .file_name     = __FILE__,
};

static __attribute__((noinline)) void
test_exact_only_declared_roots_inner(n00b_arena_t *arena)
{
    exact_target_t *live = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));
    live->value          = 0xABCD0001ULL;

    volatile exact_target_t *dead   = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));
    ((exact_target_t *)dead)->value = 0xDEAD0001ULL;

    uintptr_t live_before = (uintptr_t)live;
    uintptr_t dead_before = (uintptr_t)dead;

    void                  *roots[] = {&live};
    n00b_gc_stack_frame_t  frame;
    n00b_gc_stack_policy_t old_policy = n00b_gc_stack_set_policy(N00B_GC_STACK_EXACT_ONLY);

    n00b_gc_stack_push(&frame, &one_root_map, roots);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    n00b_gc_stack_pop(&frame);
    n00b_gc_stack_set_policy(old_policy);

    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
            assert((uintptr_t)live != live_before);

    }
    assert(live->value == 0xABCD0001ULL);
    assert((uintptr_t)dead == dead_before);
}

static void
test_exact_only_declared_roots(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_exact_only_declared_roots_inner(arena);
    printf("  [PASS] exact_only_declared_roots\n");
}

static __attribute__((noinline)) void
test_exact_only_nested_frames_collect(n00b_arena_t    *arena,
                                      exact_target_t **outer_slot,
                                      uintptr_t        outer_before)
{
    exact_target_t *inner = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));

    inner->value = 0xABCD0003ULL;

    uintptr_t inner_before = (uintptr_t)inner;

    void                 *roots[] = {&inner};
    n00b_gc_stack_frame_t frame;

    n00b_gc_stack_push(&frame, &one_root_map, roots);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    n00b_gc_stack_pop(&frame);

    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
            assert((uintptr_t)*outer_slot != outer_before);

    }
    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
        assert((uintptr_t)inner != inner_before);
    }
    assert((*outer_slot)->value == 0xABCD0002ULL);
    assert(inner->value == 0xABCD0003ULL);
}

static __attribute__((noinline)) void
test_exact_only_nested_frames_inner(n00b_arena_t *arena)
{
    exact_target_t *outer = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));

    outer->value = 0xABCD0002ULL;

    uintptr_t outer_before = (uintptr_t)outer;

    void                  *roots[] = {&outer};
    n00b_gc_stack_frame_t  frame;
    n00b_gc_stack_policy_t old_policy = n00b_gc_stack_set_policy(N00B_GC_STACK_EXACT_ONLY);

    n00b_gc_stack_push(&frame, &one_root_map, roots);
    test_exact_only_nested_frames_collect(arena, &outer, outer_before);
    n00b_gc_stack_pop(&frame);
    n00b_gc_stack_set_policy(old_policy);

    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
        assert((uintptr_t)outer != outer_before);
    }
    assert(outer->value == 0xABCD0002ULL);
}

static void
test_exact_only_nested_frames(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_exact_only_nested_frames_inner(arena);
    printf("  [PASS] exact_only_nested_frames\n");
}

static void *
fallback_no_frame_worker(void *arg)
{
    // Mention the manual GC-stack API so ncc does not auto-publish an exact
    // frame for this worker; this test intentionally exercises fallback
    // conservative scanning when no exact frame is active.
    if (false) {
        n00b_gc_stack_pop((n00b_gc_stack_frame_t *)nullptr);
    }

    fallback_worker_ctx_t *ctx = arg;
    exact_target_t        *live = n00b_alloc_with_opts(exact_target_t,
                                                       ARENA_OPTS(ctx->arena));

    live->value = 0xABCD0004ULL;

    uintptr_t         live_before    = (uintptr_t)live;
    volatile uint64_t stack_words[8] = {0};

    ctx->before_xor = live_before ^ PTR_SAVE_MASK;
    stack_words[3]  = live_before;

    n00b_gc_stack_policy_t old_policy =
        n00b_gc_stack_set_policy(N00B_GC_STACK_EXACT_WITH_FALLBACK);

    atomic_store(&ctx->stage, FALLBACK_WORKER_READY);

    while (atomic_load(&ctx->stage) == FALLBACK_WORKER_READY) {
    }

    ctx->after = stack_words[3];
    // Copying: read only if the conservative scan rewrote the slot (a stale
    // slot would fault instead of failing the assertion). Pin-all: the slot
    // is never rewritten by design, the object is kept in place, so read it
    // where it is.
    if (ctx->after != (ctx->before_xor ^ PTR_SAVE_MASK) || n00b_gc_pin_all_policy()) {
        ctx->value = ((exact_target_t *)(uintptr_t)ctx->after)->value;
    }

    n00b_gc_stack_set_policy(old_policy);

    return nullptr;
}

static void
test_exact_with_fallback_no_frame(void)
{
    n00b_arena_t          *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    fallback_worker_ctx_t  ctx   = {.arena = arena};

    auto result = n00b_thread_spawn(fallback_no_frame_worker, &ctx);
    assert(n00b_result_is_ok(result));
    n00b_thread_t *thread = n00b_result_get(result);

    while (atomic_load(&ctx.stage) != FALLBACK_WORKER_READY) {
    }

    n00b_stop_the_world();
    n00b_collect(arena);
    atomic_store(&ctx.stage, FALLBACK_WORKER_RELEASE);
    n00b_restart_the_world();

    n00b_thread_join(thread);

    uintptr_t before = ctx.before_xor ^ PTR_SAVE_MASK;

    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
        assert(ctx.after != before);
    }
    assert(ctx.value == 0xABCD0004ULL);
    printf("  [PASS] exact_with_fallback_no_frame\n");
}

static __attribute__((noinline)) void
test_exact_with_fallback_active_frame_inner(n00b_arena_t *arena)
{
    exact_target_t *live = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));
    live->value          = 0xABCD0005ULL;

    exact_target_t *dead = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));
    dead->value          = 0xDEAD0005ULL;

    uintptr_t         live_before = (uintptr_t)live;
    uintptr_t         dead_before = (uintptr_t)dead;
    volatile uint64_t stack_words[8] = {0};

    stack_words[3] = dead_before;

    void                  *roots[] = {&live};
    n00b_gc_stack_frame_t  frame;
    n00b_gc_stack_policy_t old_policy =
        n00b_gc_stack_set_policy(N00B_GC_STACK_EXACT_WITH_FALLBACK);

    n00b_gc_stack_push(&frame, &one_root_map, roots);
    n00b_stop_the_world();
    n00b_collect(arena);
    n00b_restart_the_world();
    n00b_gc_stack_pop(&frame);
    n00b_gc_stack_set_policy(old_policy);

    if (!n00b_gc_pin_all_policy()) { // pin-all: nothing moves
        assert((uintptr_t)live != live_before);
    }
    assert(live->value == 0xABCD0005ULL);
    assert(stack_words[3] == dead_before);
}

static void
test_exact_with_fallback_active_frame(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);

    test_exact_with_fallback_active_frame_inner(arena);
    printf("  [PASS] exact_with_fallback_active_frame\n");
}

#ifdef _WIN32
typedef struct {
    n00b_arena_t         *arena;
    _Atomic uint32_t      stage;
    uintptr_t            before_xor;
    uintptr_t            parent_after;
    uintptr_t            deep_after;
    uintptr_t            registered_lo;
    uintptr_t            published_lo;
    uintptr_t            root_addr;
    n00b_thread_t       *deep_self;
    n00b_thread_record_t *record;
    uint8_t              scratch_byte;
    bool                 resolve_in_deep_frame;
} stack_growth_ctx_t;

static __attribute__((noinline)) void
stack_growth_wait(stack_growth_ctx_t *ctx, uintptr_t encoded)
{
    volatile uintptr_t root_slot = encoded ^ PTR_SAVE_MASK;
    ctx->root_addr = (uintptr_t)&root_slot;
    if (ctx->resolve_in_deep_frame) {
        ctx->deep_self = n00b_thread_self();
    }
    ctx->published_lo = (uintptr_t)n00b_atomic_load(&ctx->record->stack_lo);
    atomic_store(&ctx->stage, FALLBACK_WORKER_READY);
    while (atomic_load(&ctx->stage) == FALLBACK_WORKER_READY) {
    }
    ctx->deep_after = root_slot;
}

static __attribute__((noinline)) void
grow_stack(stack_growth_ctx_t *ctx, uintptr_t encoded)
{
    volatile uint8_t scratch[512 * 1024];
    for (size_t i = 0; i < sizeof(scratch); i += 4096) {
        scratch[i] = (uint8_t)i;
    }
    stack_growth_wait(ctx, encoded);
    ctx->scratch_byte = scratch[0];
}

static DWORD WINAPI
stack_growth_worker(void *arg)
{
    stack_growth_ctx_t *ctx = arg;
    assert(n00b_thread_self() == nullptr);
    n00b_thread_init();
    exact_target_t *live = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(ctx->arena));
    live->value = 0xABCD0007ULL;
    ctx->before_xor = (uintptr_t)live ^ PTR_SAVE_MASK;
    n00b_thread_t *self = n00b_thread_self();
    ctx->record = self->record;
    ctx->registered_lo = (uintptr_t)n00b_atomic_load(&self->record->stack_lo);
    volatile uintptr_t parent_slot = (uintptr_t)live;
    live = nullptr;
    grow_stack(ctx, ctx->before_xor);
    ctx->parent_after = parent_slot;
    n00b_thread_destroy();
    return 0;
}

static void
test_native_stack_growth_case(bool resolve_in_deep_frame)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    stack_growth_ctx_t ctx = {
        .arena = arena,
        .resolve_in_deep_frame = resolve_in_deep_frame,
    };
    HANDLE thread = CreateThread(nullptr, 4 * 1024 * 1024, stack_growth_worker,
                                 &ctx, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    assert(thread != nullptr);

    while (atomic_load(&ctx.stage) != FALLBACK_WORKER_READY) {
    }
    assert(ctx.root_addr < ctx.registered_lo);
    if (resolve_in_deep_frame) {
        assert(ctx.deep_self == n00b_atomic_load(&ctx.record->thread));
        assert(ctx.published_lo <= ctx.root_addr);
    }
    else {
        assert(ctx.published_lo == ctx.registered_lo);
    }
    n00b_stop_the_world();
    n00b_collect(arena);
    atomic_store(&ctx.stage, FALLBACK_WORKER_RELEASE);
    n00b_restart_the_world();
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);

    if (!n00b_gc_pin_all_policy()) {
        uintptr_t before = ctx.before_xor ^ PTR_SAVE_MASK;
        assert(ctx.parent_after != before);
        assert(ctx.deep_after == ctx.parent_after);
    }
}

static void
test_native_stack_growth(void)
{
    test_native_stack_growth_case(false); // GC must scan before any bounds refresh.
    test_native_stack_growth_case(true);  // Foreign self-resolution refreshes the bound.
    printf("  [PASS] native_stack_growth\n");
}

static __attribute__((noinline)) void
test_main_stack_grows_below_registered_start(void)
{
    n00b_arena_t *arena = n00b_new_arena(.size = 4096, .use_gc = true);
    exact_target_t *live = n00b_alloc_with_opts(exact_target_t, ARENA_OPTS(arena));
    live->value = 0xABCD0006ULL;

    // Simulate a Windows stack registration made before deeper frames caused
    // StackLimit to move down. Only this local slot roots the arena object.
    volatile uintptr_t root_slot = (uintptr_t)live;
    live = nullptr;
    n00b_thread_t *self = n00b_thread_self();
    n00b_mmap_info_t *original = self->stack_map;
    n00b_mmap_info_t stale = *original;
    stale.start = (uint64_t)&root_slot + sizeof(root_slot);
    assert(stale.start < stale.end);
    assert((uint64_t)n00b_atomic_load(&self->record->stack_lo)
           <= (uint64_t)&root_slot);

    n00b_stop_the_world();
    self->stack_map = &stale;
    n00b_collect(arena);
    self->stack_map = original;
    n00b_restart_the_world();

    assert(((exact_target_t *)(uintptr_t)root_slot)->value == 0xABCD0006ULL);
    printf("  [PASS] main_stack_grows_below_registered_start\n");
}
#endif

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_gc_stack:\n");
    test_exact_only_declared_roots();
    test_exact_only_nested_frames();
    test_exact_with_fallback_no_frame();
    test_exact_with_fallback_active_frame();
#ifdef _WIN32
    test_main_stack_grows_below_registered_start();
    test_native_stack_growth();
#endif
    printf("All GC stack tests passed.\n");

    n00b_shutdown();
    return 0;
}
