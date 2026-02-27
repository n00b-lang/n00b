#pragma once

#include <assert.h>
#include "n00b.h"
#include "core/macros.h"

// Pointer types need typedef aliases for token-paste to work.
typedef void       *n00b_voidptr_t;
typedef char       *n00b_charptr_t;
typedef const char *n00b_const_charptr_t;

#define n00b_option_tid(T) typeid(n00b_option, T)
#define n00b_option_t(T)   struct n00b_option_tid(T)

#define n00b_option_decl(T)                       \
    struct n00b_option_tid(T) {                    \
        bool has_value;                            \
        T    value;                                \
    }

#define n00b_option_set(T, x)                     \
    ((n00b_option_t(T)){                           \
        .has_value = true,                         \
        .value     = (x),                          \
    })

#define n00b_option_none(T)                        \
    ((n00b_option_t(T)){                           \
        .has_value = false,                        \
        .value     = {},                           \
    })

#define n00b_option_get(x)                         \
    ({                                             \
        auto _bl_o = (x);                          \
        assert(_bl_o.has_value);                   \
        _bl_o.value;                               \
    })

#define n00b_option_is_set(x) ((x).has_value)

#define n00b_option_get_or_else(x, y)              \
    ({                                             \
        auto _bl_o = (x);                          \
        _bl_o.has_value ? _bl_o.value : (y);       \
    })

#define n00b_option_match(x, set, none) ((x).has_value ? (set) : (none))

#define n00b_option_from_nullable(T, ptr)          \
    ({                                             \
        n00b_option_t(T) _opt;                     \
        auto _p = (ptr);                           \
        if (_p) {                                  \
            _opt = (n00b_option_t(T)){.has_value = true, .value = _p}; \
        } else {                                   \
            _opt = (n00b_option_t(T)){.has_value = false}; \
        }                                          \
        _opt;                                      \
    })

// Common option types.
n00b_option_decl(int);
n00b_option_decl(int32_t);
n00b_option_decl(uint32_t);
n00b_option_decl(int64_t);
n00b_option_decl(size_t);
n00b_option_decl(n00b_codepoint_t);
n00b_option_decl(n00b_voidptr_t);
n00b_option_decl(n00b_charptr_t);
n00b_option_decl(n00b_const_charptr_t);
