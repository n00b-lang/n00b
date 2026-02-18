/**
 * @file list.h
 * @brief Type-safe dynamic list (growable, deque, sorted, thread-safe).
 *
 * @c n00b_list_t(T) provides a growable linear buffer with push/pop at
 * both ends, insert/delete at arbitrary indices, bulk operations, sort,
 * find, and an atomic spinlock for thread safety.
 *
 * Type safety is enforced through ncc's @c typeid().
 *
 * All mutating macros acquire a per-list spinlock.  @c n00b_list_foreach
 * is explicitly unlocked — the caller must ensure exclusivity.
 *
 * Requires @c core/alloc.h to be included by the consumer for
 * @c n00b_alloc_array / @c n00b_alloc_size / @c n00b_free.
 *
 * Usage:
 * @code
 *     n00b_list_decl(int);
 *     n00b_list_t(int) lst = n00b_list_new(int);
 *     n00b_list_push(lst, 42);
 *     int x = n00b_list_get(lst, 0);
 *     n00b_list_free(lst);
 * @endcode
 */
#pragma once

#include <assert.h>

#include "n00b.h"
#include "core/macros.h"
#include "core/atomic.h"
#include "core/align.h"

// ============================================================================
// Constants
// ============================================================================

#define N00B_DEFAULT_LIST_SZ 16

// ============================================================================
// Type definition
// ============================================================================

#define n00b_list_tid(T) typeid("n00b_list", T)

/**
 * @brief Reference a list type for element type @p T.
 * @param T  Element type.
 */
#define n00b_list_t(T) struct n00b_list_tid(T)

/**
 * @brief Declare (define) a list struct for element type @p T.
 * @param T  Element type.
 *
 * Struct layout: @c { T *data; size_t len; size_t cap; n00b_spin_lock_t lock; }
 */
#define n00b_list_decl(T)                                                                      \
    struct n00b_list_tid(T) {                                                                  \
        T                *data;                                                                \
        size_t            len;                                                                 \
        size_t            cap;                                                                 \
        n00b_spin_lock_t  lock;                                                                \
        n00b_allocator_t *allocator;                                                           \
    }

// ============================================================================
// Internal helpers  (not part of public API)
// ============================================================================

/**
 * @internal Acquire spinlock (busy-wait).
 */
#define _n00b_list_lock(xptr)                                                                  \
    while (n00b_atomic_or(&(xptr)->lock, 1) != 0)                                              \
        ;

/**
 * @internal Release spinlock.
 */
#define _n00b_list_unlock(xptr) n00b_atomic_store(&(xptr)->lock, 0)

/**
 * @internal Grow backing store to at least @p needed elements (power-of-2).
 *           MUST be called within a locked context.
 */
#define _n00b_list_ensure_cap(xptr, needed)                                                    \
    do {                                                                                       \
        size_t _bl_need = (needed);                                                            \
        if (_bl_need > (xptr)->cap) {                                                          \
            size_t               _bl_nc = n00b_align_closest_pow2_ceil(_bl_need);              \
            typeof((xptr)->data) _bl_nd = n00b_alloc_size(                                    \
                _bl_nc, sizeof(*(xptr)->data), .allocator = (xptr)->allocator);                \
            if ((xptr)->len > 0) {                                                             \
                memcpy(_bl_nd, (xptr)->data, (xptr)->len * sizeof(*(xptr)->data));             \
            }                                                                                  \
            if ((xptr)->data) {                                                                \
                n00b_free((xptr)->data);                                                       \
            }                                                                                  \
            (xptr)->data = _bl_nd;                                                             \
            (xptr)->cap  = _bl_nc;                                                             \
        }                                                                                      \
    } while (0)

// ============================================================================
// Construction / destruction
// ============================================================================

/**
 * @brief Create a new list with default capacity (N00B_DEFAULT_LIST_SZ).
 * @param T  Element type.
 */
#define n00b_list_new(T, ...)                                                                   \
    ({                                                                                         \
        (n00b_list_t(T)){                                                                      \
            .data = n00b_alloc_array(T, N00B_DEFAULT_LIST_SZ                                   \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                \
            .len  = 0,                                                                         \
            .cap  = N00B_DEFAULT_LIST_SZ,                                                      \
            .lock = 0,                                                                         \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                              \
        };                                                                                     \
    })

/**
 * @brief Create a new list with specific capacity (rounded up to pow2).
 * @param T  Element type.
 * @param N  Requested minimum capacity.
 */
#define n00b_list_new_cap(T, N, ...)                                                            \
    ({                                                                                         \
        size_t _bl_rc = n00b_align_closest_pow2_ceil(n00b_max((size_t)(N), (size_t)1));        \
        (n00b_list_t(T)){                                                                      \
            .data = n00b_alloc_array(T, _bl_rc                                                 \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                \
            .len  = 0,                                                                         \
            .cap  = _bl_rc,                                                                    \
            .lock = 0,                                                                         \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                              \
        };                                                                                     \
    })

/**
 * @brief Free the backing storage of a list and zero the struct.
 * @param x  List (lvalue).
 */
#define n00b_list_free(x)                                                                      \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        if (_bl_lp->data) {                                                                    \
            n00b_free(_bl_lp->data);                                                           \
        }                                                                                      \
        *_bl_lp = (typeof(x)){};                                                               \
    })

// ============================================================================
// Access  (all locked)
// ============================================================================

/** @brief Element count. */
#define n00b_list_len(x)                                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_r = _bl_lp->len;                                                            \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/** @brief Allocated capacity. */
#define n00b_list_cap(x)                                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_r = _bl_lp->cap;                                                            \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/**
 * @brief Get element at index (bounds-checked, aborts on OOB).
 * @param x  List (lvalue).
 * @param i  Index.
 */
#define n00b_list_get(x, i)                                                                    \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_lp->len) {                                                            \
            _n00b_list_unlock(_bl_lp);                                                         \
            abort();                                                                           \
        }                                                                                      \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                                     \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

/**
 * @brief Set element at index (bounds-checked against len, aborts on OOB).
 * @param x    List (lvalue).
 * @param i    Index.
 * @param val  Value to assign.
 */
#define n00b_list_set(x, i, val)                                                               \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_i = (i);                                                                    \
        if (_bl_i >= _bl_lp->len) {                                                            \
            _n00b_list_unlock(_bl_lp);                                                         \
            abort();                                                                           \
        }                                                                                      \
        _bl_lp->data[_bl_i] = (val);                                                           \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Push / Pop — Back  (locked)
// ============================================================================

/**
 * @brief Append an element to the back, growing if needed.
 * @param x    List (lvalue).
 * @param val  Value to append.
 */
#define n00b_list_push(x, val)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        _bl_lp->data[_bl_lp->len++] = (val);                                                   \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove and return the last element.  Asserts non-empty.
 * @param x  List (lvalue).
 */
#define n00b_list_pop(x)                                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        assert(_bl_lp->len > 0);                                                               \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[--_bl_lp->len];                             \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Push / Pop — Front  (locked)
// ============================================================================

/**
 * @brief Insert an element at the front, shifting right.
 * @param x    List (lvalue).
 * @param val  Value to prepend.
 */
#define n00b_list_push_front(x, val)                                                           \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        if (_bl_lp->len > 0) {                                                                 \
            memmove(_bl_lp->data + 1, _bl_lp->data, _bl_lp->len * sizeof(*_bl_lp->data));      \
        }                                                                                      \
        _bl_lp->data[0] = (val);                                                               \
        _bl_lp->len++;                                                                         \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove and return element 0, shifting left.  Asserts non-empty.
 * @param x  List (lvalue).
 */
#define n00b_list_pop_front(x)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        assert(_bl_lp->len > 0);                                                               \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[0];                                         \
        _bl_lp->len--;                                                                         \
        if (_bl_lp->len > 0) {                                                                 \
            memmove(_bl_lp->data, _bl_lp->data + 1, _bl_lp->len * sizeof(*_bl_lp->data));      \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Insert / Delete — Single Element  (locked)
// ============================================================================

/**
 * @brief Insert a value at index @p i, sliding existing elements right.
 * @param x    List (lvalue).
 * @param i    Insertion index (0 <= i <= len).
 * @param val  Value to insert.
 */
#define n00b_list_insert(x, i, val)                                                            \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_i = (i);                                                                    \
        assert(_bl_i <= _bl_lp->len);                                                          \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                                        \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i + 1,                                                  \
                    _bl_lp->data + _bl_i,                                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        _bl_lp->data[_bl_i] = (val);                                                           \
        _bl_lp->len++;                                                                         \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove element at index @p i, sliding left.  Returns removed value.
 * @param x  List (lvalue).
 * @param i  Index to remove (0 <= i < len).
 */
#define n00b_list_delete(x, i)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_i = (i);                                                                    \
        assert(_bl_i < _bl_lp->len);                                                           \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                                     \
        _bl_lp->len--;                                                                         \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i,                                                      \
                    _bl_lp->data + _bl_i + 1,                                                  \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Insert / Delete — Bulk  (locked)
// ============================================================================

/**
 * @brief Insert all elements of @p src at index @p i in @p x.
 *        Both lists must be the same type.  @p src is not modified.
 *        Do NOT pass the same list as both @p x and @p src.
 * @param x    Destination list (lvalue).
 * @param i    Insertion index (0 <= i <= len).
 * @param src  Source list (lvalue, read-only during call).
 */
#define n00b_list_insert_list(x, i, src)                                                       \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        auto _bl_sp = &(src);                                                                  \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_i  = (i);                                                                   \
        size_t _bl_sn = _bl_sp->len;                                                           \
        assert(_bl_i <= _bl_lp->len);                                                          \
        _n00b_list_ensure_cap(_bl_lp, _bl_lp->len + _bl_sn);                                   \
        if (_bl_i < _bl_lp->len) {                                                             \
            memmove(_bl_lp->data + _bl_i + _bl_sn,                                             \
                    _bl_lp->data + _bl_i,                                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));                            \
        }                                                                                      \
        memcpy(_bl_lp->data + _bl_i, _bl_sp->data, _bl_sn * sizeof(*_bl_lp->data));            \
        _bl_lp->len += _bl_sn;                                                                 \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Remove @p count elements starting at @p start.
 * @param x      List (lvalue).
 * @param start  First index to remove.
 * @param count  Number of elements to remove.
 */
#define n00b_list_delete_range(x, start, count)                                                \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        size_t _bl_s = (start);                                                                \
        size_t _bl_c = (count);                                                                \
        assert(_bl_s + _bl_c <= _bl_lp->len);                                                  \
        size_t _bl_tail = _bl_lp->len - _bl_s - _bl_c;                                         \
        if (_bl_tail > 0) {                                                                    \
            memmove(_bl_lp->data + _bl_s,                                                      \
                    _bl_lp->data + _bl_s + _bl_c,                                              \
                    _bl_tail * sizeof(*_bl_lp->data));                                         \
        }                                                                                      \
        _bl_lp->len -= _bl_c;                                                                  \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Concatenation
// ============================================================================

/**
 * @brief Return a new list containing elements of @p a followed by @p b.
 *        Does not lock sources — caller ensures stability.
 * @param a  First list (lvalue).
 * @param b  Second list (lvalue).
 */
#define n00b_list_concat(a, b)                                                                 \
    ({                                                                                         \
        auto      _bl_ap  = &(a);                                                              \
        auto      _bl_bp  = &(b);                                                              \
        size_t    _bl_tl  = _bl_ap->len + _bl_bp->len;                                         \
        size_t    _bl_tc  = n00b_align_closest_pow2_ceil(n00b_max(_bl_tl, (size_t)1));         \
        typeof(a) _bl_new = {                                                                  \
            .data = n00b_alloc_size(_bl_tc, sizeof(*_bl_ap->data),                             \
                        .allocator = _bl_ap->allocator),                                       \
            .len       = _bl_tl,                                                               \
            .cap       = _bl_tc,                                                               \
            .lock      = 0,                                                                    \
            .allocator = _bl_ap->allocator,                                                    \
        };                                                                                     \
        if (_bl_ap->len > 0) {                                                                 \
            memcpy(_bl_new.data, _bl_ap->data, _bl_ap->len * sizeof(*_bl_ap->data));           \
        }                                                                                      \
        if (_bl_bp->len > 0) {                                                                 \
            memcpy(_bl_new.data + _bl_ap->len,                                                 \
                   _bl_bp->data,                                                               \
                   _bl_bp->len * sizeof(*_bl_bp->data));                                       \
        }                                                                                      \
        _bl_new;                                                                               \
    })

// ============================================================================
// Search  (locked)
// ============================================================================

/**
 * @brief Find the first index of @p val, or @c (size_t)-1 if not found.
 * @param x    List (lvalue).
 * @param val  Value to search for (compared with @c ==).
 */
#define n00b_list_find(x, val)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        typeof(*_bl_lp->data) _bl_v = (val);                                                   \
        size_t                _bl_r = (size_t)-1;                                              \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                                 \
            if (_bl_lp->data[_bl_i] == _bl_v) {                                                \
                _bl_r = _bl_i;                                                                 \
                break;                                                                         \
            }                                                                                  \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_r;                                                                                 \
    })

// ============================================================================
// Sort  (locked)
// ============================================================================

/**
 * @brief In-place sort via @c qsort.
 * @param x    List (lvalue).
 * @param cmp  Comparator with standard @c qsort signature:
 *             @c int(*)(const void*, const void*)
 */
#define n00b_list_sort(x, cmp)                                                                 \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        if (_bl_lp->len > 1) {                                                                 \
            qsort(_bl_lp->data, _bl_lp->len, sizeof(*_bl_lp->data), (cmp));                    \
        }                                                                                      \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

// ============================================================================
// Remove by value  (locked)
// ============================================================================

/**
 * @brief Remove ALL instances of @p val.  Returns count removed.
 *        Single-pass compaction.
 * @param x    List (lvalue).
 * @param val  Value to remove (compared with @c ==).
 */
#define n00b_list_remove_all(x, val)                                                           \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        typeof(*_bl_lp->data) _bl_v = (val);                                                   \
        size_t                _bl_w = 0;                                                       \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                                 \
            if (_bl_lp->data[_bl_i] != _bl_v) {                                                \
                _bl_lp->data[_bl_w++] = _bl_lp->data[_bl_i];                                   \
            }                                                                                  \
        }                                                                                      \
        size_t _bl_removed = _bl_lp->len - _bl_w;                                              \
        _bl_lp->len        = _bl_w;                                                            \
        _n00b_list_unlock(_bl_lp);                                                             \
        _bl_removed;                                                                           \
    })

// ============================================================================
// Utilities
// ============================================================================

/**
 * @brief Deep copy — returns a new, independent list.
 *        Does not lock source — caller ensures stability.
 * @param x  List (lvalue).
 */
#define n00b_list_clone(x)                                                                     \
    ({                                                                                         \
        auto      _bl_sp  = &(x);                                                              \
        size_t    _bl_nc  = n00b_max(_bl_sp->cap, (size_t)1);                                  \
        typeof(x) _bl_new = {                                                                  \
            .data = n00b_alloc_size(_bl_nc, sizeof(*_bl_sp->data),                             \
                        .allocator = _bl_sp->allocator),                                       \
            .len       = _bl_sp->len,                                                          \
            .cap       = _bl_nc,                                                               \
            .lock      = 0,                                                                    \
            .allocator = _bl_sp->allocator,                                                    \
        };                                                                                     \
        if (_bl_sp->len > 0) {                                                                 \
            memcpy(_bl_new.data, _bl_sp->data, _bl_sp->len * sizeof(*_bl_sp->data));           \
        }                                                                                      \
        _bl_new;                                                                               \
    })

/**
 * @brief Reset len to 0, keeping allocated capacity.
 * @param x  List (lvalue).
 */
#define n00b_list_clear(x)                                                                     \
    ({                                                                                         \
        auto _bl_lp = &(x);                                                                    \
        _n00b_list_lock(_bl_lp);                                                               \
        _bl_lp->len = 0;                                                                       \
        _n00b_list_unlock(_bl_lp);                                                             \
    })

/**
 * @brief Pointer-based iteration loop.
 *        NOT locked — caller must ensure exclusive access.
 *
 * Example:
 * @code
 *     n00b_list_foreach(lst, p) {
 *         printf("%d\n", *p);
 *     }
 * @endcode
 */
#define n00b_list_foreach(x, var)                                                              \
    for (typeof((x).data) var = (x).data; (var) < (x).data + (x).len; ++(var))
