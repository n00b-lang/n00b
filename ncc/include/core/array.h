#pragma once
/**
 * @file array.h
 * @brief Type-safe dynamic array (standalone extraction).
 *
 * Stripped-down version of n00b_array_t: no rwlock, no allocator field,
 * single-threaded only. Lock calls compile to no-ops via data_lock.h.
 *
 * Usage:
 *     n00b_array_decl(int);
 *     n00b_array_t(int) arr = n00b_array_new(int, 16);
 *     n00b_array_set(arr, 0, 42);
 *     int x = n00b_array_get(arr, 0);
 *     n00b_array_free(arr);
 */

#include <assert.h>
#include <string.h>

#include "core/macros.h"
#include "core/alloc.h"
#include "core/data_lock.h"

// ============================================================================
// Type definition
// ============================================================================

#define n00b_array_tid(T) typeid(n00b_array, T)
#define n00b_array_t(T)   struct n00b_array_tid(T)

#define n00b_array_decl(T)                                                     \
    struct n00b_array_tid(T) {                                                 \
        T                *data;                                                \
        size_t            len;                                                 \
        size_t            cap;                                                 \
        n00b_rwlock_t    *lock;                                                \
        n00b_allocator_t *allocator;                                           \
    }

// ============================================================================
// Construction / destruction
// ============================================================================

#define n00b_array_new(T, N, ...)              _n00b_array_new_sel(T, N, false, ##__VA_ARGS__)
#define n00b_array_new_locked(T, N, ...)       _n00b_array_new_sel(T, N, true, ##__VA_ARGS__)

#define _n00b_array_new_sel(T, N, locked, ...)                                 \
    ({                                                                         \
        (n00b_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = n00b_alloc_array(T, (N)),                                  \
            .lock = nullptr,                                                   \
            .allocator = nullptr,                                              \
        };                                                                     \
    })

#define n00b_array_checked_ptr(T, N, P)                                        \
    ({                                                                         \
        (n00b_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = (P),                                                       \
            .lock = nullptr,                                                   \
        };                                                                     \
    })

#define n00b_array_free(x)                                                     \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        if (_bl_ap->data) {                                                    \
            n00b_free(_bl_ap->data);                                           \
        }                                                                      \
        *_bl_ap = (typeof(x)){};                                               \
    })

// ============================================================================
// Access
// ============================================================================

#define n00b_array_get(x, i)                                                   \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_ap->len) {                                            \
            abort();                                                           \
        }                                                                      \
        typeof(*_bl_ap->data) _bl_r = _bl_ap->data[_bl_i];                     \
        _bl_r;                                                                 \
    })

#define n00b_array_set(x, i, val)                                              \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_ap->cap) {                                            \
            abort();                                                           \
        }                                                                      \
        if (_bl_i >= _bl_ap->len) {                                            \
            _bl_ap->len = _bl_i + 1;                                           \
        }                                                                      \
        _bl_ap->data[_bl_i] = (val);                                           \
    })

#define n00b_array_len(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->len;                                                           \
    })

#define n00b_array_cap(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->cap;                                                           \
    })

// ============================================================================
// Clone
// ============================================================================

#define n00b_array_clone(x)                                                    \
    ({                                                                         \
        auto _bl_sp = &(x);                                                    \
        typeof(x) _bl_copy = (typeof(x)){                                      \
            .len       = _bl_sp->len,                                          \
            .cap       = _bl_sp->cap,                                          \
            .data      = n00b_alloc_array(typeof(*_bl_sp->data), _bl_sp->cap), \
            .lock      = nullptr,                                              \
            .allocator = nullptr,                                              \
        };                                                                     \
        memcpy(_bl_copy.data, _bl_sp->data,                                    \
               _bl_sp->len * sizeof(_bl_sp->data[0]));                         \
        _bl_copy;                                                              \
    })

// ============================================================================
// Iteration
// ============================================================================

#define n00b_array_foreach(arr, var)                                            \
    for (typeof((arr).data) var = (arr).data;                                  \
         (var) < (arr).data + (arr).len;                                       \
         ++(var))
