/**
 * @file pool.h
 * @brief Fixed-size slab pool allocator.
 *
 * Provides fast allocation of small, fixed-size objects using
 * power-of-two size classes with lock-free free lists.
 */
#pragma once

#include "n00b.h"
#include "core/alloc_base.h"
#include "adt/llstack.h"
#include "core/align.h"

#define N00B_POST_ROUND_SHIFT 6
#define N00B_NUM_FREE_LISTS   4

typedef struct n00b_pool_page_t {
    struct n00b_pool_page_t *prev;
    struct n00b_pool_page_t *next;
    /* Page-aligned size of the underlying mmap, captured by
     * new_page_entry so pool_free's big-alloc path can munmap the
     * page when its single entry is freed. Without this, big-alloc
     * pool_free was only unlinking the page from page_table — the
     * memory stayed mapped until pool_destroy. Observed leak: any
     * pool client that n00b_free's a >N00B_NUM_FREE_LISTS-class
     * allocation gave back its slot in the free list but the page
     * remained mapped. */
    size_t                   mapped_size;
} n00b_pool_page_t;

typedef struct {
    unsigned int list_index;
} n00b_pool_entry_t;

static_assert(sizeof(n00b_pool_entry_t) <= N00B_ALIGN);

struct n00b_pool_t {
    n00b_base_allocator_t vtable;
    n00b_llstack_t        free_lists[N00B_NUM_FREE_LISTS];
    n00b_pool_page_t     *page_table;
    _Atomic uint32_t      lock;
};

typedef struct n00b_pool_t n00b_pool_t;

/**
 * @brief Initialize a pool allocator.
 * @param pool Pool structure to initialize.
 * @return     Allocator interface pointer for the pool.
 *
 * @kw __system          System pool — skip STW checks (internal only).
 * @kw inline_headers    Prepend inline headers to allocations.
 * @kw external_metadata Keep OOB metadata in a separate arena.
 * @kw hidden            Hide from GC.
 * @kw name              Debug name for the pool.
 *
 * @pre @p pool points to zeroed or uninitialized memory.
 * @post The returned allocator is ready for use.
 */
extern n00b_allocator_t *
n00b_pool_init(n00b_pool_t *pool) _kargs
{
    bool        __system          = false;
    bool        inline_headers    = false;
    bool        external_metadata = false;
    bool        hidden            = false;
    const char *name              = "pool";
};
