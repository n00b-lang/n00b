/*
 * For internal system use. Thread-safe, but not typed.
 *
 *  Author:         John Viega, john@crashoverride.com
 *
 */
#define N00B_USE_INTERNAL_API
#include <stdatomic.h>

#include "n00b.h"
#include "core/allocator.h"
#include "core/alloc.h"
#include "core/align.h"
#include "core/dict_untyped.h"
#include "core/atomic.h"
#include "core/futex.h"

static inline n00b_uint128_t
compute_hash(n00b_dict_untyped_t *dict, void *key)
{
    if (dict->skip_obj_hash && dict->fn) {
        return (*dict->fn)(key);
    }
    else {
        return n00b_hash(key, dict->fn);
    }
}

// 75%
static inline uint32_t
resize_threshold(uint32_t size)
{
    return size - (size >> 2) - 1;
}

static inline int
bucket_reserved(n00b_dict_untyped_bucket_t *b)
{
    return (int)b->hv != 0;
}

static inline int
bucket_deleted(n00b_dict_untyped_bucket_t *b)
{
    return (n00b_atomic_load(&b->flags) & N00B_HT_FLAG_DELETED) != 0;
}

static inline uint32_t
new_dict_untyped_size(uint32_t last_bucket, uint32_t size)
{
    uint32_t table_size = last_bucket + 1;

    if (size >= table_size >> 1) {
        return table_size << 1;
    }
    // We will never bother to size back down to the smallest few
    // table sizes.
    if (size <= (N00B_DICT_UNTYPED_MIN_SIZE << 2)) {
        return N00B_DICT_UNTYPED_MIN_SIZE << 3;
    }
    if (size <= (table_size >> 2)) {
        return table_size >> 1;
    }

    return table_size;
}

static inline n00b_dict_untyped_store_t *
new_dict_untyped_store(n00b_dict_untyped_t *d, uint32_t alloc_items)
{
    n00b_dict_untyped_store_t *result;

    result            = n00b_alloc_flex(n00b_dict_untyped_store_t,
                                        n00b_dict_untyped_bucket_t,
                                        alloc_items,
                                        .allocator = d->allocator);
    result->last_slot = alloc_items - 1;
    result->threshold = resize_threshold(alloc_items);

    return result;
}

bool
n00b_dict_untyped_lock(n00b_dict_untyped_t *d, bool try, uint32_t *count)
{
    uint32_t flags    = N00B_HT_FLAG_COPYING;
    uint32_t new_used = 0;

    if (try) {
        flags |= N00B_HT_FLAG_MOVING;
    }
    else {
        n00b_atomic_add(&d->wait_ct, 1);
    }

    uint32_t v = n00b_atomic_or(&d->futex, 1UL << 31);

    while (v & (1UL << 31)) {
        if (try) {
            return false;
        }
        n00b_futex_wait_timespec(&d->futex, v, nullptr);
        v = n00b_atomic_or(&d->futex, 1UL << 31);
    }

    n00b_atomic_add(&d->wait_ct, -1);

    n00b_dict_untyped_store_t  *s = n00b_atomic_load(&d->store);
    n00b_dict_untyped_bucket_t *b;

    int first_active = -1;
    int last_active  = -1;

    for (uint32_t i = 0; i <= s->last_slot; i++) {
        b = &s->buckets[i];

        uint32_t f = n00b_atomic_or(&b->flags, flags);

        new_used += (bucket_reserved(b) & !bucket_deleted(b));

        if (f & N00B_HT_FLAG_MUTEX) {
            last_active = i;
            if (first_active == -1) {
                first_active = i;
            }
        }
    }

    // If we noticed writes in progress, go through the range of the
    // store that contained threads, and busy-wait if needed.
    if (last_active != -1) {
        for (int i = first_active; i <= last_active; i++) {
            while (n00b_atomic_load(&s->buckets[i].flags) & N00B_HT_FLAG_MUTEX) {}
        }
    }

    *count = new_used;

    return true;
}

static void
dict_untyped_unlock_post_migrate(n00b_dict_untyped_t *d, n00b_dict_untyped_store_t *s)
{
    atomic_store(&d->store, s);
    atomic_store(&d->futex, 0);

    if (n00b_atomic_load(&d->wait_ct)) {
        n00b_futex_wake(&d->futex, true);
    }
}

void
n00b_dict_untyped_unlock_post_copy(n00b_dict_untyped_t *d)
{
    n00b_dict_untyped_store_t *s = n00b_atomic_load(&d->store);

    for (uint32_t i = 0; i <= s->last_slot; i++) {
        n00b_atomic_and(&s->buckets[i].flags, ~N00B_HT_FLAG_COPYING);
    }

    atomic_store(&d->futex, 0);

    if (n00b_atomic_load(&d->wait_ct)) {
        n00b_futex_wake(&d->futex, true);
    }
}

static void
n00b_dict_untyped_migrate(n00b_dict_untyped_t *d)
{
    uint32_t                    nitems = 0;
    n00b_dict_untyped_store_t  *olds;
    n00b_dict_untyped_bucket_t *bold;

    if (!n00b_dict_untyped_lock(d, true, &nitems)) {
        n00b_atomic_add(&d->wait_ct, 1);
        n00b_futex_wait_for_value(&d->futex, 0);
        return;
    }
    olds = n00b_atomic_load(&d->store);

    uint32_t                   alloc_items = new_dict_untyped_size(olds->last_slot, nitems);
    n00b_dict_untyped_store_t *news        = new_dict_untyped_store(d, alloc_items);
    uint32_t                   last        = news->last_slot;
    uint32_t                   bix;

    atomic_store(&news->used_count, nitems);

    for (uint32_t i = 0; i <= olds->last_slot; i++) {
        bold = &olds->buckets[i];
        if (!bucket_reserved(bold) || bucket_deleted(bold)) {
            continue;
        }

        bix = bold->hv & last;

        for (uint32_t j = 0; j <= last; j++) {
            n00b_dict_untyped_bucket_t *bnew = &news->buckets[bix];

            if (!bucket_reserved(bnew)) {
                bnew->hv           = bold->hv;
                bnew->key          = bold->key;
                bnew->value        = bold->value;
                bnew->insert_order = bold->insert_order;
                break;
            }

            bix = (bix + 1) & news->last_slot;
        }
    }

    n00b_free(olds);

    dict_untyped_unlock_post_migrate(d, news);
}
