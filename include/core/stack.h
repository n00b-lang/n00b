/**
 * @file stack.h
 * @brief Type-safe dynamic stack (LIFO, thread-safe).
 *
 * @c n00b_stack_t(T) provides a growable LIFO stack backed by a linear
 * buffer, with an atomic spinlock for thread safety.
 *
 * Type safety is enforced through ncc's @c typeid().
 *
 * Requires @c core/alloc.h to be included by the consumer for
 * @c n00b_alloc_array / @c n00b_alloc_size / @c n00b_free.
 *
 * Usage:
 * @code
 *     n00b_stack_decl(int);
 *     n00b_stack_t(int) stk = n00b_stack_new(int);
 *     n00b_stack_push(stk, 42);
 *     int x = n00b_stack_pop(stk);
 *     n00b_stack_free(stk);
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

#define N00B_DEFAULT_STACK_SZ 16

// ============================================================================
// Type definition
// ============================================================================

#define n00b_stack_tid(T) typeid("n00b_stack", T)

#define n00b_stack_t(T) struct n00b_stack_tid(T)

#define n00b_stack_decl(T)                                                                         \
    struct n00b_stack_tid(T) {                                                                     \
        T                *data;                                                                    \
        size_t            len;                                                                     \
        size_t            cap;                                                                     \
        n00b_spin_lock_t  lock;                                                                    \
        n00b_allocator_t *allocator;                                                               \
    }

// ============================================================================
// Internal helpers  (not part of public API)
// ============================================================================

#define _n00b_stack_lock(xptr)                                                                     \
    while (n00b_atomic_or(&(xptr)->lock, 1) != 0)                                                  \
        ;

#define _n00b_stack_unlock(xptr) n00b_atomic_store(&(xptr)->lock, 0)

#define _n00b_stack_ensure_cap(xptr, needed)                                                       \
    do {                                                                                           \
        size_t _bl_need = (needed);                                                                \
        if (_bl_need > (xptr)->cap) {                                                              \
            size_t               _bl_nc = n00b_align_closest_pow2_ceil(_bl_need);                   \
            typeof((xptr)->data) _bl_nd = n00b_alloc_size(                                         \
                _bl_nc, sizeof(*(xptr)->data), .allocator = (xptr)->allocator);                    \
            if ((xptr)->len > 0) {                                                                 \
                memcpy(_bl_nd, (xptr)->data, (xptr)->len * sizeof(*(xptr)->data));                  \
            }                                                                                      \
            if ((xptr)->data) {                                                                    \
                n00b_free((xptr)->data);                                                            \
            }                                                                                      \
            (xptr)->data = _bl_nd;                                                                 \
            (xptr)->cap  = _bl_nc;                                                                 \
        }                                                                                          \
    } while (0)

// ============================================================================
// Construction / destruction
// ============================================================================

#define n00b_stack_new(T, ...)                                                                      \
    ({                                                                                             \
        (n00b_stack_t(T)){                                                                         \
            .data = n00b_alloc_array(T, N00B_DEFAULT_STACK_SZ                                      \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                   \
            .len  = 0,                                                                             \
            .cap  = N00B_DEFAULT_STACK_SZ,                                                         \
            .lock = 0,                                                                             \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                                  \
        };                                                                                         \
    })

#define n00b_stack_new_cap(T, N, ...)                                                               \
    ({                                                                                             \
        size_t _bl_rc = n00b_align_closest_pow2_ceil(n00b_max((size_t)(N), (size_t)1));             \
        (n00b_stack_t(T)){                                                                         \
            .data = n00b_alloc_array(T, _bl_rc                                                     \
                        __VA_OPT__(, .allocator = __VA_ARGS__)),                                   \
            .len  = 0,                                                                             \
            .cap  = _bl_rc,                                                                        \
            .lock = 0,                                                                             \
            __VA_OPT__(.allocator = __VA_ARGS__,)                                                  \
        };                                                                                         \
    })

#define n00b_stack_free(x)                                                                         \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        if (_bl_lp->data) {                                                                        \
            n00b_free(_bl_lp->data);                                                                \
        }                                                                                          \
        *_bl_lp = (typeof(x)){};                                                                   \
    })

// ============================================================================
// Core operations  (all locked)
// ============================================================================

/** @brief Push a value onto the top of the stack. */
#define n00b_stack_push(x, val)                                                                    \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        _n00b_stack_ensure_cap(_bl_lp, _bl_lp->len + 1);                                           \
        _bl_lp->data[_bl_lp->len++] = (val);                                                       \
        _n00b_stack_unlock(_bl_lp);                                                                \
    })

/** @brief Pop and return the top element.  Asserts non-empty. */
#define n00b_stack_pop(x)                                                                          \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        assert(_bl_lp->len > 0);                                                                   \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[--_bl_lp->len];                                 \
        _n00b_stack_unlock(_bl_lp);                                                                \
        _bl_r;                                                                                     \
    })

/** @brief Read the top element without removing it.  Asserts non-empty. */
#define n00b_stack_peek(x)                                                                         \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        assert(_bl_lp->len > 0);                                                                   \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_lp->len - 1];                               \
        _n00b_stack_unlock(_bl_lp);                                                                \
        _bl_r;                                                                                     \
    })

// ============================================================================
// Query  (locked)
// ============================================================================

/** @brief Number of elements on the stack. */
#define n00b_stack_len(x)                                                                          \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        size_t _bl_r = _bl_lp->len;                                                                \
        _n00b_stack_unlock(_bl_lp);                                                                \
        _bl_r;                                                                                     \
    })

/** @brief True if the stack has no elements. */
#define n00b_stack_is_empty(x)                                                                     \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        bool _bl_r = (_bl_lp->len == 0);                                                           \
        _n00b_stack_unlock(_bl_lp);                                                                \
        _bl_r;                                                                                     \
    })

// ============================================================================
// Utilities
// ============================================================================

/** @brief Reset the stack to empty, keeping allocated capacity. */
#define n00b_stack_clear(x)                                                                        \
    ({                                                                                             \
        auto _bl_lp = &(x);                                                                        \
        _n00b_stack_lock(_bl_lp);                                                                  \
        _bl_lp->len = 0;                                                                           \
        _n00b_stack_unlock(_bl_lp);                                                                \
    })

/**
 * @brief Iterate bottom-to-top.
 *        NOT locked — caller must ensure exclusive access.
 */
#define n00b_stack_foreach(x, var)                                                                 \
    for (typeof((x).data) var = (x).data; (var) < (x).data + (x).len; ++(var))
