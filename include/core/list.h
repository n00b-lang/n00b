/**
 * @file list.h
 * @brief Type-safe dynamic array (growable list).
 *
 * @c base_list_t(T) provides a growable array that automatically resizes.
 * Type safety is enforced through ncc's @c typeid().
 *
 * All macros take lvalues directly (value semantics). They mutate
 * the struct in-place via statement expressions.
 *
 * Usage:
 * @code
 *     list_t(int) lst = list_new(int);
 *     list_push(lst, 42);
 *     int x = list_get(lst, 0);
 *     list_free(lst);
 * @endcode
 */
#pragma once

#include <assert.h>

#include "alloc.h"
#include "macros.h"

// ============================================================================
// Type definition
// ============================================================================

#define n00b_list_tag(T) typeid("n00b_list", T)
/**
 * @brief Declare a list type for element type @p T.
 * @param T  Element type.
 *
 * Struct layout: @c { T *data; size_t len; size_t cap; }
 */
#define base_list_t(T)                                                                         \
    struct n00b_list_tag(T) {                                                                  \
        T     *data;                                                                           \
        size_t len;                                                                            \
        size_t cap;                                                                            \
    }

/** @brief Tag-only reference — use after @c base_list_t(T) has defined the struct. */
#define base_list_tag(T) struct n00b_list_tag(T)

// ============================================================================
// Internal helpers
// ============================================================================

#define BASE_LIST_DEFAULT 16

/** @brief Grow the backing storage by 2x. Internal helper. */
#define base_list_grow(x)                                                                      \
    ({                                                                                         \
        size_t    _bl_newcap = (x).cap ? (x).cap * 2 : BASE_LIST_DEFAULT;                      \
        typeof(x) _bl_bigger = (typeof(x)){                                                    \
            .len  = (x).len,                                                                   \
            .cap  = _bl_newcap,                                                                \
            .data = base_alloc(_bl_newcap * sizeof((x).data[0])),                              \
        };                                                                                     \
        memcpy(_bl_bigger.data, (x).data, (x).len * sizeof((x).data[0]));                      \
        base_dealloc((x).data);                                                                \
        _bl_bigger;                                                                            \
    })

// ============================================================================
// Construction / destruction
// ============================================================================

/**
 * @brief Create a new list with optional initial capacity.
 * @param T     Element type.
 * @param ...   Optional initial capacity (default 16).
 */
#define base_list_new(T, ...)                                                                  \
    ({                                                                                         \
        size_t _bl_max = BASE_FIRST(__VA_ARGS__ __VA_OPT__(, ) BASE_LIST_DEFAULT);             \
        (base_list_tag(T)){                                                                    \
            .len  = 0,                                                                         \
            .cap  = _bl_max,                                                                   \
            .data = base_alloc(_bl_max * sizeof(T)),                                           \
        };                                                                                     \
    })

/**
 * @brief Heap-allocate a list (returns pointer).
 * @param T     Element type.
 * @param ...   Optional initial capacity.
 */
#define base_list_ptr(T, ...)                                                                  \
    ({                                                                                         \
        base_list_tag(T) *_bl_res = base_alloc(sizeof(base_list_tag(T)));                      \
        *_bl_res                  = base_list_new(T __VA_OPT__(, ) __VA_ARGS__);               \
        _bl_res;                                                                               \
    })

/**
 * @brief Free the backing storage of a list.
 * @param x  List (lvalue).
 */
#define base_list_free(x) base_dealloc((x).data)

// ============================================================================
// Access
// ============================================================================

/**
 * @brief Get element at index (bounds-checked, aborts on OOB).
 * @param x  List (lvalue).
 * @param i  Index.
 */
#define base_list_get(x, i)                                                                    \
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
#define base_list_set(x, i, val)                                                               \
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
#define base_list_push(x, val)                                                                 \
    do {                                                                                       \
        if ((x).len == (x).cap) {                                                              \
            (x) = base_list_grow((x));                                                         \
        }                                                                                      \
        (x).data[(x).len++] = (val);                                                           \
    } while (0)

/**
 * @brief Remove and return the last element.
 * @param x  List (lvalue, mutated in-place).
 * @return The removed element.
 */
#define base_list_pop(x)                                                                       \
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
#define base_list_remove(x, i)                                                                 \
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
#define base_list_dequeue(x) base_list_remove(x, 0)

/**
 * @brief Insert an element at index, shifting existing elements right.
 * @param x    List (lvalue, mutated in-place).
 * @param i    Index to insert at (0 <= i <= len).
 * @param val  Value to insert.
 */
#define base_list_insert(x, i, val)                                                            \
    do {                                                                                       \
        size_t _bl_ii = (i);                                                                   \
        assert(_bl_ii <= (x).len);                                                             \
        if ((x).len == (x).cap) {                                                              \
            (x) = base_list_grow((x));                                                         \
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
 * @brief Clear the list (reset length to 0, keep capacity).
 * @param x  List (lvalue).
 */
#define base_list_clear(x) ((x).len = 0)

// ============================================================================
// Query
// ============================================================================

/** @brief Number of elements in the list. */
#define base_list_len(x) ((x).len)

/** @brief Allocated capacity. */
#define base_list_cap(x) ((x).cap)

/**
 * @brief Linear search for a value.
 * @param x    List (lvalue).
 * @param val  Value to find (compared with ==).
 * @return Index of first match, or @c (size_t)-1 if not found.
 */
#define base_list_find(x, val)                                                                 \
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
 * @brief Clone a list (deep copy of data array).
 * @param x  List (lvalue).
 * @return A new list with copied data.
 */
#define base_list_clone(x)                                                                     \
    ({                                                                                         \
        typeof(x) _bl_copy = (typeof(x)){                                                      \
            .len  = (x).len,                                                                   \
            .cap  = (x).cap,                                                                   \
            .data = base_alloc((x).cap * sizeof((x).data[0])),                                 \
        };                                                                                     \
        memcpy(_bl_copy.data, (x).data, (x).len * sizeof((x).data[0]));                        \
        _bl_copy;                                                                              \
    })

/**
 * @brief Append all elements from src to dst.
 * @param dst  Destination list (lvalue, mutated).
 * @param src  Source list (lvalue).
 */
#define base_list_extend(dst, src)                                                             \
    do {                                                                                       \
        for (size_t _bl_ei = 0; _bl_ei < (src).len; _bl_ei++) {                                \
            base_list_push((dst), (src).data[_bl_ei]);                                         \
        }                                                                                      \
    } while (0)

/**
 * @brief Sort the list in-place using qsort.
 * @param x    List (lvalue).
 * @param cmp  Comparison function (same signature as qsort's compar).
 */
#define base_list_sort(x, cmp) qsort((x).data, (x).len, sizeof((x).data[0]), (cmp))

// ============================================================================
// Iteration
// ============================================================================

/**
 * @brief Iterate over list elements.
 * @param lst  List (lvalue).
 * @param var  Pointer variable name for the loop body.
 *
 * Example:
 * @code
 *     base_list_foreach(lst, p) {
 *         printf("%d\n", *p);
 *     }
 * @endcode
 */
#define base_list_foreach(lst, var)                                                            \
    for (typeof((lst).data) var = (lst).data; (var) < (lst).data + (lst).len; ++(var))

// ============================================================================
// Compat aliases (unprefixed short names)
// ============================================================================

#define list_t(T)              base_list_t(T)
#define list_tag(T)            base_list_tag(T)
#define list_new(T, ...)       base_list_new(T __VA_OPT__(, ) __VA_ARGS__)
#define list_push(x, val)      base_list_push(x, val)
#define list_pop(x)            base_list_pop(x)
#define list_at(x, i)          base_list_get(x, i)
#define list_get(x, i)         base_list_get(x, i)
#define list_set(x, i, val)    base_list_set(x, i, val)
#define list_len(x)            base_list_len(x)
#define list_cap(x)            base_list_cap(x)
#define list_clear(x)          base_list_clear(x)
#define list_free(x)           base_list_free(x)
#define list_clone(x)          base_list_clone(x)
#define list_foreach(x, var)   base_list_foreach(x, var)
#define list_remove(x, i)      base_list_remove(x, i)
#define list_dequeue(x)        base_list_dequeue(x)
#define list_insert(x, i, val) base_list_insert(x, i, val)
#define list_find(x, val)      base_list_find(x, val)
#define list_sort(x, cmp)      base_list_sort(x, cmp)
#define list_extend(dst, src)  base_list_extend(dst, src)
#define list_ptr(T, ...)       base_list_ptr(T __VA_OPT__(, ) __VA_ARGS__)

/** @brief No-op, kept for backward compatibility. */
#define BASE_LIST_IMPL(...) /* no-op */
