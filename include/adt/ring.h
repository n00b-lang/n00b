/**
 * @file ring.h
 * @brief Type-safe fixed-capacity ring buffer with drop-oldest overflow.
 *
 * @c n00b_ring_t(T) provides a bounded FIFO queue for best-effort
 * diagnostic and streaming paths.  When full, @c n00b_ring_push drops
 * the oldest item, increments a drop counter, and stores the new item.
 *
 * The backing storage is an @c n00b_array_t(T).  Rings are protected by
 * an embedded @c n00b_mutex_t pointer by default; use
 * @c n00b_ring_new_private for single-owner/SPSC paths that have
 * external synchronization.
 */
#pragma once

#include "n00b.h"
#include "adt/array.h"
#include "core/alloc.h"
#include "core/mutex.h"

#define n00b_ring_tid(T) typeid("n00b_ring", T)

/**
 * @brief Reference a ring type for element type @p T.
 * @param T Element type.
 */
#define n00b_ring_t(T)                                                                         \
    _generic_struct n00b_ring_tid(T) {                                                         \
        n00b_array_t(T)   storage;                                                             \
        size_t            head;                                                                \
        size_t            len;                                                                 \
        uint64_t          drop_count;                                                          \
        n00b_mutex_t     *lock;                                                                \
        n00b_allocator_t *allocator;                                                           \
    }

#define _n00b_ring_lock(xptr)                                                                  \
    do {                                                                                       \
        if ((xptr)->lock != nullptr) {                                                         \
            n00b_mutex_lock((xptr)->lock);                                                      \
        }                                                                                      \
    } while (0)

#define _n00b_ring_unlock(xptr)                                                                \
    do {                                                                                       \
        if ((xptr)->lock != nullptr) {                                                         \
            n00b_mutex_unlock((xptr)->lock);                                                    \
        }                                                                                      \
    } while (0)

/**
 * @brief Create a mutex-protected ring with fixed capacity @p N.
 *
 * Capacity zero is rounded up to one so push/pop operations never
 * divide by zero.
 */
#define n00b_ring_new(T, N, ...)              _n00b_ring_new_sel(T, N, true, ##__VA_ARGS__)

/** @brief Create a ring without an internal mutex. */
#define n00b_ring_new_private(T, N, ...)      _n00b_ring_new_sel(T, N, false, ##__VA_ARGS__)

#define _n00b_ring_new_sel(T, N, locked, ...)                                                 \
    ({                                                                                         \
        size_t            _ring_cap  = (size_t)(N);                                            \
        n00b_alloc_opts_t _ring_opts = (n00b_alloc_opts_t){__VA_ARGS__};                       \
        if (_ring_cap == 0) {                                                                  \
            _ring_cap = 1;                                                                     \
        }                                                                                      \
        n00b_mutex_t *_ring_lock = nullptr;                                                    \
        if (locked) {                                                                          \
            _ring_lock = n00b_alloc_with_opts(                                                 \
                n00b_mutex_t,                                                                  \
                &(n00b_alloc_opts_t){.allocator = _ring_opts.allocator});                      \
            n00b_mutex_init(_ring_lock);                                                       \
        }                                                                                      \
        (n00b_ring_t(T)){                                                                      \
            .storage = n00b_array_new(T,                                                       \
                                      _ring_cap,                                               \
                                      .allocator = _ring_opts.allocator,                       \
                                      .scan_kind = _ring_opts.scan_kind,                       \
                                      .scan_cb = _ring_opts.scan_cb,                           \
                                      .scan_user = _ring_opts.scan_user),                      \
            .head       = 0,                                                                   \
            .len        = 0,                                                                   \
            .drop_count = 0,                                                                   \
            .lock       = _ring_lock,                                                          \
            .allocator  = _ring_opts.allocator,                                                \
        };                                                                                     \
    })

/** @brief Free ring backing storage and clear the ring. */
#define n00b_ring_free(x)                                                                      \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        _n00b_ring_lock(_bl_rp);                                                               \
        n00b_array_free(_bl_rp->storage);                                                       \
        _n00b_ring_unlock(_bl_rp);                                                             \
        if (_bl_rp->lock != nullptr) {                                                         \
            n00b_free(_bl_rp->lock);                                                           \
        }                                                                                      \
        *_bl_rp = (typeof(x)){};                                                               \
    })

/** @brief Ring capacity. */
#define n00b_ring_cap(x)                                                                       \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        _n00b_ring_lock(_bl_rp);                                                               \
        size_t _bl_result = _bl_rp->storage.cap;                                               \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })

/** @brief Current item count. */
#define n00b_ring_len(x)                                                                       \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        _n00b_ring_lock(_bl_rp);                                                               \
        size_t _bl_result = _bl_rp->len;                                                       \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })

/** @brief Number of items dropped by full-ring pushes. */
#define n00b_ring_drop_count(x)                                                                \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        _n00b_ring_lock(_bl_rp);                                                               \
        uint64_t _bl_result = _bl_rp->drop_count;                                              \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })

/** @brief True when the ring holds no items. */
#define n00b_ring_is_empty(x) (n00b_ring_len(x) == 0)

/** @brief Push @p val, dropping the oldest item first when full. */
#define n00b_ring_push(x, val)                                                                 \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        typeof(*_bl_rp->storage.data) _bl_val = (val);                                         \
        bool _bl_result = false;                                                               \
        _n00b_ring_lock(_bl_rp);                                                               \
        if (_bl_rp->storage.data != nullptr && _bl_rp->storage.cap != 0) {                     \
            if (_bl_rp->len == _bl_rp->storage.cap) {                                          \
                _bl_rp->head = (_bl_rp->head + 1) % _bl_rp->storage.cap;                       \
                _bl_rp->len--;                                                                 \
                _bl_rp->drop_count++;                                                          \
            }                                                                                  \
            size_t _bl_tail = (_bl_rp->head + _bl_rp->len) % _bl_rp->storage.cap;              \
            _bl_rp->storage.data[_bl_tail] = _bl_val;                                          \
            _bl_rp->len++;                                                                     \
            _bl_result = true;                                                                 \
        }                                                                                      \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })

/**
 * @brief Pop the oldest item into @p out.
 * @return true if an item was popped, false if the ring was empty.
 */
#define n00b_ring_pop(x, out)                                                                  \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        typeof(_bl_rp->storage.data) _bl_out = (out);                                          \
        bool _bl_result = false;                                                               \
        _n00b_ring_lock(_bl_rp);                                                               \
        if (_bl_rp->storage.data != nullptr && _bl_rp->storage.cap != 0 && _bl_rp->len != 0) { \
            if (_bl_out != nullptr) {                                                          \
                *_bl_out = _bl_rp->storage.data[_bl_rp->head];                                 \
            }                                                                                  \
            _bl_rp->head = (_bl_rp->head + 1) % _bl_rp->storage.cap;                           \
            _bl_rp->len--;                                                                     \
            if (_bl_rp->len == 0) {                                                            \
                _bl_rp->head = 0;                                                              \
            }                                                                                  \
            _bl_result = true;                                                                 \
        }                                                                                      \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })

/**
 * @brief Copy the oldest item into @p out without removing it.
 * @return true if an item was present, false if the ring was empty.
 */
#define n00b_ring_peek(x, out)                                                                 \
    ({                                                                                         \
        auto _bl_rp = &(x);                                                                    \
        typeof(_bl_rp->storage.data) _bl_out = (out);                                          \
        bool _bl_result = false;                                                               \
        _n00b_ring_lock(_bl_rp);                                                               \
        if (_bl_rp->storage.data != nullptr && _bl_rp->storage.cap != 0 && _bl_rp->len != 0) { \
            if (_bl_out != nullptr) {                                                          \
                *_bl_out = _bl_rp->storage.data[_bl_rp->head];                                 \
            }                                                                                  \
            _bl_result = true;                                                                 \
        }                                                                                      \
        _n00b_ring_unlock(_bl_rp);                                                             \
        _bl_result;                                                                            \
    })
