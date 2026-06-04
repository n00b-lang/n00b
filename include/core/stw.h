/**
 * @file stw.h
 * @brief Stop-the-world (STW) synchronization infrastructure.
 *
 * Provides thread suspension/resumption primitives used by the garbage
 * collector to achieve a consistent snapshot of all thread stacks.
 */
#pragma once

#include "n00b.h"
#include "core/thread.h"
#include "core/macros.h"
#include "core/runtime.h"

#define N00B_STW      0x40000000U
#define N00B_BLOCKING 0x20000000U
#define N00B_SUSPEND  0x00000001U
#define N00B_RUNNING  0x00000000U
#define N00B_NO_OWNER -1

/**
 * @brief Halt all threads for GC.  Use the n00b_stop_the_world() macro.
 * @pre  Runtime must be initialized; caller must be a registered thread.
 * @post All other threads are suspended at safe points.
 */
extern void _n00b_stop_the_world(char *loc);

/**
 * @brief Resume all threads after GC.  Use the n00b_restart_the_world() macro.
 * @pre  The calling thread holds the STW lock (via _n00b_stop_the_world).
 * @post All suspended threads are resumed.
 */
extern void _n00b_restart_the_world(char *loc);

/** @brief Suspend the calling thread (for blocking ops during STW). */
extern void _n00b_thread_suspend(char *loc);

/** @brief Resume the calling thread after a blocking suspension. */
extern void _n00b_thread_resume(char *loc);

/** @brief Check in with the STW subsystem (called after futex waits). */
extern void n00b_thread_checkin(void);

/** @brief Wait for an active STW owner to release the world. */
extern void n00b_wait_for_stw_release(void);

/** @brief Register the calling thread with the STW subsystem. */
extern void n00b_thread_start(void);

/**
 * @brief Install the WP-4 preemptive-STW suspend mechanism (D-040).
 *
 * Called once from n00b_init.  On Linux it installs the RT-signal handler used
 * to preemptively suspend RUNNING threads at GC time; a no-op where suspension
 * needs no signal (macOS Mach thread_suspend / Windows SuspendThread).
 */
extern void n00b_stw_init(void);

#define n00b_stop_the_world()    _n00b_stop_the_world(N00B_LOC_STRING())
#define n00b_restart_the_world() _n00b_restart_the_world(N00B_LOC_STRING())

#define n00b_run_blocking(...)                                                                 \
    {                                                                                          \
        n00b_jmp_buf_t save_state = {};                                                        \
        if (!n00b_setjmp(&save_state)) {                                                       \
            n00b_capture_stack_top(n00b_thread_self());                                        \
            _n00b_thread_suspend(N00B_LOC_STRING());                                           \
            __VA_ARGS__;                                                                       \
            _n00b_thread_resume(N00B_LOC_STRING());                                            \
            n00b_longjmp(&save_state, 1);                                                      \
        }                                                                                      \
        else {                                                                                 \
            n00b_thread_checkin();                                                             \
        }                                                                                      \
    }

typedef struct {
    n00b_jmp_buf_t save_state;
    volatile void  *jmp_target;
} n00b_stw_suspend_ctx;

#define n00b_jmp_paste(x, y) x##y
#define n00b_jmp_label(x)    n00b_jmp_paste(__n00b_stw_target_, x)

#define n00b_thread_suspend(ctx)                                                               \
    n00b_thread_t *t = n00b_thread_self();                                                     \
    if (!t) {                                                                                  \
        exit(-1);                                                                              \
    }                                                                                          \
    if (n00b_atomic_load(&n00b_get_runtime()->stw) != (uint32_t)t->id_info.parts.id) {           \
        ctx.jmp_target = nullptr;                                                              \
        if (!n00b_setjmp(&ctx.save_state)) {                                                   \
            n00b_capture_stack_top(n00b_thread_self());                                        \
            _n00b_thread_suspend(N00B_LOC_STRING());                                           \
        }                                                                                      \
        else {                                                                                 \
            goto *(void *)ctx.jmp_target;                                                      \
        }                                                                                      \
    }

#define n00b_thread_resume(ctx)                                                                \
    {                                                                                          \
        n00b_thread_t *t = n00b_thread_self();                                                 \
        if (!t) {                                                                              \
            exit(-1);                                                                          \
        }                                                                                      \
        if (n00b_atomic_load(&n00b_get_runtime()->stw) != (uint32_t)t->id_info.parts.id) {                      \
            ctx.jmp_target = &&n00b_jmp_label(__LINE__);                                       \
                                                                                              \
            _n00b_thread_resume(N00B_LOC_STRING());                                            \
                                                                                               \
            n00b_longjmp(&ctx.save_state, 1);                                                  \
            n00b_jmp_label(__LINE__) :;                                                        \
        }                                                                                      \
    }

/** @pre Runtime must be initialized. */
static inline bool
n00b_world_is_stopped(void)
{
    return n00b_atomic_load(&n00b_get_runtime()->stw) != (uint32_t)N00B_NO_OWNER;
}

// ============================================================================
// Can't-STW barrier (WP-001)
//
// A thread inside an STW-unsafe critical section — currently the mmap
// interval-tree mutation, whose collector side reads it lock-free under STW —
// brackets that section with n00b_stw_barrier_enter()/_leave().  The STW
// initiator drains the barrier (waits for stw_barrier_count == 0, blocking new
// entries) BEFORE suspending any thread, so no thread is ever frozen mid-tree-op
// and the collector's lock-free walk sees a consistent tree.  Once drained and
// every other thread is suspended, the initiator sets stw_exclusive; the mmap
// fast paths short-circuit on it (one atomic load, no n00b_thread_self()) since
// the collector is then the sole runner.
// ============================================================================

/**
 * @brief True during the collector's sole-runner STW phase (barrier drained +
 *        all other threads suspended).  @pre Runtime initialized.
 */
static inline bool
n00b_stw_in_exclusive(void)
{
    return n00b_atomic_load(&n00b_get_runtime()->stw_exclusive);
}

/**
 * @brief Enter a can't-STW critical section (the barrier "reader" side).
 *        Blocks while a stop-the-world owner is set, then registers; the
 *        post-increment re-check closes the race with an owner that appears
 *        between the wait and the increment.  Must NOT be called by the STW
 *        owner (it would self-deadlock) — the mmap fast paths gate it behind
 *        n00b_stw_in_exclusive().  Defined in stw.c (uses the futex wait).
 * @pre Runtime initialized; caller is not the STW owner.
 */
extern void n00b_stw_barrier_enter(void);

/** @brief Leave a can't-STW critical section. @pre A matching enter. */
static inline void
n00b_stw_barrier_leave(void)
{
    n00b_atomic_add(&n00b_get_runtime()->stw_barrier_count, -1);
}
