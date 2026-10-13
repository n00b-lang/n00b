#pragma once

#include <stdint.h>
#include <string.h>
#include "core.h"
#include "lib/alloc.h"
#include "lib/hash.h"

#define CORE_DICT_MIN_SIZE_LOG 4
#define CORE_DICT_MIN_SIZE     (1 << CORE_DICT_MIN_SIZE_LOG)

#define _CORE_DICT_LOAD_NUM 3
#define _CORE_DICT_LOAD_DEN 4

#define _CORE_BUCKET_EMPTY    0
#define _CORE_BUCKET_OCCUPIED 1
#define _CORE_BUCKET_DELETED  2

typedef struct {
    core_hash_value_t hv;
    uint32_t         state;
} core_dict_bucket_t;

typedef bool (*core_dict_key_eq_fn)(const void *, const void *);

struct core_dict_t {
    core_hash_fn         hash_fn;
    core_dict_key_eq_fn  key_eq;
    core_dict_bucket_t  *buckets;
    void              **keys;
    void              **values;
    size_t               capacity;
    size_t               count;
    size_t               tombstones;
};

typedef struct {
    void *key;
    void *value;
} core_dict_item_t;

static inline size_t
_core_dict_mask(const core_dict_t *d)
{
    return d->capacity - 1;
}

static inline size_t
_core_dict_probe(core_hash_value_t hv, size_t mask)
{
    return (size_t)hv & mask;
}

static inline void _core_dict_resize(core_dict_t *d, size_t new_cap);

static inline void
_core_dict_maybe_resize(core_dict_t *d)
{
    if ((d->count + d->tombstones) * _CORE_DICT_LOAD_DEN
        > d->capacity * _CORE_DICT_LOAD_NUM) {
        size_t new_cap = d->capacity;
        if (d->count * _CORE_DICT_LOAD_DEN > d->capacity * _CORE_DICT_LOAD_NUM) {
            new_cap = d->capacity * 2;
        }
        _core_dict_resize(d, new_cap);
    }
}

static inline void
_core_dict_resize(core_dict_t *d, size_t new_cap)
{
    core_dict_bucket_t *old_buckets = d->buckets;
    void             **old_keys    = d->keys;
    void             **old_values  = d->values;
    size_t              old_cap     = d->capacity;

    d->buckets    = core_alloc_array(core_dict_bucket_t, new_cap);
    d->keys       = core_alloc_array(void *, new_cap);
    d->values     = core_alloc_array(void *, new_cap);
    d->capacity   = new_cap;
    d->count      = 0;
    d->tombstones = 0;

    size_t mask = _core_dict_mask(d);

    for (size_t i = 0; i < old_cap; i++) {
        if (old_buckets[i].state != _CORE_BUCKET_OCCUPIED) {
            continue;
        }
        core_hash_value_t hv  = old_buckets[i].hv;
        size_t            idx = _core_dict_probe(hv, mask);

        while (d->buckets[idx].state == _CORE_BUCKET_OCCUPIED) {
            idx = (idx + 1) & mask;
        }

        d->buckets[idx].hv    = hv;
        d->buckets[idx].state = _CORE_BUCKET_OCCUPIED;
        d->keys[idx]          = old_keys[i];
        d->values[idx]        = old_values[i];
        d->count++;
    }

    core_free(old_buckets);
    core_free(old_keys);
    core_free(old_values);
}

static inline bool
_core_dict_ptr_eq(const void *a, const void *b)
{
    return a == b;
}

static inline bool
core_dict_cstr_eq(const void *a, const void *b)
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
core_dict_init(core_dict_t *dict,
                       core_hash_fn         hash_fn,
                       core_dict_key_eq_fn  key_eq)
{
    dict->hash_fn    = hash_fn ? hash_fn : core_hash_word;
    dict->key_eq     = key_eq  ? key_eq  : _core_dict_ptr_eq;
    dict->capacity   = CORE_DICT_MIN_SIZE;
    dict->count      = 0;
    dict->tombstones = 0;
    dict->buckets    = core_alloc_array(core_dict_bucket_t, CORE_DICT_MIN_SIZE);
    dict->keys       = core_alloc_array(void *, CORE_DICT_MIN_SIZE);
    dict->values     = core_alloc_array(void *, CORE_DICT_MIN_SIZE);
}

static inline core_hash_value_t
_core_dict_string_hash(void *key)
{
    return core_string_hash(*(const core_string_t *)key);
}

static inline bool
_core_dict_string_eq(const void *a, const void *b)
{
    return core_string_eq(*(const core_string_t *)a,
                          *(const core_string_t *)b);
}

static inline void
core_dict_init_string(core_dict_t *dict)
{
    core_dict_init(dict, _core_dict_string_hash, _core_dict_string_eq);
}

static inline core_string_t *
core_dict_string_key_copy(core_string_t value)
{
    core_string_t *key = core_alloc(core_string_t);
    *key = core_string_dup(value);
    return key;
}

static inline void
core_dict_string_key_free(void *ptr)
{
    core_string_t *key = ptr;
    if (!key) return;
    core_free(key->data);
    core_free(key);
}

static inline size_t
_core_dict_find(core_dict_t *d, core_hash_value_t hv, void *key)
{
    size_t mask = _core_dict_mask(d);
    size_t idx  = _core_dict_probe(hv, mask);

    while (true) {
        if (d->buckets[idx].state == _CORE_BUCKET_EMPTY) {
            return (size_t)-1;
        }

        if (d->buckets[idx].state == _CORE_BUCKET_OCCUPIED
            && d->buckets[idx].hv == hv
            && d->key_eq(d->keys[idx], key)) {
            return idx;
        }

        idx = (idx + 1) & mask;
    }
}

static inline void *
_core_dict_put_hashed(core_dict_t *d, core_hash_value_t hv, void *key,
                     void *value)
{
    _core_dict_maybe_resize(d);

    size_t mask          = _core_dict_mask(d);
    size_t idx           = _core_dict_probe(hv, mask);
    size_t first_deleted = (size_t)-1;

    while (true) {
        if (d->buckets[idx].state == _CORE_BUCKET_EMPTY) {
            size_t ins = (first_deleted != (size_t)-1) ? first_deleted : idx;
            if (d->buckets[ins].state == _CORE_BUCKET_DELETED) {
                d->tombstones--;
            }
            d->buckets[ins].hv    = hv;
            d->buckets[ins].state = _CORE_BUCKET_OCCUPIED;
            d->keys[ins]          = key;
            d->values[ins]        = value;
            d->count++;
            return nullptr;
        }

        if (d->buckets[idx].state == _CORE_BUCKET_DELETED) {
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
_core_dict_put(core_dict_t *d, void *key, void *value)
{
    return _core_dict_put_hashed(d, d->hash_fn(key), key, value);
}

static inline void *
_core_dict_get(core_dict_t *d, void *key, bool *found)
{
    size_t idx = _core_dict_find(d, d->hash_fn(key), key);

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
_core_dict_remove(core_dict_t *d, void *key)
{
    size_t idx = _core_dict_find(d, d->hash_fn(key), key);

    if (idx == (size_t)-1) {
        return false;
    }

    d->buckets[idx].state = _CORE_BUCKET_DELETED;
    d->count--;
    d->tombstones++;

    return true;
}

static inline bool
_core_dict_add(core_dict_t *d, void *key, void *value)
{
    core_hash_value_t hv = d->hash_fn(key);

    if (_core_dict_find(d, hv, key) != (size_t)-1) {
        return false;
    }

    _core_dict_put_hashed(d, hv, key, value);

    return true;
}

static inline bool
_core_dict_replace(core_dict_t *d, void *key, void *value)
{
    size_t idx = _core_dict_find(d, d->hash_fn(key), key);

    if (idx == (size_t)-1) {
        return false;
    }

    d->values[idx] = value;

    return true;
}

#define core_dict_put(d, k, v) \
    _core_dict_put(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define core_dict_get(d, k, b) \
    _core_dict_get(d, ((void *)(uintptr_t)(k)), (b))
#define core_dict_replace(d, k, v) \
    _core_dict_replace(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define core_dict_add(d, k, v) \
    _core_dict_add(d, ((void *)(uintptr_t)(k)), ((void *)(uintptr_t)(v)))
#define core_dict_remove(d, k) \
    _core_dict_remove(d, ((void *)(uintptr_t)(k)))

static inline bool
core_dict_contains(core_dict_t *d, void *v)
{
    bool found;
    core_dict_get(d, v, &found);
    return found;
}

static inline void
core_dict_free(core_dict_t *d)
{
    core_free(d->buckets);
    core_free(d->keys);
    core_free(d->values);
    d->buckets  = nullptr;
    d->keys     = nullptr;
    d->values   = nullptr;
    d->capacity = 0;
    d->count    = 0;
}

static inline size_t
core_dict_len(core_dict_t *d)
{
    return d->count;
}

#define core_dict_foreach(d, var)                                              \
    for (size_t _core_di = 0; _core_di < (d)->capacity; _core_di++)              \
        if ((d)->buckets[_core_di].state != _CORE_BUCKET_OCCUPIED) { }          \
        else                                                                  \
            for (core_dict_item_t _core_it = {.key   = (d)->keys[_core_di],      \
                                            .value = (d)->values[_core_di]},   \
                 *var = &_core_it;                                             \
                 var; var = nullptr)
