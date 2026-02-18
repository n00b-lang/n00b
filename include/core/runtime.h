/**
 * @file runtime.h
 * @brief Runtime state and initialization.
 *
 * Defines the n00b_runtime_t structure that holds global state (threads,
 * memory maps, allocators) and the n00b_init() entry point.
 */
#pragma once

#if !defined(N00B_THREADS_MAX)
#define N00B_THREADS_MAX 4096
#endif

#include <assert.h>

#include "n00b.h"
#include "core/alloc_base.h"
#include "core/rt_access.h"
#include "core/array.h"
#include "core/list.h"
#include "core/option.h"
#include "core/pool.h"

typedef struct n00b_runtime_t n00b_runtime_t;

n00b_array_decl(char *);
n00b_array_decl(n00b_thread_t *);
n00b_array_decl(uint32_t);

n00b_list_decl(n00b_gc_root_t);

// Forward declaration to avoid including interval_tree.h.
typedef struct n00b_interval_tree_t n00b_interval_tree_t;

struct n00b_mmap_ctx_t {
    n00b_interval_tree_t *mmap_tree;   // page-level mmaps
    n00b_interval_tree_t *range_tree;  // optional sub-range tracking
    _Atomic int64_t       tid_lock;
    n00b_pool_t           pool;
};

struct n00b_runtime_t {
    n00b_array_t(char *) argv;
    n00b_array_t(char *) envp;
    n00b_mmap_ctx_t             mmaps;
    _Atomic uint32_t            next_thread_slot;
    _Atomic uint32_t            live_threads;
    _Atomic bool                startup_complete;
    _Atomic(n00b_allocator_t *) default_allocator;
    n00b_arena_t               *default_arena; // GC'd arena (when using default allocator)
    n00b_pool_t                 gc_root_pool;  // System pool for root list storage.
    n00b_list_t(n00b_gc_root_t) gc_roots;      // User-registered GC roots.
    _Atomic(n00b_thread_t *)    thread_list[N00B_THREADS_MAX];
    uint32_t                    thread_generations[N00B_THREADS_MAX];
    n00b_base_allocator_t       slab_allocator;
    n00b_futex_t                stw;
    uint32_t                    stw_nesting;
};

/**
 * @brief Initialize the n00b runtime.
 * @param rt   Runtime structure to initialize.
 * @param argc Argument count from main().
 * @param argv Argument vector from main().
 *
 * @kw allocator    Allocator to use (nullptr = GC'd arena).
 * @kw envp         Environment pointer (nullptr = inherit).
 * @kw numeric_locale Numeric locale string ("" = default).
 * @kw fd_limit     File descriptor limit (0 = don't change, <0 = don't set).
 * @kw max_threads  Maximum thread count (default N00B_THREADS_MAX).
 *
 * @pre Must be called exactly once, from the main thread, before any
 *      other n00b API.
 * @post `n00b_get_runtime()` returns a valid pointer. The calling
 *       thread is registered with the STW subsystem.
 */
extern void
n00b_init(n00b_runtime_t *rt, int argc, char *argv[]) _kargs
{
    n00b_allocator_t *allocator       = nullptr; // nullptr = use a GC'd arena
    char             **envp           = nullptr;
    char              *numeric_locale = "";
    int                fd_limit       = 0; // Less than 0 = "don't set"
    unsigned int       max_threads    = N00B_THREADS_MAX;
};

/**
 * @brief Get the runtime's default allocator.
 * @return Pointer to the default allocator.
 */
static inline n00b_allocator_t *
n00b_default_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    assert(rt->default_allocator);

    return rt->default_allocator;
}

/**
 * @brief Get the runtime's slab (pool) allocator.
 * @return Pointer to the slab allocator.
 */
static inline n00b_allocator_t *
n00b_slab_allocator(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();

    assert(rt);
    return (n00b_allocator_t *)&rt->slab_allocator;
}
