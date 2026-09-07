#pragma once

#include <assert.h>
#include <string.h>

#include "core.h"
#include "lib/macros.h"
#include "lib/alloc.h"
#include "lib/align.h"
#include "lib/option.h"
#include "lib/array.h"
#include "lib/string.h"

#define CORE_DEFAULT_LIST_SZ 16

#define core_list_tid(T) typeid(core_list, T)
#define core_list_t(T)   struct core_list_tid(T)

#define core_list_decl(T)                                                      \
    struct core_list_tid(T) {                                                  \
        T                *data;                                                \
        size_t            len;                                                 \
        size_t            cap;                                                 \
    }

core_list_decl(int);
core_list_decl(int32_t);
core_list_decl(core_string_t);
core_list_decl(core_const_charptr_t);

#define _core_list_ensure_cap(xptr, needed)                                    \
    do {                                                                       \
        size_t _bl_need = (needed);                                            \
        if (_bl_need > (xptr)->cap) {                                          \
            size_t _bl_nc = core_align_closest_pow2_ceil(_bl_need);            \
            typeof((xptr)->data) _bl_nd =                                      \
                (typeof((xptr)->data))core_alloc_size(_bl_nc, sizeof(*(xptr)->data)); \
            if ((xptr)->len > 0) {                                             \
                memcpy(_bl_nd, (xptr)->data,                                   \
                       (xptr)->len * sizeof(*(xptr)->data));                   \
            }                                                                  \
            if ((xptr)->data) {                                                \
                core_free((xptr)->data);                                            \
            }                                                                  \
            (xptr)->data = _bl_nd;                                             \
            (xptr)->cap  = _bl_nc;                                             \
        }                                                                      \
    } while (0)

#define core_list_new(T)                                                       \
    ({                                                                         \
        (core_list_t(T)){                                                      \
            .data      = core_alloc_array(T, CORE_DEFAULT_LIST_SZ),            \
            .len       = 0,                                                    \
            .cap       = CORE_DEFAULT_LIST_SZ,                                 \
        };                                                                     \
    })

#define core_list_new_cap(T, N)                                               \
    ({                                                                         \
        size_t _bl_rc = core_align_closest_pow2_ceil(                          \
                            core_max((size_t)(N), (size_t)1));                 \
        (core_list_t(T)){                                                      \
            .data      = core_alloc_array(T, _bl_rc),                          \
            .len       = 0,                                                    \
            .cap       = _bl_rc,                                               \
        };                                                                     \
    })

#define core_list_free(x)                                                      \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        if (_bl_lp->data) {                                                    \
            core_free(_bl_lp->data);                                                \
        }                                                                      \
        *_bl_lp = (typeof(x)){};                                               \
    })

#define core_list_len(x)                                                       \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        _bl_lp->len;                                                           \
    })

#define core_list_cap(x)                                                       \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        _bl_lp->cap;                                                           \
    })

#define core_list_get(x, i)                                                    \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_lp->len) {                                            \
            abort();                                                           \
        }                                                                      \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                     \
        _bl_r;                                                                 \
    })

#define core_list_set(x, i, val)                                               \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        if (_bl_i >= _bl_lp->len) {                                            \
            abort();                                                           \
        }                                                                      \
        _bl_lp->data[_bl_i] = (val);                                           \
    })

#define core_list_push(x, val)                                                 \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        _core_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                        \
        _bl_lp->data[_bl_lp->len++] = (val);                                   \
    })

#define core_list_pop(T, x)                                                    \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        core_option_t(T) _bl_opt;                                              \
        if (_bl_lp->len > 0) {                                                 \
            _bl_opt = core_option_set(T, _bl_lp->data[--_bl_lp->len]);         \
        }                                                                      \
        else {                                                                 \
            _bl_opt = core_option_none(T);                                     \
        }                                                                      \
        _bl_opt;                                                               \
    })

#define core_list_push_front(x, val)                                           \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        _core_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                        \
        if (_bl_lp->len > 0) {                                                 \
            memmove(_bl_lp->data + 1, _bl_lp->data,                            \
                    _bl_lp->len * sizeof(*_bl_lp->data));                      \
        }                                                                      \
        _bl_lp->data[0] = (val);                                               \
        _bl_lp->len++;                                                         \
    })

#define core_list_pop_front(T, x)                                              \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        core_option_t(T) _bl_opt;                                              \
        if (_bl_lp->len > 0) {                                                 \
            T _bl_r = _bl_lp->data[0];                                         \
            _bl_lp->len--;                                                     \
            if (_bl_lp->len > 0) {                                             \
                memmove(_bl_lp->data, _bl_lp->data + 1,                        \
                        _bl_lp->len * sizeof(*_bl_lp->data));                  \
            }                                                                  \
            _bl_opt = core_option_set(T, _bl_r);                               \
        }                                                                      \
        else {                                                                 \
            _bl_opt = core_option_none(T);                                     \
        }                                                                      \
        _bl_opt;                                                               \
    })

#define core_list_insert(x, i, val)                                            \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        assert(_bl_i <= _bl_lp->len);                                          \
        _core_list_ensure_cap(_bl_lp, _bl_lp->len + 1);                        \
        if (_bl_i < _bl_lp->len) {                                             \
            memmove(_bl_lp->data + _bl_i + 1,                                  \
                    _bl_lp->data + _bl_i,                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));            \
        }                                                                      \
        _bl_lp->data[_bl_i] = (val);                                           \
        _bl_lp->len++;                                                         \
    })

#define core_list_delete(x, i)                                                 \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        size_t _bl_i = (i);                                                    \
        assert(_bl_i < _bl_lp->len);                                           \
        typeof(*_bl_lp->data) _bl_r = _bl_lp->data[_bl_i];                     \
        _bl_lp->len--;                                                         \
        if (_bl_i < _bl_lp->len) {                                             \
            memmove(_bl_lp->data + _bl_i,                                      \
                    _bl_lp->data + _bl_i + 1,                                  \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));            \
        }                                                                      \
        _bl_r;                                                                 \
    })

#define core_list_insert_list(x, i, src)                                       \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        auto _bl_sp = &(src);                                                  \
        size_t _bl_i  = (i);                                                   \
        size_t _bl_sn = _bl_sp->len;                                           \
        assert(_bl_i <= _bl_lp->len);                                          \
        _core_list_ensure_cap(_bl_lp, _bl_lp->len + _bl_sn);                   \
        if (_bl_i < _bl_lp->len) {                                             \
            memmove(_bl_lp->data + _bl_i + _bl_sn,                             \
                    _bl_lp->data + _bl_i,                                      \
                    (_bl_lp->len - _bl_i) * sizeof(*_bl_lp->data));            \
        }                                                                      \
        memcpy(_bl_lp->data + _bl_i, _bl_sp->data,                             \
               _bl_sn * sizeof(*_bl_lp->data));                                \
        _bl_lp->len += _bl_sn;                                                 \
    })

#define core_list_delete_range(x, start, count)                                \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        size_t _bl_s = (start);                                                \
        size_t _bl_c = (count);                                                \
        assert(_bl_s + _bl_c <= _bl_lp->len);                                  \
        size_t _bl_tail = _bl_lp->len - _bl_s - _bl_c;                         \
        if (_bl_tail > 0) {                                                    \
            memmove(_bl_lp->data + _bl_s,                                      \
                    _bl_lp->data + _bl_s + _bl_c,                              \
                    _bl_tail * sizeof(*_bl_lp->data));                         \
        }                                                                      \
        _bl_lp->len -= _bl_c;                                                  \
    })

#define core_list_concat(a, b)                                                 \
    ({                                                                         \
        auto      _bl_ap  = &(a);                                              \
        auto      _bl_bp  = &(b);                                              \
        size_t    _bl_tl  = _bl_ap->len + _bl_bp->len;                         \
        size_t    _bl_tc  = core_align_closest_pow2_ceil(                      \
                                core_max(_bl_tl, (size_t)1));                  \
        typeof(a) _bl_new = {                                                  \
            .data      = (typeof(_bl_ap->data))core_alloc_size(                   \
                             _bl_tc, sizeof(*_bl_ap->data)),                   \
            .len       = _bl_tl,                                               \
            .cap       = _bl_tc,                                               \
        };                                                                     \
        if (_bl_ap->len > 0) {                                                 \
            memcpy(_bl_new.data, _bl_ap->data,                                 \
                   _bl_ap->len * sizeof(*_bl_ap->data));                       \
        }                                                                      \
        if (_bl_bp->len > 0) {                                                 \
            memcpy(_bl_new.data + _bl_ap->len,                                 \
                   _bl_bp->data,                                               \
                   _bl_bp->len * sizeof(*_bl_bp->data));                       \
        }                                                                      \
        _bl_new;                                                               \
    })

#define core_list_find(x, val)                                                 \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        typeof(*_bl_lp->data) _bl_v = (val);                                   \
        core_option_t(size_t) _bl_r = core_option_none(size_t);               \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                 \
            if (_bl_lp->data[_bl_i] == _bl_v) {                                \
                _bl_r = core_option_set(size_t, _bl_i);                        \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        _bl_r;                                                                 \
    })

#define core_list_sort(x, cmp)                                                 \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        if (_bl_lp->len > 1) {                                                 \
            qsort(_bl_lp->data, _bl_lp->len,                                   \
                  sizeof(*_bl_lp->data), (cmp));                               \
        }                                                                      \
    })

#define core_list_remove_all(x, val)                                           \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        typeof(*_bl_lp->data) _bl_v = (val);                                   \
        size_t                _bl_w = 0;                                       \
        for (size_t _bl_i = 0; _bl_i < _bl_lp->len; _bl_i++) {                 \
            if (_bl_lp->data[_bl_i] != _bl_v) {                                \
                _bl_lp->data[_bl_w++] = _bl_lp->data[_bl_i];                   \
            }                                                                  \
        }                                                                      \
        size_t _bl_removed = _bl_lp->len - _bl_w;                              \
        _bl_lp->len        = _bl_w;                                            \
        _bl_removed;                                                           \
    })

#define core_list_clone(x)                                                     \
    ({                                                                         \
        auto      _bl_sp  = &(x);                                              \
        size_t    _bl_nc  = core_max(_bl_sp->cap, (size_t)1);                  \
        typeof(x) _bl_new = {                                                  \
            .data      = (typeof(_bl_sp->data))core_alloc_size(                   \
                             _bl_nc, sizeof(*_bl_sp->data)),                   \
            .len       = _bl_sp->len,                                          \
            .cap       = _bl_nc,                                               \
        };                                                                     \
        if (_bl_sp->len > 0) {                                                 \
            memcpy(_bl_new.data, _bl_sp->data,                                 \
                   _bl_sp->len * sizeof(*_bl_sp->data));                       \
        }                                                                      \
        _bl_new;                                                               \
    })

#define core_list_is_empty(x) (core_list_len(x) == 0)

#define core_list_clear(x)                                                     \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        _bl_lp->len = 0;                                                       \
    })

#define core_list_foreach(x, var)                                              \
    for (typeof((x).data) var = (x).data;                                      \
         (var) < (x).data + (x).len;                                           \
         ++(var))

#define core_list_to_array(T, x)                                               \
    ({                                                                         \
        auto _bl_lp = &(x);                                                    \
        core_array_t(T) _bl_arr = {                                            \
            .data = _bl_lp->data,                                              \
            .len  = _bl_lp->len,                                               \
            .cap  = _bl_lp->cap,                                               \
        };                                                                     \
        _bl_lp->data      = nullptr;                                           \
        _bl_lp->len       = 0;                                                 \
        _bl_lp->cap       = 0;                                                 \
        _bl_arr;                                                               \
    })
