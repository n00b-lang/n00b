#pragma once

#include <assert.h>
#include "core/option.h"  // for n00b_voidptr_t typedef
#include "core/macros.h"

typedef int n00b_err_t;

#define n00b_result_tid(T) typeid(n00b_result, T)
#define n00b_result_t(T)   struct n00b_result_tid(T)

#define n00b_result_decl(T)                        \
    struct n00b_result_tid(T) {                    \
        bool is_ok;                                \
        union {                                    \
            T          ok;                         \
            n00b_err_t err;                        \
        };                                         \
    }

#define n00b_result_ok(T, x) \
    ((n00b_result_t(T)){ .is_ok = true, .ok = (x) })

#define n00b_result_err(T, e) \
    ((n00b_result_t(T)){ .is_ok = false, .err = (e) })

#define n00b_result_is_ok(x)  ((x).is_ok)
#define n00b_result_is_err(x) (!(x).is_ok)

#define n00b_result_get(x)                         \
    ({                                             \
        auto _bl_r = (x);                          \
        assert(_bl_r.is_ok);                       \
        _bl_r.ok;                                  \
    })

#define n00b_result_get_err(x)                     \
    ({                                             \
        auto _bl_r = (x);                          \
        assert(!_bl_r.is_ok);                      \
        _bl_r.err;                                 \
    })

#define n00b_result_get_or_else(x, y)              \
    ({                                             \
        auto _bl_r = (x);                          \
        _bl_r.is_ok ? _bl_r.ok : (y);             \
    })

#define n00b_result_match(x, ok_expr, err_expr) ((x).is_ok ? (ok_expr) : (err_expr))

n00b_result_decl(int);
n00b_result_decl(int64_t);
n00b_result_decl(n00b_voidptr_t);
n00b_result_decl(uint64_t);
n00b_result_decl(bool);
n00b_result_decl(uint8_t);
n00b_result_decl(size_t);
