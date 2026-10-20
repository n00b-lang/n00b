#pragma once

#include <assert.h>
#include <string.h>

#include "lib/macros.h"
#include "lib/alloc.h"

#define core_array_tid(T) typeid(core_array, T)
#define core_array_t(T)   struct core_array_tid(T)

#define core_array_decl(T)                                                     \
    struct core_array_tid(T) {                                                 \
        T     *data;                                                           \
        size_t len;                                                            \
        size_t cap;                                                            \
    }

#define core_array_new(T, N)                                                   \
    ({                                                                         \
        (core_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = core_alloc_array(T, (N)),                                   \
        };                                                                     \
    })

#define core_array_free(x)                                                     \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        if (_bl_ap->data) {                                                    \
            core_free(_bl_ap->data);                                           \
        }                                                                      \
        *_bl_ap = (typeof(x)){};                                               \
    })

#define core_array_get(x, i)                                                   \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_ap->len) {                                            \
            abort();                                                           \
        }                                                                      \
        typeof(*_bl_ap->data) _bl_r = _bl_ap->data[_bl_i];                     \
        _bl_r;                                                                 \
    })

#define core_array_set(x, i, val)                                              \
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

#define core_array_len(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->len;                                                           \
    })

#define core_array_cap(x)                                                      \
    ({                                                                         \
        auto _bl_ap = &(x);                                                    \
        _bl_ap->cap;                                                           \
    })

#define core_array_foreach(arr, var)                                            \
    for (typeof((arr).data) var = (arr).data;                                  \
         (var) < (arr).data + (arr).len;                                       \
         ++(var))
