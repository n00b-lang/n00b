#define N00B_MEM_INTERNAL_API
#define N00B_USE_INTERNAL_API

#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <assert.h>

#include "n00b.h"
#include "core/alloc_mdata.h"
#include "core/alloc.h"
#include "core/memory_info.h"
#include "core/mmaps.h"
#include "core/atomic.h"
#include "adt/llstack.h"
#include "core/align.h"
#include "core/pool.h"
#include "util/math.h"

static inline void
pool_lock(n00b_pool_t *pool)
{
    while (n00b_atomic_or(&pool->lock, 1) != 0)
        ;
}

static inline void
pool_unlock(n00b_pool_t *pool)
{
    n00b_atomic_store(&pool->lock, 0);
}

static inline void *
new_page_entry(n00b_pool_t *pool, uint64_t *sz_ptr)
{
    uint64_t sz     = *sz_ptr;
    uint64_t hdr_sz = n00b_align(sizeof(n00b_pool_page_t));
    sz += hdr_sz;

    char *name    = (char *)(((n00b_allocator_t *)pool)->debug_name);
    auto  mmap_r  = n00b_mmap(n00b_page_align(sz),
                              .allocator = (n00b_allocator_t *)pool,
                              .name      = name,
                              .kind      = n00b_mmap_pool);
    assert(n00b_result_is_ok(mmap_r));
    n00b_pool_page_t *cur = n00b_result_get(mmap_r);

    *sz_ptr   = n00b_page_align(sz) - hdr_sz;
    void *res = (void *)cur + hdr_sz;

    pool_lock(pool);
    cur->next = pool->page_table;
    if (cur->next) {
        cur->next->prev = cur;
    }
    pool->page_table = cur;
    pool_unlock(pool);

    return res;
}

// Here, sz includes space for the n00b_pool_entry_t at the front.
static inline n00b_pool_entry_t *
big_mmap(n00b_pool_t *pool, uint64_t sz)
{
    n00b_pool_entry_t *entry = new_page_entry(pool, &sz);
    entry->list_index        = N00B_NUM_FREE_LISTS;

    return entry;
}

static inline void
delete_one_page_entry(n00b_pool_t *pool, n00b_pool_page_t *entry)
{
    pool_lock(pool);

    if (entry->prev) {
        entry->prev->next = entry->next;
    }
    else {
        assert(pool->page_table == entry);
        pool->page_table = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    }

    pool_unlock(pool);
}

static inline void *
add_page_to_list(n00b_pool_t *pool, uint64_t sz, n00b_llstack_t *stack)
{
    // Return one, and push the others to the free list.
    // Yes, we could pre-link the extras, but that's a PITA.
    uint64_t alloc_sz = n00b_page_size - n00b_align(sizeof(n00b_pool_page_t));
    void    *res      = new_page_entry(pool, &alloc_sz);

    assert(!(((uint64_t)sz) & 15));

    char    *p = ((char *)res) + n00b_align(sz);
    uint32_t n = (alloc_sz / n00b_align(sz));

    for (uint32_t i = 1; i < n; i++) {
        // Yes, we end up having to cast.
        n00b_llstack_node_t *node = (n00b_llstack_node_t *)p;
        p += n00b_align(sz);
        n00b_llstack_push_node(stack, node);
    }

    return res;
}

static void
pool_destroy(n00b_pool_t *pool)
{
    n00b_pool_page_t *entry = pool->page_table;
    n00b_pool_page_t *next;

    while (entry) {
        next = entry->next;
        n00b_safe_munmap(entry, n00b_page_size);
        entry = next;
    }
}

static void
pool_free(n00b_pool_t *pool, void *ptr)
{
    n00b_pool_entry_t *entry = (n00b_pool_entry_t *)((char *)ptr - N00B_ALIGN);
    assert(entry->list_index <= N00B_NUM_FREE_LISTS);

    if (entry->list_index == N00B_NUM_FREE_LISTS) {
        /* `entry` sits at offset n00b_align(sizeof(n00b_pool_page_t))
         * past the page header (see `new_page_entry`).  Decrementing
         * a `n00b_pool_page_t *` by 1 only walks back
         * `sizeof(n00b_pool_page_t)` bytes, which is wrong when that
         * is smaller than N00B_ALIGN. */
        n00b_pool_page_t *page = (n00b_pool_page_t *)(((char *)entry)
                                                     - n00b_align(sizeof(n00b_pool_page_t)));
        delete_one_page_entry(pool, page);
        return;
    }

    unsigned int ix = entry->list_index;
    memset(entry, 0, ix << N00B_POST_ROUND_SHIFT);
    n00b_llstack_node_t *node_ptr = (n00b_llstack_node_t *)entry;

    n00b_llstack_push_node(&pool->free_lists[ix], node_ptr);
}

static void *
pool_alloc(n00b_pool_t *pool, uint64_t request, void *ignore)
{
    request += n00b_align(sizeof(n00b_pool_entry_t));

    uint64_t     sz = n00b_align_closest_pow2_ceil(request);
    unsigned int ix = n00b_int_log2(sz >> N00B_POST_ROUND_SHIFT);

    n00b_pool_entry_t *entry;

    if (ix >= N00B_NUM_FREE_LISTS) {
        entry = big_mmap(pool, request);
        ix    = N00B_NUM_FREE_LISTS;
    }
    else {
        n00b_llstack_t      *stack = &pool->free_lists[ix];
        n00b_llstack_node_t *nptr  = n00b_llstack_pop_node(stack);

        if (nptr) {
            entry = (n00b_pool_entry_t *)nptr;
        }
        else {
            entry = add_page_to_list(pool, sz, stack);
        }
    }

    entry->list_index = ix;
    void *p           = (void *)(((char *)entry) + N00B_ALIGN);

    assert(!(((uint64_t)p) & (N00B_ALIGN - 1)));
    return p;
}

n00b_allocator_t *
n00b_pool_init(n00b_pool_t *pool) _kargs
{
    bool        __system          = false;
    bool        inline_headers    = false;
    bool        external_metadata = false;
    bool        hidden            = false;
    const char *name              = "pool";
}
{
    n00b_allocator_setup((n00b_allocator_t *)pool,
                         (n00b_calloc_fn)pool_alloc,
                         .free              = (n00b_free_fn)pool_free,
                         .destroy           = (n00b_allocator_destroy_fn)pool_destroy,
                         .name              = (char *)name,
                         .inline_headers    = inline_headers,
                         .external_metadata = external_metadata,
                         .hidden            = hidden,
                         .__system          = __system);

    pool->lock       = 0;
    pool->page_table = nullptr;

    for (int i = 0; i < N00B_NUM_FREE_LISTS; i++) {
        n00b_llstack_init(&pool->free_lists[i]);
    }

    return (n00b_allocator_t *)pool;
}
