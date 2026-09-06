#pragma once

#include <stdint.h>
#include <string.h>
#include "ncc.h"
#include "lib/alloc.h"
#include "lib/hash.h"

#define NCC_DICT_MIN_SIZE_LOG 4
#define NCC_DICT_MIN_SIZE     (1 << NCC_DICT_MIN_SIZE_LOG)

#define _NCC_DICT_LOAD_NUM 3
#define _NCC_DICT_LOAD_DEN 4

#define _NCC_BUCKET_EMPTY    0
#define _NCC_BUCKET_OCCUPIED 1
#define _NCC_BUCKET_DELETED  2

typedef struct {
    ncc_hash_value_t hv;
    uint32_t         state;
} ncc_dict_bucket_t;

typedef bool (*ncc_dict_key_eq_fn)(const void *, const void *);

struct ncc_dict_t {
    ncc_hash_fn         hash_fn;
    ncc_dict_key_eq_fn  key_eq;
    ncc_dict_bucket_t  *buckets;
    void              **keys;
    void              **values;
    size_t               capacity;
    size_t               count;
    size_t               tombstones;
};

typedef struct {
    void *key;
    void *value;
} ncc_dict_item_t;

static inline size_t
_ncc_dict_mask(const ncc_dict_t *d)
{
    return d->capacity - 1;
}

static inline size_t
_ncc_dict_probe(ncc_hash_value_t hv, size_t mask)
{
    return (size_t)hv & mask;
}

static inline void _ncc_dict_resize(ncc_dict_t *d, size_t new_cap);

static inline void
_ncc_dict_maybe_resize(ncc_dict_t *d)
{
    if ((d->count + d->tombstones) * _NCC_DICT_LOAD_DEN
        > d->capacity * _NCC_DICT_LOAD_NUM) {
        size_t new_cap = d->capacity;
        if (d->count * _NCC_DICT_LOAD_DEN > d->capacity * _NCC_DICT_LOAD_NUM) {
            new_cap = d->capacity * 2;
        }
        _ncc_dict_resize(d, new_cap);
    }
}

static inline void
_ncc_dict_resize(ncc_dict_t *d, size_t new_cap)
{
    ncc_dict_bucket_t *old_buckets = d->buckets;
    void             **old_keys    = d->keys;
    void             **old_values  = d->values;
    size_t              old_cap     = d->capacity;

    d->buckets    = ncc_alloc_array(ncc_dict_bucket_t, new_cap);
    d->keys       = ncc_alloc_array(void *, new_cap);
    d->values     = ncc_alloc_array(void *, new_cap);
    d->capacity   = new_cap;
    d->count      = 0;
    d->tombstones = 0;

    size_t mask = _ncc_dict_mask(d);

    for (size_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].state != _NCC_BUCKET_OCCUPIED) {
            continue;
        }
        ncc_hash_value_t hv  = old_buckets[i].hv;
        size_t            idx = _ncc_dict_probe(hv, mask);

        while (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED) {
            idx = (idx + 1) & mask;
        }

        d->buckets[idx].hv    = hv;
        d->buckets[idx].state = _NCC_BUCKET_OCCUPIED;
        d->keys[idx]          = old_keys[i];
        d->values[idx]        = old_values[i];
        d->count++;
    }

    ncc_free(old_buckets);
    ncc_free(old_keys);
    ncc_free(old_values);
}

static inline bool
_ncc_dict_ptr_eq(const void *a, const void *b)
{
    return a == b;
}

static inline bool
ncc_dict_cstr_eq(const void *a, const void *b)
{
    if (a == b) {
        return true;
    }
    if (!a || !b) {
        return false;
    }
    return strcmp((const char *)a, (const char *)b) == 0;
}

static inline void
ncc_dict_init(ncc_dict_t *dict,
                       ncc_hash_fn         hash_fn,
                       ncc_dict_key_eq_fn  key_eq)
{
    dict->hash_fn    = hash_fn ? hash_fn : ncc_hash_word;
    dict->key_eq     = key_eq  ? key_eq  : _ncc_dict_ptr_eq;
    dict->capacity   = NCC_DICT_MIN_SIZE;
    dict->count      = 0;
    dict->tombstones = 0;
    dict->buckets    = ncc_alloc_array(ncc_dict_bucket_t, NCC_DICT_MIN_SIZE);
    dict->keys       = ncc_alloc_array(void *, NCC_DICT_MIN_SIZE);
    dict->values     = ncc_alloc_array(void *, NCC_DICT_MIN_SIZE);
}

static inline size_t
_ncc_dict_find(ncc_dict_t *d, ncc_hash_value_t hv, void *key)
{
    size_t mask = _ncc_dict_mask(d);
    size_t idx  = _ncc_dict_probe(hv, mask);

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            return (size_t)-1;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_OCCUPIED
            && d->buckets[idx].hv == hv
            && d->key_eq(d->keys[idx], key)) {
            return idx;
        }

        idx = (idx + 1) & mask;
    }
}

static inline void *
_ncc_dict_put_hashed(ncc_dict_t *d, ncc_hash_value_t hv, void *key,
                     void *value)
{
    _ncc_dict_maybe_resize(d);

    size_t mask          = _ncc_dict_mask(d);
    size_t idx           = _ncc_dict_probe(hv, mask);
    size_t first_deleted = (size_t)-1;

    while (true) {
        if (d->buckets[idx].state == _NCC_BUCKET_EMPTY) {
            size_t ins = (first_deleted != (size_t)-1) ? first_deleted : idx;
            if (d->buckets[ins].state == _NCC_BUCKET_DELETED) {
                d->tombstones--;
            }
            d->buckets[ins].hv    = hv;
            d->buckets[ins].state = _NCC_BUCKET_OCCUPIED;
            d->keys[ins]          = key;
            d->values[ins]        = value;
            d->count++;
            return nullptr;
        }

        if (d->buckets[idx].state == _NCC_BUCKET_DELETED) {
            if (first_deleted == (size_t)-1) {
                first_deleted = idx;
            }
        }
        else if (d->buckets[idx].hv == hv
                 && d->key_eq(d->keys[idx], key)) {
            void *old      = d->values[idx];
            d->values[idx] = value;
            return old;
        }

        idx = (idx + 1) & mask;
    }
}

static inline void *
_ncc_dict_put(ncc_dict_t *d, void *key, void *value)
{
    return _ncc_dict_put_hashed(d, d->hash_fn(key), key, value);
}

static inline void *
_ncc_dict_get(ncc_dict_t *d, void *key, bool *found)
{
    size_t idx = _ncc_dict_find(d, d->hash_fn(key), key);

    if (idx == (size_t)-1) {
        if (found) {
            *found = false;
        }
        return nullptr;
    }

    if (found) {
        *found = true;
    }

    return d->values[idx];
}

static inline bool
_ncc_dict_remove(ncc_dict_t *d, void *key)
{
    size_t idx = _ncc_dict_find(d, d->hash_fn(key), key);

    if (idx == (size_t)-1) {
        return false;
    }

    d->buckets[idx].state = _NCC_BUCKET_DELETED;
    d->count--;
    d->tombstones++;

    return true;
}

static inline bool
_ncc_dict_add(ncc_dict_t *d, void *key, void *value)
{
    ncc_hash_value_t hv = d->hash_fn(key);

    if (_ncc_dict_find(d, hv, key) != (size_t)-1) {
        return false;
    }

    _ncc_dict_put_hashed(d, hv, key, value);

    return true;
}

static inline bool
_ncc_dict_replace(ncc_dict_t *d, void *key, void *value)
{
    size_t idx = _ncc_dict_find(d, d->hash_fn(key), key);

    if (idx == (size_t)-1) {
        return false;
    }

    d->values[idx] = value;

    return true;
}

#define ncc_dict_put(d, k, v) \
    _ncc_dict_put(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_get(d, k, b) \
    _ncc_dict_get(d, ((void *)(uintptr_t)(k)), (b))
#define ncc_dict_replace(d, k, v) \
    _ncc_dict_replace(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_add(d, k, v) \
    _ncc_dict_add(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define ncc_dict_remove(d, k) \
    _ncc_dict_remove(d, ((void *)(uintptr_t)(k)))

static inline bool
ncc_dict_contains(ncc_dict_t *d, void *v)
{
    bool found;
    ncc_dict_get(d, v, &found);
    return found;
}

static inline void
ncc_dict_free(ncc_dict_t *d)
{
    ncc_free(d->buckets);
    ncc_free(d->keys);
    ncc_free(d->values);
    d->buckets  = nullptr;
    d->keys     = nullptr;
    d->values   = nullptr;
    d->capacity = 0;
    d->count    = 0;
}

static inline size_t
ncc_dict_len(ncc_dict_t *d)
{
    return d->count;
}

#define ncc_dict_foreach(d, var)                                              \
    for (size_t _ncc_di = 0; _ncc_di < (d)->capacity; _ncc_di++)              \
        if ((d)->buckets[_ncc_di].state != _NCC_BUCKET_OCCUPIED) { }          \
        else                                                                  \
            for (ncc_dict_item_t _ncc_it = {.key   = (d)->keys[_ncc_di],      \
                                            .value = (d)->values[_ncc_di]},   \
                 *var = &_ncc_it;                                             \
                 var; var = nullptr)
