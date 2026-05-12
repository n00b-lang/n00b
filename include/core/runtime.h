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
#include "core/thread.h"
#include "adt/array.h"
#include "adt/list.h"
#include "adt/option.h"
#include "adt/variant.h"
#include "adt/interval_tree.h"
#include "core/pool.h"
#include "conduit/conduit_types.h"

typedef struct n00b_runtime_t n00b_runtime_t;

// Forward declarations to avoid circular includes.
typedef struct n00b_conduit_service    n00b_conduit_service_t;

/**
 * @brief Variant type for the unified mmap interval tree.
 *
 * Discriminates between full mmap records (page-level regions) and
 * lightweight sub-range records (headerless allocations).
 */
typedef n00b_variant_t(n00b_mmap_info_t *, n00b_alloc_range_t *) n00b_mmap_data_t;

struct n00b_mmap_ctx_t {
    n00b_interval_tree_t(n00b_mmap_data_t) *mmap_tree;
    _Atomic int64_t  tid_lock;
    n00b_pool_t      pool;
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
    n00b_pool_t                 system_pool;   // System pool for root list & lock records.
    n00b_list_t(n00b_gc_root_t) gc_roots;      // User-registered GC roots.
    /* Non-hidden, non-arena allocators (e.g. caller-owned pools created
     * with `hidden = false`).  The GC walks every page of each pool in
     * this list during a collect so pointers FROM pool memory INTO the
     * default arena get traced/forwarded.  Hidden pools are NOT in
     * this list.  Backed by system_pool. */
    n00b_list_t(n00b_allocator_t *) scannable_pools;
    n00b_list_t(n00b_finalizer_info_t *) finalizers; // Global finalizer list (system_pool-backed).
    n00b_dict_untyped_t        *type_registry;     // typehash -> n00b_type_info_t *
    n00b_pool_t                 conduit_pool;      // Pool for conduit infra (registered as GC root).
    n00b_dict_untyped_t        *sub_map;           // conduit subscription handle -> sub ptr
    n00b_conduit_t             *default_conduit;   // Default conduit for IO service.
    n00b_conduit_service_t     *default_service;   // Service thread pool (IO + signal).
    n00b_conduit_fd_owner_t    *stdout_owner;      // Managed fd 1.
    n00b_conduit_fd_owner_t    *stderr_owner;      // Managed fd 2.
    n00b_conduit_topic_base_t  *stdout_topic;      // Typed stdout buffer topic.
    n00b_conduit_topic_base_t  *stderr_topic;      // Typed stderr buffer topic.
    n00b_thread_record_t        threads[N00B_THREADS_MAX];
    n00b_base_allocator_t       slab_allocator;
    n00b_futex_t                stw;
    uint32_t                    stw_nesting;
    const char                 *theme_name;    // Active theme name (set during init).
    n00b_unicode_ctx_t         *unicode_ctx;   // Phase 4.5 unicode subsystem state.
    n00b_regex_ctx_t           *regex_ctx;     // Regex port-side caches.
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
 * @brief Plain-C wrapper for n00b_init() with default kargs.
 *
 * Callable from code not compiled through ncc (e.g. startup shims
 * for AOT-compiled binaries).  Heap-allocates an n00b_runtime_t
 * internally.
 */
extern void n00b_init_simple(int argc, char *argv[]);

/**
 * @brief Shut down the runtime, stopping all service threads.
 * @pre  Must be called from the main thread before returning from main().
 * @post All conduit IO threads have exited.
 */
extern void n00b_shutdown(void);

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
