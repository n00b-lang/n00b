// Test header: n00b_result_t using _generic_struct for auto-deduplication.
//
// This replaces n00b_result_decl() — the struct definition is embedded
// in n00b_result_t() itself, and _generic_struct ensures only the first
// occurrence actually defines the struct.

#pragma once

#include <stdbool.h>
#include <assert.h>

typedef int n00b_err_t;

#define n00b_result_tid(T) typeid("result", T)

#define n00b_result_t(T)                                    \
    _generic_struct n00b_result_tid(T) {                    \
        bool is_ok;                                         \
        union {                                             \
            T          ok;                                  \
            n00b_err_t err;                                 \
        };                                                  \
    }

#define n00b_result_ok(T, x) \
    ((n00b_result_t(T)){ .is_ok = true, .ok = (x) })

#define n00b_result_err(T, e) \
    ((n00b_result_t(T)){ .is_ok = false, .err = (e) })

#define n00b_result_is_ok(x)  ((x).is_ok)
#define n00b_result_is_err(x) (!(x).is_ok)

#define n00b_result_get(x)                     \
    ({                                         \
        auto _bl_r = (x);                      \
        assert(_bl_r.is_ok);                   \
        _bl_r.ok;                              \
    })

#define n00b_result_get_err(x)                 \
    ({                                         \
        auto _bl_r = (x);                      \
        assert(!_bl_r.is_ok);                  \
        _bl_r.err;                             \
    })
