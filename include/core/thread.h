/**
 * @file thread.h
 * @brief Thread management and per-thread state.
 *
 * Defines the n00b_thread_t structure, thread-local self pointer, and
 * helpers for capturing stack bounds (needed by the GC).
 */
#pragma once

#include <pthread.h>

#include "n00b.h"
#include "core/rt_access.h"
#include "core/option.h"

n00b_option_decl(pthread_attr_t);

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
    int     fds[2]; // 0 is read end, 1 is write end
    uint8_t ready;
} n00b_memperm_pipe_t;

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
    pthread_t           pthread_id;
    n00b_memperm_pipe_t memperm_pipe;
    n00b_futex_t        self_lock;
    pthread_attr_t      pthread_attrs;

#if 0
    // Stuff to support the locking subsystem:

    // These two are used to ensure we can clean up lock state to
    // be consistent, when a thread exits while holding locks.
    //
    // The read-locks list also tracks nesting information.
    _Atomic(n00b_lock_base_t *)       exclusive_locks;
    _Atomic(n00b_thread_read_log_t *) read_locks;
    _Atomic(n00b_thread_read_log_t *) log_alloc_cache;
    // This supports the Global Interpreter Lock.
    n00b_futex_t                      self_lock;
    // State associated with any condition variable we are waiting on.
    n00b_condition_thread_state_t     cv_info;
    n00b_lock_base_t                 *lock_wait_target;
    char                             *lock_wait_loc;
    n00b_arena_t                     *thread_arena;
    n00b_arena_t                     *pre_error_arena;
    char                             *lock_wait_trace;
#endif
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
    n00b_runtime_t *runtime       = n00b_get_runtime();
    uint32_t        acquired_slot = 0;
};

/**
 * @brief Record the stack base address for a thread.
 * @param thread  Thread to update.
 * @param runtime Runtime owning the thread.
 */
extern void n00b_capture_stack_base(n00b_thread_t *thread, n00b_runtime_t *runtime);

#endif
