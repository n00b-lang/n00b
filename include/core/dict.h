/**
 * @file locking_dict_typed.h
 * @brief Type-safe wrapper for base_ldict_t.
 *
 * Provides @c ldict_t(K, V) — a thread-safe concurrent dictionary with
 * compile-time type safety. Uses typeid() for unique struct names and
 * auto-boxing for types that don't fit in a @c void*.
 *
 * Requires ncc (uses typeid()).
 *
 * Usage:
 * @code
 *     #include <base/locking_dict_typed.h>
 *
 *     BASE_LDICT_IMPL(int, int, my_int_hash)
 *
 *     ldict_t(int, int) d = ldict_new(int, int);
 *     ldict_init(int, int, &d, 0);
 *     ldict_put(int, int, &d, 42, 100);
 *     int val;
 *     if (ldict_get(int, int, &d, 42, &val)) { ... }
 *     ldict_free(int, int, &d);
 * @endcode
 */
#pragma once

#include "locking_dict.h"
#include "generic.h"
#include "alloc.h"
#include <string.h>

/**
 * @brief Declare a typed locking dictionary for key type @p K and value type @p V.
 * @param K  Key type.
 * @param V  Value type.
 */
#define ldict_t(K, V) struct typeid("base_ldict_", K, "_", V)

/** @internal Function name generators for typed ldict operations. */
#define _LDICT_BOX_K(K, V)       typeid("_ldict_box_k_", K, "_", V)
/** @internal */
#define _LDICT_UNBOX_K(K, V)     typeid("_ldict_unbox_k_", K, "_", V)
/** @internal */
#define _LDICT_BOX_V(K, V)       typeid("_ldict_box_v_", K, "_", V)
/** @internal */
#define _LDICT_UNBOX_V(K, V)     typeid("_ldict_unbox_v_", K, "_", V)
/** @internal */
#define _LDICT_FREE_K(K, V)      typeid("_ldict_free_k_", K, "_", V)
/** @internal */
#define _LDICT_FREE_V(K, V)      typeid("_ldict_free_v_", K, "_", V)
/** @internal */
#define _LDICT_HASH_WRAP(K, V)   typeid("_ldict_hash_wrap_", K, "_", V)
/** @internal */
#define _LDICT_INIT(K, V)        typeid("_ldict_init_", K, "_", V)
/** @internal */
#define _LDICT_FREE(K, V)        typeid("_ldict_free_", K, "_", V)
/** @internal */
#define _LDICT_PUT(K, V)         typeid("_ldict_put_", K, "_", V)
/** @internal */
#define _LDICT_GET(K, V)         typeid("_ldict_get_", K, "_", V)
/** @internal */
#define _LDICT_REMOVE(K, V)      typeid("_ldict_remove_", K, "_", V)
/** @internal */
#define _LDICT_CONTAINS(K, V)    typeid("_ldict_contains_", K, "_", V)
/** @internal */
#define _LDICT_REPLACE(K, V)     typeid("_ldict_replace_", K, "_", V)
/** @internal */
#define _LDICT_ADD(K, V)         typeid("_ldict_add_", K, "_", V)

/**
 * @brief Define a typed locking dictionary for key type @p K and value type @p V.
 *
 * Generates the struct definition and all inline helper functions
 * (init, put, get, replace, add, remove, contains, free).
 *
 * Auto-boxing:
 *   - @c sizeof(T) <= @c sizeof(void*): value packed into @c void* via memcpy.
 *   - @c sizeof(T) > @c sizeof(void*): heap-allocated, pointer stored.
 *
 * @param K        Key type.
 * @param V        Value type.
 * @param hash_fn  Hash function: @c base_ldict_hash_t hash_fn(K key).
 */
#define BASE_LDICT_IMPL(K, V, hash_fn)                                                              \
    ldict_t(K, V)                                                                                    \
    {                                                                                                \
        base_ldict_t inner;                                                                          \
    };                                                                                               \
                                                                                                     \
    /* Box key: pack small values into void*, heap-alloc large ones */                               \
    static inline void *_LDICT_BOX_K(K, V)(K key)                                                   \
    {                                                                                                \
        void *raw = nullptr;                                                                         \
        if (sizeof(K) <= sizeof(void *)) {                                                 \
            memcpy(&raw, &key, sizeof(K));                                                           \
        }                                                                                            \
        else {                                                                                       \
            raw = base_alloc(sizeof(K));                                                             \
            memcpy(raw, &key, sizeof(K));                                                            \
        }                                                                                            \
        return raw;                                                                                  \
    }                                                                                                \
                                                                                                     \
    /* Unbox key */                                                                                  \
    static inline K _LDICT_UNBOX_K(K, V)(void *raw)                                                 \
    {                                                                                                \
        K key;                                                                                       \
        if (sizeof(K) <= sizeof(void *)) {                                                 \
            memcpy(&key, &raw, sizeof(K));                                                           \
        }                                                                                            \
        else {                                                                                       \
            memcpy(&key, raw, sizeof(K));                                                            \
        }                                                                                            \
        return key;                                                                                  \
    }                                                                                                \
                                                                                                     \
    /* Box value */                                                                                  \
    static inline void *_LDICT_BOX_V(K, V)(V val)                                                   \
    {                                                                                                \
        void *raw = nullptr;                                                                         \
        if (sizeof(V) <= sizeof(void *)) {                                                 \
            memcpy(&raw, &val, sizeof(V));                                                           \
        }                                                                                            \
        else {                                                                                       \
            raw = base_alloc(sizeof(V));                                                             \
            memcpy(raw, &val, sizeof(V));                                                            \
        }                                                                                            \
        return raw;                                                                                  \
    }                                                                                                \
                                                                                                     \
    /* Unbox value */                                                                                \
    static inline V _LDICT_UNBOX_V(K, V)(void *raw)                                                 \
    {                                                                                                \
        V val;                                                                                       \
        if (sizeof(V) <= sizeof(void *)) {                                                 \
            memcpy(&val, &raw, sizeof(V));                                                           \
        }                                                                                            \
        else {                                                                                       \
            memcpy(&val, raw, sizeof(V));                                                            \
        }                                                                                            \
        return val;                                                                                  \
    }                                                                                                \
                                                                                                     \
    /* Free boxed key (no-op if inline) */                                                           \
    static inline void _LDICT_FREE_K(K, V)(void *raw)                                               \
    {                                                                                                \
        if (sizeof(K) > sizeof(void *)) {                                                  \
            base_dealloc(raw);                                                                       \
        }                                                                                            \
    }                                                                                                \
                                                                                                     \
    /* Free boxed value (no-op if inline) */                                                         \
    static inline void _LDICT_FREE_V(K, V)(void *raw)                                               \
    {                                                                                                \
        if (sizeof(V) > sizeof(void *)) {                                                  \
            base_dealloc(raw);                                                                       \
        }                                                                                            \
    }                                                                                                \
                                                                                                     \
    /* Hash wrapper: unbox key, call user hash_fn, ensure non-zero */                                \
    static inline base_ldict_hash_t _LDICT_HASH_WRAP(K, V)(void *p)                                 \
    {                                                                                                \
        K key = _LDICT_UNBOX_K(K, V)(p);                                                            \
        return (base_ldict_hash_t)hash_fn(key) | 1;                                                 \
    }                                                                                                \
                                                                                                     \
    /* Init */                                                                                       \
    static inline void _LDICT_INIT(K, V)(ldict_t(K, V) * d, uint32_t cap)                           \
    {                                                                                                \
        base_ldict_init(&d->inner, cap, _LDICT_HASH_WRAP(K, V));                                    \
    }                                                                                                \
                                                                                                     \
    /* Put: box key+val, call base_ldict_put, free old boxed value */                                \
    static inline void _LDICT_PUT(K, V)(ldict_t(K, V) * d, K key, V val)                            \
    {                                                                                                \
        void *bk  = _LDICT_BOX_K(K, V)(key);                                                        \
        void *bv  = _LDICT_BOX_V(K, V)(val);                                                        \
        void *old = base_ldict_put(&d->inner, bk, bv);                                              \
        if (old) {                                                                                   \
            _LDICT_FREE_V(K, V)(old);                                                                \
        }                                                                                            \
    }                                                                                                \
                                                                                                     \
    /* Get: returns bool, writes to *out */                                                          \
    static inline bool _LDICT_GET(K, V)(ldict_t(K, V) * d, K key, V * out)                          \
    {                                                                                                \
        void *bk = _LDICT_BOX_K(K, V)(key);                                                         \
        bool  found;                                                                                 \
        void *raw = base_ldict_get(&d->inner, bk, &found);                                          \
        _LDICT_FREE_K(K, V)(bk);                                                                    \
        if (found && out) {                                                                          \
            *out = _LDICT_UNBOX_V(K, V)(raw);                                                       \
        }                                                                                            \
        return found;                                                                                \
    }                                                                                                \
                                                                                                     \
    /* Replace: returns true if key existed */                                                       \
    static inline bool _LDICT_REPLACE(K, V)(ldict_t(K, V) * d, K key, V val)                        \
    {                                                                                                \
        void *bk = _LDICT_BOX_K(K, V)(key);                                                         \
        void *bv = _LDICT_BOX_V(K, V)(val);                                                         \
        bool  ok = base_ldict_replace(&d->inner, bk, bv);                                           \
        _LDICT_FREE_K(K, V)(bk);                                                                    \
        if (!ok) {                                                                                   \
            _LDICT_FREE_V(K, V)(bv);                                                                \
        }                                                                                            \
        return ok;                                                                                   \
    }                                                                                                \
                                                                                                     \
    /* Add: returns true if key was new */                                                           \
    static inline bool _LDICT_ADD(K, V)(ldict_t(K, V) * d, K key, V val)                            \
    {                                                                                                \
        void *bk = _LDICT_BOX_K(K, V)(key);                                                         \
        void *bv = _LDICT_BOX_V(K, V)(val);                                                         \
        bool  ok = base_ldict_add(&d->inner, bk, bv);                                               \
        if (!ok) {                                                                                   \
            _LDICT_FREE_K(K, V)(bk);                                                                \
            _LDICT_FREE_V(K, V)(bv);                                                                \
        }                                                                                            \
        return ok;                                                                                   \
    }                                                                                                \
                                                                                                     \
    /* Remove: frees boxed key+value if heap-allocated */                                            \
    static inline bool _LDICT_REMOVE(K, V)(ldict_t(K, V) * d, K key)                                \
    {                                                                                                \
        void *bk = _LDICT_BOX_K(K, V)(key);                                                         \
        /* Need to retrieve boxed key/value before removing */                                       \
        bool  found;                                                                                 \
        void *raw_val = base_ldict_get(&d->inner, bk, &found);                                      \
        if (!found) {                                                                                \
            _LDICT_FREE_K(K, V)(bk);                                                                \
            return false;                                                                            \
        }                                                                                            \
        bool removed = base_ldict_remove(&d->inner, bk);                                            \
        if (removed) {                                                                               \
            _LDICT_FREE_V(K, V)(raw_val);                                                            \
        }                                                                                            \
        _LDICT_FREE_K(K, V)(bk);                                                                    \
        return removed;                                                                              \
    }                                                                                                \
                                                                                                     \
    /* Contains */                                                                                   \
    static inline bool _LDICT_CONTAINS(K, V)(ldict_t(K, V) * d, K key)                              \
    {                                                                                                \
        void *bk    = _LDICT_BOX_K(K, V)(key);                                                      \
        bool  found = base_ldict_contains(&d->inner, bk);                                           \
        _LDICT_FREE_K(K, V)(bk);                                                                    \
        return found;                                                                                \
    }                                                                                                \
                                                                                                     \
    /* Free: iterate all buckets, free boxed keys+values, then free dict */                          \
    static inline void _LDICT_FREE(K, V)(ldict_t(K, V) * d)                                         \
    {                                                                                                \
        if (sizeof(K) > sizeof(void *) || sizeof(V) > sizeof(void *)) {                    \
            base_ldict_store_t *s = atomic_load_explicit(&d->inner.store, memory_order_acquire);     \
            if (s) {                                                                                 \
                for (uint32_t i = 0; i <= s->last_slot; i++) {                                       \
                    base_ldict_bucket_t *b = &s->buckets[i];                                         \
                    if (b->hv != 0                                                                   \
                        && !(atomic_load_explicit(&b->flags, memory_order_acquire) & 4)) {           \
                        _LDICT_FREE_K(K, V)(b->key);                                                 \
                        _LDICT_FREE_V(K, V)(b->value);                                               \
                    }                                                                                \
                }                                                                                    \
            }                                                                                        \
        }                                                                                            \
        base_ldict_free(&d->inner);                                                                  \
    }

/**
 * @brief Create a zero-initialized typed locking dictionary.
 * @param K  Key type.
 * @param V  Value type.
 * @return A zero-initialized dictionary.
 */
#define ldict_new(K, V)                  ((ldict_t(K, V)){0})

/**
 * @brief Initialize a typed locking dictionary.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param cap  Initial capacity (0 for default).
 */
#define ldict_init(K, V, d, cap)         _LDICT_INIT(K, V)(d, cap)

/**
 * @brief Insert or update a key-value pair.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to insert.
 * @param val  Value to associate.
 */
#define ldict_put(K, V, d, key, val)     _LDICT_PUT(K, V)(d, key, val)

/**
 * @brief Look up a key, writing the value to @p out.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to look up.
 * @param out  Pointer where the value is written if found.
 * @return @c true if the key was found.
 */
#define ldict_get(K, V, d, key, out)     _LDICT_GET(K, V)(d, key, out)

/**
 * @brief Replace the value for an existing key.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to update.
 * @param val  New value.
 * @return @c true if the key existed and was updated.
 */
#define ldict_replace(K, V, d, key, val) _LDICT_REPLACE(K, V)(d, key, val)

/**
 * @brief Add a key-value pair only if the key is not present.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to add.
 * @param val  Value to associate.
 * @return @c true if the key was new and was inserted.
 */
#define ldict_add(K, V, d, key, val)     _LDICT_ADD(K, V)(d, key, val)

/**
 * @brief Remove a key from the dictionary.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to remove.
 * @return @c true if the key was found and removed.
 */
#define ldict_remove(K, V, d, key)       _LDICT_REMOVE(K, V)(d, key)

/**
 * @brief Check if a key exists in the dictionary.
 * @param K    Key type.
 * @param V    Value type.
 * @param d    Pointer to the dictionary.
 * @param key  Key to check.
 * @return @c true if the key is present.
 */
#define ldict_contains(K, V, d, key)     _LDICT_CONTAINS(K, V)(d, key)

/**
 * @brief Get the number of live entries.
 * @param d  Pointer to the dictionary.
 * @return Number of entries.
 */
#define ldict_len(d)                     base_ldict_len(&(d)->inner)

/**
 * @brief Free the dictionary and all boxed keys/values.
 * @param K  Key type.
 * @param V  Value type.
 * @param d  Pointer to the dictionary.
 */
#define ldict_free(K, V, d)              _LDICT_FREE(K, V)(d)

/**
 * @name Prefixed aliases
 * @{
 */
/** @copydoc ldict_new */
#define base_ldict_new(K, V)                    ldict_new(K, V)
/** @copydoc ldict_init */
#define base_ldict_typed_init(K, V, d, cap)     ldict_init(K, V, d, cap)
/** @copydoc ldict_put */
#define base_ldict_typed_put(K, V, d, key, val) ldict_put(K, V, d, key, val)
/** @copydoc ldict_get */
#define base_ldict_typed_get(K, V, d, key, out) ldict_get(K, V, d, key, out)
/** @copydoc ldict_remove */
#define base_ldict_typed_remove(K, V, d, key)   ldict_remove(K, V, d, key)
/** @copydoc ldict_contains */
#define base_ldict_typed_contains(K, V, d, key) ldict_contains(K, V, d, key)
/** @copydoc ldict_len */
#define base_ldict_typed_len(d)                 ldict_len(d)
/** @copydoc ldict_free */
#define base_ldict_typed_free(K, V, d)          ldict_free(K, V, d)
/** @} */
