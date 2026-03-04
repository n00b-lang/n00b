#pragma once
/**
 * @file array.h
 * @brief Type-safe dynamic array (standalone extraction).
 *
 * Stripped-down version of ncc_array_t: no rwlock, no allocator field,
 * single-threaded only. Lock calls compile to no-ops via data_lock.h.
 *
 * Usage:
 *     ncc_array_decl(int);
 *     ncc_array_t(int) arr = ncc_array_new(int, 16);
 *     ncc_array_set(arr, 0, 42);
 *     int x = ncc_array_get(arr, 0);
 *     ncc_array_free(arr);
 */

#include <assert.h>
#include <string.h>

#include "core/macros.h"
#include "core/alloc.h"
#include "core/data_lock.h"

// ============================================================================
// Type definition
// ============================================================================

#define ncc_array_tid(T) typeid(ncc_array, T)
#define ncc_array_t(T)   struct ncc_array_tid(T)

#define ncc_array_decl(T)                                                     \
    struct ncc_array_tid(T) {                                                 \
        T                *data;                                                \
        size_t            len;                                                 \
        size_t            cap;                                                 \
        ncc_rwlock_t    *lock;                                                \
        ncc_allocator_t *allocator;                                           \
    }

// ============================================================================
// Construction / destruction
// ============================================================================

#define ncc_array_new(T, N, ...)              _ncc_array_new_sel(T, N, false, ##__VA_ARGS__)
#define ncc_array_new_locked(T, N, ...)       _ncc_array_new_sel(T, N, true, ##__VA_ARGS__)

#define _ncc_array_new_sel(T, N, locked, ...)                                 \
    ({                                                                         \
        (ncc_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = ncc_alloc_array(T, (N)),                                  \
            .lock = nullptr,                                                   \
            .allocator = nullptr,                                              \
        };                                                                     \
    })

#define ncc_array_checked_ptr(T, N, P)                                        \
    ({                                                                         \
        (ncc_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = (P),                                                       \
            .lock = nullptr,                                                   \
        };                                                                     \
    })

#define ncc_array_free(x)                                                     \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        if (_bl_ap->data) {                                                    \
            ncc_free(_bl_ap->data);                                           \
        }                                                                      \
        *_bl_ap = (typeof(x)){};                                               \
    })

// ============================================================================
// Access
// ============================================================================

#define ncc_array_get(x, i)                                                   \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_ap->len) {                                            \
            abort();                                                           \
        }                                                                      \
        typeof(*_bl_ap->data) _bl_r = _bl_ap->data[_bl_i];                     \
        _bl_r;                                                                 \
    })

#define ncc_array_set(x, i, val)                                              \
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

#define ncc_array_len(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->len;                                                           \
    })

#define ncc_array_cap(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->cap;                                                           \
    })

// ============================================================================
// Clone
// ============================================================================

#define ncc_array_clone(x)                                                    \
    ({                                                                         \
        auto _bl_sp = &(x);                                                    \
        typeof(x) _bl_copy = (typeof(x)){                                      \
            .len       = _bl_sp->len,                                          \
            .cap       = _bl_sp->cap,                                          \
            .data      = ncc_alloc_array(typeof(*_bl_sp->data), _bl_sp->cap), \
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

#define ncc_array_foreach(arr, var)                                            \
    for (typeof((arr).data) var = (arr).data;                                  \
         (var) < (arr).data + (arr).len;                                       \
         ++(var))
