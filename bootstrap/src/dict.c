/**
 * @file dict.c
 * @brief Dictionary implementation with module struct API.
 *
 * Minimal thread-safe dictionary extracted from n00b.
 * Uses per-bucket spin locks and linear probing.
 */

#define NCC_LIB_IMPL  // Prevent compat macros from interfering with definitions
#include "dict.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "base_alloc_shim.h"
#include <string.h>
#include <time.h>

#include "futex.h"

// Atomic helpers
#define atomic_load_acq(x)     atomic_load_explicit(x, memory_order_acquire)
#define atomic_store_rel(x, y) atomic_store_explicit(x, y, memory_order_release)
#define atomic_add(x, y)       atomic_fetch_add_explicit(x, y, memory_order_acq_rel)
#define atomic_or(x, y)        atomic_fetch_or_explicit(x, y, memory_order_acq_rel)
#define atomic_and(x, y)       atomic_fetch_and_explicit(x, y, memory_order_acq_rel)

// Bucket flags
#define FLAG_MUTEX   1
#define FLAG_COPYING 2
#define FLAG_DELETED 4
#define FLAG_MOVING  8

static inline ncc_hash_t
compute_hash(ncc_dict_t *dict, void *key)
{
    if (dict->hash_fn) {
        return dict->hash_fn(key);
    }
    return ncc_hash_ptr(key);
}

static inline uint32_t
resize_threshold(uint32_t size)
{
    return size - (size >> 2) - 1; // 75%
}

static inline bool
bucket_reserved(ncc_dict_bucket_t *b)
{
    return b->hv != 0;
}

static inline bool
bucket_deleted(ncc_dict_bucket_t *b)
{
    return (atomic_load_acq(&b->flags) & FLAG_DELETED) != 0;
}

static inline uint32_t
new_dict_size(uint32_t last_bucket, uint32_t size)
{
    uint32_t table_size = last_bucket + 1;

    if (size >= table_size >> 1) {
        return table_size << 1;
    }
    if (size <= (NCC_DICT_MIN_SIZE << 2)) {
        return NCC_DICT_MIN_SIZE << 3;
    }
    if (size <= (table_size >> 2)) {
        return table_size >> 1;
    }
    return table_size;
}

static inline ncc_dict_store_t *
new_dict_store(uint32_t alloc_items)
{
    size_t size = sizeof(ncc_dict_store_t)
                + sizeof(ncc_dict_bucket_t) * alloc_items;
    ncc_dict_store_t *result = base_calloc(1, size);

    if (result) {
        result->last_slot = alloc_items - 1;
        result->threshold = resize_threshold(alloc_items);
    }
    return result;
}

static inline void
unlock_bucket(ncc_dict_bucket_t *b)
{
    atomic_and(&b->flags, ~FLAG_MUTEX);
}

static bool
dict_lock(ncc_dict_t *d, bool try_lock, uint32_t *count)
{
    uint32_t flags    = FLAG_COPYING;
    uint32_t new_used = 0;

    if (try_lock) {
        flags |= FLAG_MOVING;
    }
    else {
        atomic_add(&d->wait_ct, 1);
    }

    uint32_t v = atomic_or(&d->futex, 1UL << 31);

    while (v & (1UL << 31)) {
        if (try_lock) {
            return false;
        }
        futex_wait((uint32_t *)&d->futex, v);
        v = atomic_or(&d->futex, 1UL << 31);
    }

    atomic_add(&d->wait_ct, -1);

    ncc_dict_store_t  *s = atomic_load_acq(&d->store);
    ncc_dict_bucket_t *b;

    int first_active = -1;
    int last_active  = -1;

    for (uint32_t i = 0; i <= s->last_slot; i++) {
        b = &s->buckets[i];

        uint32_t f = atomic_or(&b->flags, flags);

        new_used += (bucket_reserved(b) && !bucket_deleted(b));

        if (f & FLAG_MUTEX) {
            last_active = i;
            if (first_active == -1) {
                first_active = i;
            }
        }
    }

    if (last_active != -1) {
        for (int i = first_active; i <= last_active; i++) {
            while (atomic_load_acq(&s->buckets[i].flags) & FLAG_MUTEX) {
            }
        }
    }

    *count = new_used;
    return true;
}

static void
dict_unlock_post_migrate(ncc_dict_t *d, ncc_dict_store_t *s)
{
    atomic_store_rel(&d->store, s);
    atomic_store_rel(&d->futex, 0);

    if (atomic_load_acq(&d->wait_ct)) {
        futex_wake((uint32_t *)&d->futex, true);
    }
}

static void
dict_migrate(ncc_dict_t *d)
{
    uint32_t                nitems = 0;
    ncc_dict_store_t  *olds;
    ncc_dict_bucket_t *bold;

    if (!dict_lock(d, true, &nitems)) {
        atomic_add(&d->wait_ct, 1);
        futex_wait_for_value(&d->futex, 0);
        atomic_add(&d->wait_ct, -1);
        return;
    }

    olds = atomic_load_acq(&d->store);

    uint32_t               alloc_items = new_dict_size(olds->last_slot, nitems);
    ncc_dict_store_t *news        = new_dict_store(alloc_items);
    uint32_t               last        = news->last_slot;
    uint32_t               bix;

    atomic_store_rel(&news->used_count, nitems);

    for (uint32_t i = 0; i <= olds->last_slot; i++) {
        bold = &olds->buckets[i];
        if (!bucket_reserved(bold) || bucket_deleted(bold)) {
            continue;
        }

        bix = bold->hv & last;

        for (uint32_t j = 0; j <= last; j++) {
            ncc_dict_bucket_t *bnew = &news->buckets[bix];

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

    base_dealloc(olds);
    dict_unlock_post_migrate(d, news);
}

static inline ncc_dict_bucket_t *
acquire_if_present(ncc_dict_t *d, ncc_dict_store_t *store, ncc_hash_t hv)
{
    uint32_t                last_slot;
    uint32_t                bix;
    uint32_t                flags;
    ncc_dict_bucket_t *cur;
    bool                    miss = false;

    do {
        last_slot = store->last_slot;
        bix       = hv & last_slot;

        for (uint32_t i = 0; i <= last_slot; i++) {
            cur = &store->buckets[bix];

            do {
                flags = atomic_or(&cur->flags, FLAG_MUTEX);
                if (flags & FLAG_MOVING) {
                    goto try_again;
                }
            } while (flags & FLAG_MUTEX);

            if (cur->hv == hv) {
                return cur;
            }
            if (!bucket_reserved(cur)) {
                miss = true;
            }

            bix = (bix + 1) & last_slot;
            atomic_and(&cur->flags, ~FLAG_MUTEX);

            if (miss) {
                return nullptr;
            }
        }

        return nullptr;

try_again:
        futex_wait_for_value(&d->futex, 0);
        store = atomic_load_acq(&d->store);
    } while (true);
}

static inline ncc_dict_bucket_t *
acquire_or_add(ncc_dict_t *d, ncc_dict_store_t *store, ncc_hash_t hv)
{
    uint32_t                last_slot;
    uint32_t                bix;
    uint32_t                flags;
    ncc_dict_bucket_t *cur;

    do {
        last_slot = store->last_slot;
        bix       = hv & last_slot;

        for (uint32_t i = 0; i <= last_slot; i++) {
            cur = &store->buckets[bix];

            do {
                flags = atomic_or(&cur->flags, FLAG_MUTEX);
                if (flags & FLAG_COPYING) {
                    goto try_again;
                }
            } while (flags & FLAG_MUTEX);

            if (cur->hv == hv || !bucket_reserved(cur)) {
                return cur;
            }

            bix = (bix + 1) & last_slot;
            atomic_and(&cur->flags, ~FLAG_MUTEX);
        }

        return nullptr;

try_again:
        futex_wait_for_value(&d->futex, 0);
        store = atomic_load_acq(&d->store);
    } while (true);
}

// Public API

ncc_hash_t
ncc_hash_ptr(void *p)
{
    // Mix the pointer bits for better distribution
    uint64_t x = (uint64_t)p;
    x          = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x          = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x          = x ^ (x >> 31);
    return (ncc_hash_t)x | 1; // Ensure non-zero
}

ncc_hash_t
ncc_hash_cstring(void *p)
{
    const char *s = p;
    if (!s) {
        return 1;
    }

    // FNV-1a hash
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return (ncc_hash_t)h | 1; // Ensure non-zero
}

void
ncc_dict_init(ncc_dict_t *d, uint32_t start_capacity, ncc_hash_fn hash_fn)
{
    if (start_capacity < NCC_DICT_MIN_SIZE) {
        start_capacity = NCC_DICT_MIN_SIZE;
    }

    // Round up to power of 2
    start_capacity--;
    start_capacity |= start_capacity >> 1;
    start_capacity |= start_capacity >> 2;
    start_capacity |= start_capacity >> 4;
    start_capacity |= start_capacity >> 8;
    start_capacity |= start_capacity >> 16;
    start_capacity++;

    *d = (ncc_dict_t){
        .hash_fn         = hash_fn,
        .insertion_epoch = 0,
        .wait_ct         = 0,
        .length          = 0,
        .futex           = 0,
    };

    atomic_store_rel(&d->store, new_dict_store(start_capacity));
}

void
ncc_dict_free(ncc_dict_t *d)
{
    ncc_dict_store_t *s = atomic_load_acq(&d->store);
    if (s) {
        base_dealloc(s);
        atomic_store_rel(&d->store, nullptr);
    }
}

void *
ncc_dict_put(ncc_dict_t *d, void *key, void *value)
{
    ncc_hash_t        hv     = compute_hash(d, key);
    ncc_dict_store_t *store  = atomic_load_acq(&d->store);
    void                  *result = nullptr;

try_again:;
    ncc_dict_bucket_t *bucket      = acquire_or_add(d, store, hv);
    bool                    reset_epoch = false;

    if (!bucket->hv) {
        if (atomic_add(&store->used_count, 1) >= store->threshold) {
            unlock_bucket(bucket);
            dict_migrate(d);
            store = atomic_load_acq(&d->store);
            goto try_again;
        }
        reset_epoch = true;
        bucket->hv  = hv;
    }
    else {
        if (bucket_deleted(bucket)) {
            reset_epoch = true;
            bucket->flags &= ~FLAG_DELETED;
        }
        else {
            result = bucket->value;
        }
    }

    if (reset_epoch) {
        bucket->insert_order = (uint32_t)atomic_add(&store->used_count, 1);
        atomic_add(&d->length, 1);
    }

    bucket->key   = key;
    bucket->value = value;
    unlock_bucket(bucket);

    return result;
}

void *
ncc_dict_get(ncc_dict_t *d, void *key, bool *found)
{
    ncc_hash_t         hv    = compute_hash(d, key);
    ncc_dict_store_t  *store = atomic_load_acq(&d->store);
    ncc_dict_bucket_t *b     = acquire_if_present(d, store, hv);
    void                   *result;

    if (!b) {
        if (found) {
            *found = false;
        }
        return nullptr;
    }

    if (bucket_deleted(b)) {
        if (found) {
            *found = false;
        }
        unlock_bucket(b);
        return nullptr;
    }

    if (found) {
        *found = true;
    }

    result = b->value;
    unlock_bucket(b);

    return result;
}

bool
ncc_dict_remove(ncc_dict_t *d, void *key)
{
    ncc_hash_t         hv    = compute_hash(d, key);
    ncc_dict_store_t  *store = atomic_load_acq(&d->store);
    ncc_dict_bucket_t *b     = acquire_if_present(d, store, hv);

    if (!b) {
        return false;
    }
    if (!bucket_reserved(b) || bucket_deleted(b)) {
        unlock_bucket(b);
        return false;
    }

    b->value = nullptr;
    b->flags |= FLAG_DELETED;
    atomic_add(&d->length, -1);
    unlock_bucket(b);

    return true;
}

