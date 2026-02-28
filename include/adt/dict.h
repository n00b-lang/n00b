#pragma once

#include "n00b.h"
#include "core/macros.h"
#include "core/alloc.h"
#include "core/hash.h"

#include <stdint.h>
#include <assert.h>

#if !defined(N00B_DICT_UNTYPED_MIN_SIZE_LOG)
#define N00B_DICT_UNTYPED_MIN_SIZE_LOG 4
#define N00B_DICT_UNTYPED_MIN_SIZE     (1 << N00B_DICT_UNTYPED_MIN_SIZE_LOG)
#endif

#if !defined(N00B_DICT_MIN_SIZE_LOG)
#define N00B_DICT_MIN_SIZE_LOG 4
#define N00B_DICT_MIN_SIZE     (1 << N00B_DICT_MIN_SIZE_LOG)
#endif

#define n00b_dict_store_tid(k, v) typeid("store", k, v)
#define n00b_dict_store_t(k, v)   struct n00b_dict_store_tid(k, v)

typedef struct n00b_dict_bucket_t {
    n00b_uint128_t   hv;
    uint32_t         insert_order;
    _Atomic uint32_t flags; // Mutex, deleted.
} n00b_dict_bucket_t;

#define n00b_dict_store_decl(k, v)                                                             \
    n00b_dict_store_t(k, v)                                                                    \
    {                                                                                          \
        uint32_t            last_slot;                                                         \
        uint32_t            threshold;                                                         \
        _Atomic uint32_t    used_count;                                                        \
        n00b_dict_bucket_t *buckets;                                                           \
        k *keys;                                                                               \
        v *values;                                                                             \
    }

typedef struct __n00b_internal_type_erased_store_t {
    uint32_t            last_slot;
    uint32_t            threshold;
    _Atomic uint32_t    used_count;
    n00b_dict_bucket_t *buckets;
    void              **keys;
    void              **values;
} __n00b_internal_type_erased_store_t;

#define n00b_dict_tid(k, v) typeid("dict", k, v)
#define n00b_dict_t(k, v)   struct n00b_dict_tid(k, v)

// The futex is only for migrations.
#define N00B_BASE_DICT_FIELDS                                                                  \
    n00b_hash_fn         fn;                                                                   \
    n00b_allocator_t    *allocator;                                                            \
    _Atomic uint32_t     insertion_epoch;                                                      \
    _Atomic n00b_isize_t wait_ct;                                                              \
    _Atomic n00b_isize_t length;                                                               \
    n00b_futex_t         futex;                                                                \
    uint8_t              cache         : 1;                                                    \
    uint8_t              lock          : 1;                                                    \
    uint8_t              skip_obj_hash : 1;

typedef struct _n00b_dict_internal_t {
    _Atomic(void **) store;
    N00B_BASE_DICT_FIELDS
} _n00b_dict_internal_t;

#define n00b_dict_decl_base(k, v)                                                              \
    n00b_dict_t(k, v)                                                                          \
    {                                                                                          \
        _Atomic(n00b_dict_store_t(k, v) *) store;                                              \
        N00B_BASE_DICT_FIELDS                                                                  \
    }

#define n00b_dict_decl(k, v)                                                                   \
    n00b_dict_store_decl(k, v);                                                                \
    n00b_dict_decl_base(k, v);                                                                 \
    static_assert(sizeof(k) <= (1 << 15));                                                     \
    static_assert(sizeof(v) <= (1 << 15));                                                     \
    static_assert(sizeof(n00b_dict_t(k, v)) == sizeof(_n00b_dict_internal_t))

// Structural type checking, ha!
#define _n00b_dict_structural_check(dict_ptr)                                                  \
    static_assert(sizeof(*(dict_ptr)) == sizeof(_n00b_dict_internal_t));                         \
    static_assert(offsetof(typeof(*(dict_ptr)), store)                                         \
                  == offsetof(_n00b_dict_internal_t, store));                                    \
    static_assert(offsetof(typeof(*(dict_ptr)), fn) == offsetof(_n00b_dict_internal_t, fn));     \
    static_assert(offsetof(typeof(*(dict_ptr)), allocator)                                     \
                  == offsetof(_n00b_dict_internal_t, allocator));                                \
    static_assert(offsetof(typeof(*(dict_ptr)), insertion_epoch)                               \
                  == offsetof(_n00b_dict_internal_t, insertion_epoch));                          \
    static_assert(offsetof(typeof(*(dict_ptr)), wait_ct)                                       \
                  == offsetof(_n00b_dict_internal_t, wait_ct));                                  \
    static_assert(offsetof(typeof(*(dict_ptr)), length)                                        \
                  == offsetof(_n00b_dict_internal_t, length));                                   \
    static_assert(offsetof(typeof(*(dict_ptr)), futex) == offsetof(_n00b_dict_internal_t, futex))

#define _n00b_wrap_dict_call(opname, dict_ptr, ...)                                            \
    ({                                                                                         \
        _n00b_dict_structural_check(dict_ptr);                                                 \
        _n00b_dict_internal_##opname((_n00b_dict_internal_t *)(dict_ptr),                      \
                                     sizeof((dict_ptr)->store->keys[0]),                       \
                                     sizeof((dict_ptr)->store->values[0])                      \
                                         __VA_OPT__(, __VA_ARGS__));                           \
    })

#define _n00b_ditem_type(dict_ptr, field_name) typeof((dict_ptr)->store->field_name[0])
#define n00b_dict_init(dict, ...) _n00b_wrap_dict_call(init, dict __VA_OPT__(, __VA_ARGS__))

#define n00b_dict_put(dict, key, value) _n00b_wrap_dict_call(put, dict, &(key), &(value))
#define n00b_dict_get(dict, key, found)                                                        \
    ({                                                                                         \
        void *_dg_vp = _n00b_wrap_dict_call(get, dict, &(key), found);                         \
        _n00b_ditem_type(dict, values) _dg_zero = (_n00b_ditem_type(dict, values)){0};         \
        _dg_vp ? *(_n00b_ditem_type(dict, values) *)_dg_vp : _dg_zero;                        \
    })
#define n00b_dict_add(dict, key, value) _n00b_wrap_dict_call(add, dict, &(key), &(value))
#define n00b_dict_remove(dict, key)     _n00b_wrap_dict_call(remove, dict, &(key))
#define n00b_dict_cas(dict, key, old, new, ...)                                                \
    _n00b_wrap_dict_call(cas, dict, &(key), old, new __VA_OPT__(, __VA_ARGS__))

#define n00b_dict_contains(dict, key)                                                          \
    ({                                                                                         \
        bool found;                                                                            \
        (void)n00b_dict_get(dict, key, &found);                                                \
        found;                                                                                 \
    })

/**
 * @brief Iterate over all live entries in a typed dictionary.
 *
 * @param dict   Pointer to the typed dict.
 * @param kvar   Variable name to bind each key.
 * @param vvar   Variable name to bind each value.
 * @param body   Statement or block to execute per entry.
 *
 * Example:
 *     n00b_dict_foreach(my_dict, k, v, {
 *         printf("key=%lld val=%d\n", (long long)k, v);
 *     });
 */
#define n00b_dict_foreach(dict, kvar, vvar, body)                                                  \
    do {                                                                                           \
        _n00b_dict_structural_check(dict);                                                         \
        _n00b_dict_internal_t *_df_d                                                               \
            = (_n00b_dict_internal_t *)(dict);                                                     \
        __n00b_internal_type_erased_store_t *_df_s                                                 \
            = (__n00b_internal_type_erased_store_t *)atomic_load_explicit(                          \
                  &_df_d->store, memory_order_acquire);                                            \
        if (_df_s) {                                                                               \
            typedef _n00b_ditem_type(dict, keys)   _df_kt;                                         \
            typedef _n00b_ditem_type(dict, values)  _df_vt;                                        \
            _df_kt *_df_keys = (_df_kt *)_df_s->keys;                                              \
            _df_vt *_df_vals = (_df_vt *)_df_s->values;                                            \
            for (uint32_t _df_i = 0; _df_i <= _df_s->last_slot; _df_i++) {                         \
                n00b_dict_bucket_t *_df_b = &_df_s->buckets[_df_i];                                \
                if (_df_b->hv != (n00b_uint128_t)0                                                 \
                    && !(atomic_load_explicit(&_df_b->flags, memory_order_relaxed)                  \
                         & N00B_HT_FLAG_DELETED)) {                                                \
                    _df_kt kvar = _df_keys[_df_i];                                                 \
                    _df_vt vvar = _df_vals[_df_i];                                                 \
                    (void)vvar;                                                                    \
                    body                                                                           \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
    } while (0)

// This private interface (from here, down) is untyped, and thus internal.
extern void       _n00b_finalize_dict(_n00b_dict_internal_t *);
extern n00b_size_t n00b_dict_internal_len(_n00b_dict_internal_t *);

/**
 * @brief Initialize an untyped dictionary.
 * @param dict Dictionary to initialize.
 *
 * @kw start_capacity Initial bucket count (default N00B_DICT_MIN_SIZE).
 * @kw allocator      Allocator for internal storage (nullptr = runtime default).
 * @kw hash           Hash function for keys (nullptr = n00b_hash_word).
 * @kw skip_obj_hash  If true, use the raw key bits instead of calling the hash function.
 */
extern void
_n00b_dict_internal_init(_n00b_dict_internal_t *, uint16_t ksz, uint16_t vsz) _kargs
{
    n00b_allocator_t *allocator      = nullptr;
    uint32_t          start_capacity = N00B_DICT_MIN_SIZE;
    n00b_hash_fn      hash           = nullptr;
    bool              skip_obj_hash  = false;
};

extern void *_n00b_dict_internal_put(_n00b_dict_internal_t *d,
                                     uint32_t               ksz,
                                     uint32_t               vsz,
                                     void                  *key,
                                     void                  *value);
extern void *_n00b_dict_internal_get(_n00b_dict_internal_t *d,
                                     uint32_t               ksz,
                                     uint32_t               vsz,
                                     void                  *key,
                                     bool                  *found);
extern bool  _n00b_dict_internal_add(_n00b_dict_internal_t *d,
                                     uint32_t               ksz,
                                     uint32_t               vsz,
                                     void                  *key,
                                     void                  *value);
extern bool
_n00b_dict_internal_remove(_n00b_dict_internal_t *d, uint32_t ksz, uint32_t vsz, void *key);

extern bool
_n00b_dict_internal_cas(_n00b_dict_internal_t *d,
                        uint32_t               ksz,
                        uint32_t               vsz,
                        void                  *key,
                        void                 **old_item_ptr,
                        void                  *new_item) _kargs
{
    bool null_old_means_absence = false;
    bool null_new_means_delete  = false;
};

#define N00B_HT_FLAG_MUTEX   1
#define N00B_HT_FLAG_COPYING 2
#define N00B_HT_FLAG_DELETED 4
#define N00B_HT_FLAG_MOVING  8

#ifdef N00B_USE_INTERNAL_API
/**
 * @brief Acquire the dictionary's migration mutex.
 * @param d     Dictionary to lock.
 * @param try   If true, return immediately on failure.
 * @param count Output: migration epoch when lock was acquired.
 * @return      true if lock was acquired.
 */
extern bool n00b_dict_internal_lock(_n00b_dict_internal_t *d,
                                    bool                   try,
                                    uint32_t              *count);

/** @brief Unlock the dictionary after a store migration. */
extern void n00b_dict_internal_unlock_post_copy(_n00b_dict_internal_t *d);

#endif
