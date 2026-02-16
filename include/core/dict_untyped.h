#pragma once

#include <stdint.h>
#include "n00b.h"
#include "core/allocator.h"
#include "core/hash.h"

#define N00B_DICT_UNTYPED_MIN_SIZE_LOG 4
#define N00B_DICT_UNTYPED_MIN_SIZE     (1 << N00B_DICT_UNTYPED_MIN_SIZE_LOG)

typedef int (*n00b_dict_sort_fn)(const void *, const void *);

struct n00b_dict_untyped_bucket_t {
    __int128_t       hv;
    void            *key;
    void            *value;
    uint32_t         insert_order;
    _Atomic uint32_t flags;
};

struct n00b_dict_untyped_store_t {
    uint32_t                   last_slot;
    uint32_t                   threshold;
    _Atomic uint64_t           used_count;
    n00b_dict_untyped_bucket_t buckets[];
};

struct n00b_dict_untyped_t {
    n00b_hash_fn                         fn;
    n00b_allocator_t                    *allocator;
    _Atomic(n00b_dict_untyped_store_t *) store;
    _Atomic uint32_t                     insertion_epoch;
    _Atomic n00b_isize_t                 wait_ct; // Waiting on migration
    _Atomic n00b_isize_t                 length;  // Items in dict
    n00b_futex_t                         futex;
    uint32_t                             skip_obj_hash : 1;
};

struct n00b_dict_untyped_item_t {
    void    *key;
    void    *value;
    uint64_t order;
};

extern void *_n00b_dict_untyped_put(n00b_dict_untyped_t *d, void *key, void *value);
extern void *_n00b_dict_untyped_get(n00b_dict_untyped_t *d, void *key, bool *found);
extern bool  _n00b_dict_untyped_replace(n00b_dict_untyped_t *d, void *key, void *value);
extern bool  _n00b_dict_untyped_add(n00b_dict_untyped_t *d, void *key, void *value);
extern bool  _n00b_dict_untyped_remove(n00b_dict_untyped_t *d, void *key);
extern bool  _n00b_dict_untyped_cas(n00b_dict_untyped_t *d,
                                    void                *key,
                                    void               **old_item_ptr,
                                    void                *new_item,
                                    bool                 expect_empty,
                                    bool                 delete_existing);

#define n00b_dict_untyped_put(d, k, v)                                                         \
    _n00b_dict_untyped_put(d, ((void *)(int64_t)k), ((void *)(int64_t)v))
#define n00b_dict_untyped_get(d, k, b) _n00b_dict_untyped_get(d, ((void *)(int64_t)k), b)
#define n00b_dict_untyped_replace(d, k, v)                                                     \
    _n00b_dict_untyped_replace(d, ((void *)(int64_t)k), ((void *)(int64_t)v))
#define n00b_dict_untyped_add(d, k, v)                                                         \
    _n00b_dict_untyped_add(d, ((void *)(int64_t)k), ((void *)(int64_t)v))

#define n00b_dict_untyped_remove(d, k) _n00b_dict_untyped_remove(d, ((void *)(int64_t)k))

#define n00b_dict_untyped_cas(d, k, o, n, b1, b2)                                              \
    _n00b_dict_untyped_cas(d,                                                                  \
                           ((void *)(int64_t)k),                                               \
                           ((void *)(int64_t)o),                                               \
                           ((void *)(int64_t)n),                                               \
                           b1,                                                                 \
                           b2)

extern void
n00b_dict_untyped_init(n00b_dict_untyped_t *dict) _kargs
{
    uint32_t                start_capacity = N00B_DICT_MIN_SIZE;
    const n00b_allocator_t *allocator      = nullptr;
    n00b_hash_fn            hash           = nullptr;
    bool                    skip_obj_hash  = false;
};

static inline bool
n00b_dict_untyped_contains(n00b_dict_untyped_t *d, void *v)
{
    bool found;

    n00b_dict_untyped_get(d, v, &found);

    return found;
}

#ifdef N00B_USE_INTERNAL_API
extern bool n00b_dict_untyped_lock(n00b_dict_untyped_t *d, bool try, uint32_t *count);
extern void n00b_dict_untyped_unlock_post_copy(n00b_dict_untyped_t *d);

#define N00B_HT_FLAG_MUTEX   1
#define N00B_HT_FLAG_COPYING 2
#define N00B_HT_FLAG_DELETED 4
#define N00B_HT_FLAG_MOVING  8

#endif
