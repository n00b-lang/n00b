/**
 * @file thread.h
 * @brief Thread management and per-thread state.
 *
 * Defines the n00b_thread_t structure, thread-local self pointer, and
 * helpers for capturing stack bounds (needed by the GC).
 */
#pragma once

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "n00b.h"
#include "core/rt_access.h"
#include "core/option.h"
#include "core/result.h"

#if !defined(_WIN32)
n00b_option_decl(pthread_attr_t);
#endif

/** @brief Thread-local storage for the current thread's n00b_thread_t. */
extern thread_local n00b_thread_t __n00b_thread_self;

/**
 * @brief Get a pointer to the calling thread's n00b_thread_t.
 * @return Pointer to the thread-local n00b_thread_t.
 */
static inline n00b_thread_t *
n00b_thread_self(void)
{
    return &__n00b_thread_self;
}

typedef struct {
#if defined(_WIN32)
    uint8_t ready;
#else
    int     fds[2]; // 0 is read end, 1 is write end
    uint8_t ready;
#endif
} n00b_memperm_pipe_t;

/**
 * @brief Per-thread state for condition variable waits.
 *
 * Defined here (rather than in condition.h) so that
 * `n00b_thread_record_t` can embed it by value without a
 * circular include dependency.
 */
struct n00b_condition_thread_state_t {
    n00b_condition_t *current_cv;
    uint64_t          wait_predicate;
    void             *thread_param;
    char             *wait_loc;
};

/**
 * @brief Per-thread shared record allocated from the system pool.
 *
 * Lives in `rt->threads[]` so that *other* threads can walk lock
 * chains on crash or thread-exit cleanup.  The owning thread
 * accesses this through `n00b_thread_t::record`.
 */
struct n00b_thread_record_t {
    _Atomic(n00b_thread_t *)          thread;          ///< Back-pointer to TLS thread_t.
    uint32_t                          generation;      ///< Generation counter for slot reuse.
    _Atomic(n00b_lock_base_t *)       exclusive_locks; ///< Head of exclusive-lock chain.
    _Atomic(n00b_thread_read_log_t *) read_locks;      ///< Head of read-lock chain.
    _Atomic(n00b_thread_read_log_t *) log_alloc_cache; ///< Cached freed read-log entries.
    n00b_condition_thread_state_t     cv_info;         ///< Condition-variable wait state.
    n00b_lock_base_t                 *lock_wait_target;///< Lock we are currently blocked on.
    char                             *lock_wait_loc;   ///< Source location of the wait.
    char                             *lock_wait_trace; ///< Backtrace at wait (debug).
};

struct n00b_thread_t {
    union {
        struct {
            int32_t id;
            int32_t generation;
        } parts;
        uint64_t unique_id;
    } id_info;

    void               *stack_base;
    void               *stack_top;
    n00b_mmap_info_t   *stack_map;
    n00b_memperm_pipe_t memperm_pipe;
#if defined(_WIN32)
    uint32_t            os_thread_id;
#else
    pthread_t          pthread_id;
    pthread_attr_t     pthread_attrs;
#endif
    n00b_futex_t        self_lock;
    n00b_thread_record_t *record; ///< Pointer into rt->threads[slot].
};

/**
 * @brief Record the current stack top for GC scanning.
 * @param thread Thread whose stack_top to update.
 * @pre @p thread is the calling thread's n00b_thread_t.
 * @post `thread->stack_top` points to the approximate top of the stack frame.
 */
static inline void
n00b_capture_stack_top(n00b_thread_t *thread)
{
    void *ptr;
#if defined(__GNUC__)
    // This dodges a silly warning.
    static volatile uint64_t x = ~0ULL;
    thread->stack_top          = (void *)(((uint64_t)&ptr) & x);
#else
    thread->stack_top = &ptr;
#endif
}

/** @brief Get the unique (slot + generation) 64-bit thread ID. */
static inline int64_t
n00b_thread_unique_id(void)
{
    return __n00b_thread_self.id_info.unique_id;
}

/** @brief Get the current thread's slot index. */
static inline int32_t
n00b_thread_id(void)
{
    return __n00b_thread_self.id_info.parts.id;
}

/** @brief Get the current thread's generation counter. */
static inline int32_t
n00b_thread_generation(void)
{
    return __n00b_thread_self.id_info.parts.generation;
}

/**
 * @brief Spawn a new thread with full n00b lifecycle.
 *
 * Reserves a thread slot, creates a pthread wrapped in the n00b
 * launcher (GC registration, STW participation, lock cleanup on exit).
 *
 * @param fn   Thread entry point.
 * @param arg  Argument passed to @p fn.
 * @return     The spawned thread, or an error code (ENXIO, ENOMEM, or
 *             the pthread_create failure code).
 *
 * @pre  Runtime must be initialized.
 * @post The new thread participates in GC stop-the-world.
 */
extern n00b_result_t(n00b_thread_t *) n00b_thread_spawn(void *(*fn)(void *), void *arg);

/**
 * @brief Join a spawned thread.
 * @param thread  Thread to join (from n00b_thread_spawn).
 * @return        Thread return value.
 */
extern void *n00b_thread_join(n00b_thread_t *thread);

#if defined __N00B_THREAD_INTERNAL
/**
 * @brief Initialize the calling thread's n00b_thread_t (internal).
 *
 * @kw runtime       Runtime to register with.
 * @kw attrs         Optional pthread attributes.
 * @kw acquired_slot Pre-acquired thread slot index (0 = auto-assign).
 *
 * @pre Runtime must be initialized.
 * @post The calling thread is registered in the runtime's thread table
 *       and participates in STW pauses.
 */
void
n00b_thread_init() _kargs
{
    n00b_runtime_t *runtime             = n00b_get_runtime();
#if !defined(_WIN32)
    n00b_option_t(pthread_attr_t) attrs = n00b_option_none(pthread_attr_t);
#endif
    uint32_t acquired_slot              = 0;
};

/**
 * @brief Tear down the calling thread's n00b_thread_t.
 *
 * Releases any locks still held, clears the thread record, and
 * decrements `live_threads`.
 *
 * @pre  The calling thread was previously initialized via n00b_thread_init().
 * @post The thread slot is available for reuse.
 */
extern void n00b_thread_destroy(void);

/**
 * @brief Record the stack base address for a thread.
 * @param thread  Thread to update.
 * @param runtime Runtime owning the thread.
 */
extern void n00b_capture_stack_base(n00b_thread_t *thread, n00b_runtime_t *runtime);

#endif
