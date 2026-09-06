#pragma once

#include <assert.h>
#include <string.h>

#include "lib/macros.h"
#include "lib/alloc.h"

#define ncc_array_tid(T) typeid(ncc_array, T)
#define ncc_array_t(T)   struct ncc_array_tid(T)

#define ncc_array_decl(T)                                                     \
    struct ncc_array_tid(T) {                                                 \
        T     *data;                                                           \
        size_t len;                                                            \
        size_t cap;                                                            \
    }

#define ncc_array_new(T, N)                                                   \
    ({                                                                         \
        (ncc_array_t(T)){                                                     \
            .len  = 0,                                                         \
            .cap  = (N),                                                       \
            .data = ncc_alloc_array(T, (N)),                                   \
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

#define ncc_array_foreach(arr, var)                                            \
    for (typeof((arr).data) var = (arr).data;                                  \
         (var) < (arr).data + (arr).len;                                       \
         ++(var))
