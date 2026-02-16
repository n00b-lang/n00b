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
 *     array_t(int) lst = array_new(int);
 *     array_push(lst, 42);
 *     int x = array_get(lst, 0);
 *     array_free(lst);
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
 * @brief Declare a array type for element type @p T.
 * @param T  Element type.
 *
 * Struct layout: @c { T *data; size_t len; size_t cap; }
 */
#define n00b_array_t(T)   struct n00b_array_tid(T)

#define n00b_array_decl(T)                                                                     \
    struct n00b_array_tid(T) {                                                                 \
        T     *data;                                                                           \
        size_t len;                                                                            \
        size_t cap;                                                                            \
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
#define n00b_array_new(T, N)                                                                   \
    ({                                                                                         \
        (n00b_array_t(T)){                                                                     \
            .len  = 0,                                                                         \
            .cap  = N,                                                                         \
            .data = n00b_alloc(N * sizeof(T)),                                                 \
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
 * @brief Free the backing storage of a array (only use if _new'd)
 * @param x  List (lvalue).
 */
#define n00b_array_free(x) n00b_dealloc((x).data)

// ============================================================================
// Access
// ============================================================================

/**
 * @brief Get element at index (bounds-checked, aborts on OOB).
 * @param x  List (lvalue).
 * @param i  Index.
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
 * @param x    List (lvalue).
 * @param i    Index.
 * @param val  Value to assign.
 */
#define n00b_array_set(x, i, val)                                                              \
    ({                                                                                         \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= (x).len) {                                                                \
            abort();                                                                           \
        }                                                                                      \
        (x).data[_bl_i] = (val);                                                               \
    })

// ============================================================================
// Mutation
// ============================================================================

/**
 * @brief Append an element to the end.
 * @param x    List (lvalue, mutated in-place).
 * @param val  Value to append.
 */
#define n00b_array_push(x, val)                                                                \
    do {                                                                                       \
        if ((x).len == (x).cap) {                                                              \
            (x) = n00b_array_grow((x));                                                        \
        }                                                                                      \
        (x).data[(x).len++] = (val);                                                           \
    } while (0)

/**
 * @brief Remove and return the last element.
 * @param x  List (lvalue, mutated in-place).
 * @return The removed element.
 */
#define n00b_array_pop(x)                                                                      \
    ({                                                                                         \
        assert((x).len);                                                                       \
        (x).data[--(x).len];                                                                   \
    })

/**
 * @brief Remove element at index, shifting remaining left.
 * @param x  List (lvalue, mutated in-place).
 * @param i  Index to remove.
 * @return The removed element.
 */
#define n00b_array_remove(x, i)                                                                \
    ({                                                                                         \
        size_t _bl_ri = (i);                                                                   \
        assert(_bl_ri < (x).len);                                                              \
        typeof((x).data[0]) _bl_removed = (x).data[_bl_ri];                                    \
        if (_bl_ri < (x).len - 1) {                                                            \
            memmove(&(x).data[_bl_ri],                                                         \
                    &(x).data[_bl_ri + 1],                                                     \
                    ((x).len - _bl_ri - 1) * sizeof((x).data[0]));                             \
        }                                                                                      \
        (x).len--;                                                                             \
        _bl_removed;                                                                           \
    })

/**
 * @brief Remove and return the first element (shift all left).
 * @param x  List (lvalue, mutated in-place).
 * @return The removed element.
 */
#define n00b_array_dequeue(x) n00b_array_remove(x, 0)

/**
 * @brief Insert an element at index, shifting existing elements right.
 * @param x    List (lvalue, mutated in-place).
 * @param i    Index to insert at (0 <= i <= len).
 * @param val  Value to insert.
 */
#define n00b_array_insert(x, i, val)                                                           \
    do {                                                                                       \
        size_t _bl_ii = (i);                                                                   \
        assert(_bl_ii <= (x).len);                                                             \
        if ((x).len == (x).cap) {                                                              \
            (x) = n00b_array_grow((x));                                                        \
        }                                                                                      \
        if (_bl_ii < (x).len) {                                                                \
            memmove(&(x).data[_bl_ii + 1],                                                     \
                    &(x).data[_bl_ii],                                                         \
                    ((x).len - _bl_ii) * sizeof((x).data[0]));                                 \
        }                                                                                      \
        (x).data[_bl_ii] = (val);                                                              \
        (x).len++;                                                                             \
    } while (0)

/**
 * @brief Clear the array (reset length to 0, keep capacity).
 * @param x  List (lvalue).
 */
#define n00b_array_clear(x) ((x).len = 0)

// ============================================================================
// Query
// ============================================================================

/** @brief Number of elements in the array. */
#define n00b_array_len(x) ((x).len)

/** @brief Allocated capacity. */
#define n00b_array_cap(x) ((x).cap)

/**
 * @brief Linear search for a value.
 * @param x    List (lvalue).
 * @param val  Value to find (compared with ==).
 * @return Index of first match, or @c (size_t)-1 if not found.
 */
#define n00b_array_find(x, val)                                                                \
    ({                                                                                         \
        size_t              _bl_found  = (size_t)-1;                                           \
        typeof((x).data[0]) _bl_needle = (val);                                                \
        for (size_t _bl_fi = 0; _bl_fi < (x).len; _bl_fi++) {                                  \
            if ((x).data[_bl_fi] == _bl_needle) {                                              \
                _bl_found = _bl_fi;                                                            \
                break;                                                                         \
            }                                                                                  \
        }                                                                                      \
        _bl_found;                                                                             \
    })

// ============================================================================
// Bulk operations
// ============================================================================

/**
 * @brief Clone a array (deep copy of data array).
 * @param x  List (lvalue).
 * @return A new array with copied data.
 */
#define n00b_array_clone(x)                                                                    \
    ({                                                                                         \
        typeof(x) _bl_copy = (typeof(x)){                                                      \
            .len  = (x).len,                                                                   \
            .cap  = (x).cap,                                                                   \
            .data = n00b_alloc((x).cap * sizeof((x).data[0])),                                 \
        };                                                                                     \
        memcpy(_bl_copy.data, (x).data, (x).len * sizeof((x).data[0]));                        \
        _bl_copy;                                                                              \
    })

/**
 * @brief Append all elements from src to dst.
 * @param dst  Destination array (lvalue, mutated).
 * @param src  Source array (lvalue).
 */
#define n00b_array_extend(dst, src)                                                            \
    do {                                                                                       \
        for (size_t _bl_ei = 0; _bl_ei < (src).len; _bl_ei++) {                                \
            n00b_array_push((dst), (src).data[_bl_ei]);                                        \
        }                                                                                      \
    } while (0)

/**
 * @brief Sort the array in-place using qsort.
 * @param x    List (lvalue).
 * @param cmp  Comparison function (same signature as qsort's compar).
 */
#define n00b_array_sort(x, cmp) qsort((x).data, (x).len, sizeof((x).data[0]), (cmp))

// ============================================================================
// Iteration
// ============================================================================

/**
 * @brief Iterate over array elements.
 * @param lst  List (lvalue).
 * @param var  Pointer variable name for the loop body.
 *
 * Example:
 * @code
 *     n00b_array_foreach(lst, p) {
 *         printf("%d\n", *p);
 *     }
 * @endcode
 */
#define n00b_array_foreach(lst, var)                                                           \
    for (typeof((lst).data) var = (lst).data; (var) < (lst).data + (lst).len; ++(var))
