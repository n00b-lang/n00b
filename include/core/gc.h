/**
 * @file gc.h
 * @brief Garbage collector interface and supporting types.
 *
 * Implements a copying/compacting collector for @c n00b_arena_t heaps.
 * When an arena runs out of space and has @c collection_enabled set,
 * @c n00b_collect() copies live allocations to a new arena segment,
 * rewrites all pointers, and releases the old memory.
 *
 * The collector traces:
 *   - User-registered GC roots (@ref n00b_gc_register_root)
 *   - Thread stacks (all registered threads)
 *   - The @c argv / @c envp arrays in @c n00b_runtime_t
 *
 * Callers must hold the stop-the-world lock before invoking
 * @c n00b_collect().
 */
#pragma once

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "adt/list.h"
#include "adt/dict_untyped.h"
#include "core/pool.h"
#include "core/arena.h"

// ============================================================================
// GC root type
// ============================================================================

/**
 * @brief Describes a user-registered GC root: a memory range the
 *        collector must scan for heap pointers.
 *
 * @see n00b_gc_register_root
 */
typedef struct n00b_gc_root_t {
    void  *addr;      /**< Start of the scannable region. */
    size_t num_words;  /**< Number of pointer-sized words to scan. */
} n00b_gc_root_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Run a copying collection on @p arena.
 *
 * All live allocations reachable from the root set are copied to a
 * fresh segment; stale memory is unmapped.
 *
 * @param arena  The arena to collect.
 * @pre The STW lock must be held by the caller.
 * @pre @p arena has `collection_enabled` set.
 * @post All live allocations in @p arena have been relocated; old
 *       segments are unmapped and pointers are rewritten.
 */
extern void n00b_collect(n00b_arena_t *arena);

/**
 * @brief Register a memory range as a GC root.
 *
 * The collector will scan @p num_words pointer-sized words starting
 * at @p addr during every collection.  Use @ref n00b_gc_register_root
 * for the convenient macro interface.
 *
 * @param addr      Start address of the root region.
 * @param num_words Number of pointer-sized words to scan.
 * @pre Runtime must be initialized.
 * @pre @p addr must remain valid for the lifetime of the registration.
 */
extern void _n00b_gc_register_root(void *addr, size_t num_words);

/**
 * @brief Unregister a previously registered GC root.
 *
 * Removes the first root whose address matches @p addr.
 *
 * @param addr  The address originally passed to register.
 */
extern void _n00b_gc_unregister_root(void *addr);

/**
 * @brief Register a variable as a GC root.
 *
 * Takes the address of @p var and computes the number of
 * pointer-sized words from its size.
 *
 * @code
 *     int *my_ptr;
 *     n00b_gc_register_root(my_ptr);
 * @endcode
 */
#define n00b_gc_register_root(var)                                                             \
    _n00b_gc_register_root(&(var),                                                             \
                           (sizeof(var) + sizeof(void *) - 1) / sizeof(void *))

/**
 * @brief Unregister a variable previously registered as a GC root.
 */
#define n00b_gc_unregister_root(var) _n00b_gc_unregister_root(&(var))

// ============================================================================
// Constants
// ============================================================================

#define N00B_DEFAULT_GC_ARENA_SIZE (1 << 16) // 64 KiB for to-space initial
#define N00B_GC_WL_START_SIZE      256
#define N00B_TOO_FEW_ALLOCS        128

// ============================================================================
// Internal types (used by gc.c; exposed for arena.c helpers)
// ============================================================================

/**
 * @brief Work-list entry: a to-space allocation that still needs scanning.
 */
typedef struct {
    n00b_inline_hdr_t *tospace_alloc;
    uint32_t           num_words;
} n00b_gc_wl_item_t;

/**
 * @brief Per-collection state, stack-allocated by the collector entry point.
 */
typedef struct {
    n00b_arena_t                     *from_space;
    n00b_arena_t                     *to_space;
    n00b_pool_t                       work_pool;
    n00b_list_t(n00b_gc_wl_item_t *)  worklist;
    n00b_dict_untyped_t               memos;
} n00b_collect_t;

// ============================================================================
// Arena metric helpers (also used by arena.c)
// ============================================================================

/**
 * @brief Total bytes used across all segments of @p arena.
 * @param arena  Target arena.
 * @return       Byte count of live allocation space.
 */
static inline uint64_t
n00b_arena_used(n00b_arena_t *arena)
{
    uint64_t        sz      = 0;
    n00b_segment_t *segment = arena->current_segment;

    // The current (top) segment doesn't have last_addr set yet.
    segment->last_addr = atomic_load(&arena->next_alloc);

    while (segment) {
        sz     += (uint64_t)(segment->last_addr - (char *)&segment->mem[0]);
        segment = segment->next_segment;
    }

    return sz;
}

/**
 * @brief Total capacity (usable bytes) across all segments of @p arena.
 * @param arena  Target arena.
 * @return       Byte count of total segment capacity.
 */
static inline uint64_t
n00b_arena_size(n00b_arena_t *arena)
{
    uint64_t        sz      = 0;
    n00b_segment_t *segment = arena->current_segment;

    while (segment) {
        sz     += segment->size - sizeof(n00b_segment_t);
        segment = segment->next_segment;
    }

    return sz;
}
