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
    (*(({                                                                                      \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= (x).len) {                                                                \
            abort();                                                                           \
        }                                                                                      \
        &(x).data[_bl_i];                                                                      \
    })))

/**
 * @brief Set element at index (bounds-checked, aborts on OOB).
 * @param x    Array (lvalue).
 * @param i    Index.
 * @param val  Value to assign.
 * @pre @p i < array capacity.
 */
#define n00b_array_set(x, i, val)                                                              \
    ({                                                                                         \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= (x).cap) {                                                                \
            abort();                                                                           \
        }                                                                                      \
        if (_bl_i >= (x).len) {                                                                \
            (x).len = _bl_i;                                                                   \
        }                                                                                      \
        (x).data[_bl_i] = (val);                                                               \
    })

/** @brief Number of elements in the array. */
#define n00b_array_len(x) ((x).len)

/** @brief Allocated capacity. */
#define n00b_array_cap(x) ((x).cap)

/**
 * @brief Clone an array (deep copy of data buffer).
 * @param x  Array (lvalue).
 * @return A new array with copied data.
 */
#define n00b_array_clone(x)                                                                    \
    ({                                                                                         \
        typeof(x) _bl_copy = (typeof(x)){                                                      \
            .len       = (x).len,                                                              \
            .cap       = (x).cap,                                                              \
            .data      = n00b_alloc((x).cap * sizeof((x).data[0]),                             \
                             .allocator = (x).allocator),                                      \
            .allocator = (x).allocator,                                                        \
        };                                                                                     \
        memcpy(_bl_copy.data, (x).data, (x).len * sizeof((x).data[0]));                        \
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
