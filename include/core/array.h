/**
 * @file array.h
 * @brief Type-safe dynamic array (growable array).
 *
 * @c n00b_array_t(T) provides a growable array that automatically resizes.
 * Type safety is enforced through ncc's @c typeid().
 *
 * All macros take lvalues directly (value semantics). They mutate
 * the struct in-place via statement expressions.
 *
 * Usage:
 * @code
 *     n00b_array_decl(int);
 *     n00b_array_t(int) arr = n00b_array_new(int, 16);
 *     n00b_array_set(arr, 0, 42);
 *     int x = n00b_array_get(arr, 0);
 *     n00b_array_free(arr);
 * @endcode
 */
#pragma once

#include <assert.h>

#include "macros.h"
#include "core/data_lock.h"

// ============================================================================
// Type definition
// ============================================================================

#define n00b_array_tid(T) typeid("array", T)
/**
 * @brief Declare an array type for element type @p T.
 * @param T  Element type.
 *
 * Struct layout: @c { T *data; size_t len; size_t cap; }
 */
#define n00b_array_t(T)   struct n00b_array_tid(T)

#define n00b_array_decl(T)                                                                     \
    struct n00b_array_tid(T) {                                                                 \
        T                *data;                                                                \
        size_t            len;                                                                 \
        size_t            cap;                                                                 \
        n00b_rwlock_t    *lock;                                                                \
        n00b_allocator_t *allocator;                                                           \
    }

// ============================================================================
// Internal helpers
// ============================================================================

// ============================================================================
// Construction / destruction
// ============================================================================

/**
 * @brief Create a new array with optional initial capacity.
 * @param T     Element type.
 * @param ...   Optional initial capacity (default 16).
 */
#define n00b_array_new(T, N, ...)                                                               \
    ({                                                                                         \
        (n00b_array_t(T)){                                                                     \
            .len  = 0,                                                                         \
            .cap  = N,                                                                         \
            .data = n00b_alloc(N * sizeof(T)                                                   \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                \
            .lock = nullptr,                                                                   \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                              \
        };                                                                                     \
    })

/**
 * @brief Create a new locked array with the given capacity.
 * @param T     Element type.
 * @param N     Initial capacity.
 */
#define n00b_array_new_locked(T, N, ...)                                                       \
    ({                                                                                         \
        (n00b_array_t(T)){                                                                     \
            .len  = 0,                                                                         \
            .cap  = N,                                                                         \
            .data = n00b_alloc(N * sizeof(T)                                                   \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                \
            .lock = n00b_data_lock_new(),                                                      \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                              \
        };                                                                                     \
    })

/**
 * @brief Heap-allocate a array (returns pointer).
 * @param T     Element type.
 * @param ...   Optional initial capacity.
 */
#define n00b_array_checked_ptr(T, N, P)                                                        \
    ({                                                                                         \
        (n00b_array_t(T)){                                                                     \
            .len  = 0,                                                                         \
            .cap  = N,                                                                         \
            .data = P,                                                                         \
            .lock = nullptr,                                                                   \
        };                                                                                     \
    })

/**
 * @brief Free the backing storage of an array (only use if _new'd).
 * @param x  Array (lvalue).
 */
#define n00b_array_free(x) n00b_free((x).data)

// ============================================================================
// Access
// ============================================================================

/**
 * @brief Get element at index (bounds-checked, aborts on OOB).
 * @param x  Array (lvalue).
 * @param i  Index.
 * @pre @p i < array length.
 */
#define n00b_array_get(x, i)                                                                   \
    ({                                                                                         \
        auto _bl_ap = &(x);                                                                    \
        n00b_data_read_lock(_bl_ap->lock);                                                     \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_ap->len) {                                                            \
            n00b_data_unlock(_bl_ap->lock);                                                    \
            abort();                                                                           \
        }                                                                                      \
        typeof(*_bl_ap->data) _bl_r = _bl_ap->data[_bl_i];                                     \
        n00b_data_unlock(_bl_ap->lock);                                                        \
        _bl_r;                                                                                 \
    })

/**
 * @brief Set element at index (bounds-checked, aborts on OOB).
 * @param x    Array (lvalue).
 * @param i    Index.
 * @param val  Value to assign.
 * @pre @p i < array capacity.
 */
#define n00b_array_set(x, i, val)                                                              \
    ({                                                                                         \
        auto _bl_ap = &(x);                                                                    \
        n00b_data_write_lock(_bl_ap->lock);                                                    \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_ap->cap) {                                                            \
            n00b_data_unlock(_bl_ap->lock);                                                    \
            abort();                                                                           \
        }                                                                                      \
        if (_bl_i >= _bl_ap->len) {                                                            \
            _bl_ap->len = _bl_i;                                                               \
        }                                                                                      \
        _bl_ap->data[_bl_i] = (val);                                                           \
        n00b_data_unlock(_bl_ap->lock);                                                        \
    })

/** @brief Number of elements in the array. */
#define n00b_array_len(x)                                                                      \
    ({                                                                                         \
        auto _bl_ap = &(x);                                                                    \
        n00b_data_read_lock(_bl_ap->lock);                                                     \
        size_t _bl_r = _bl_ap->len;                                                            \
        n00b_data_unlock(_bl_ap->lock);                                                        \
        _bl_r;                                                                                 \
    })

/** @brief Allocated capacity. */
#define n00b_array_cap(x)                                                                      \
    ({                                                                                         \
        auto _bl_ap = &(x);                                                                    \
        n00b_data_read_lock(_bl_ap->lock);                                                     \
        size_t _bl_r = _bl_ap->cap;                                                            \
        n00b_data_unlock(_bl_ap->lock);                                                        \
        _bl_r;                                                                                 \
    })

/**
 * @brief Clone an array (deep copy of data buffer).
 * @param x  Array (lvalue).
 * @return A new array with copied data.
 */
#define n00b_array_clone(x)                                                                    \
    ({                                                                                         \
        auto _bl_sp = &(x);                                                                    \
        typeof(x) _bl_copy = (typeof(x)){                                                      \
            .len       = _bl_sp->len,                                                          \
            .cap       = _bl_sp->cap,                                                          \
            .data      = n00b_alloc(_bl_sp->cap * sizeof(_bl_sp->data[0]),                     \
                             .allocator = _bl_sp->allocator),                                  \
            .lock      = _bl_sp->lock ? n00b_data_lock_new() : nullptr,                        \
            .allocator = _bl_sp->allocator,                                                    \
        };                                                                                     \
        memcpy(_bl_copy.data, _bl_sp->data, _bl_sp->len * sizeof(_bl_sp->data[0]));            \
        _bl_copy;                                                                              \
    })

/**
 * @brief Iterate over array elements.
 * @param arr  Array (lvalue).
 * @param var  Pointer variable name for the loop body.
 *
 * Example:
 * @code
 *     n00b_array_t(int) arr = ...;
 *
 *     n00b_array_foreach(arr, p) {
 *         printf("value @%p = %d\n", p, *p);
 *     }
 * @endcode
 */
#define n00b_array_foreach(arr, var)                                                           \
    for (typeof((arr).data) var = (arr).data; (var) < (arr).data + (arr).len; ++(var))
