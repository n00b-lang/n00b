#pragma once

#include <assert.h>
#include "core.h"
#include "lib/macros.h"

typedef void       *core_voidptr_t;

#define core_option_tid(T) typeid(core_option, T)
#define core_option_t(T)   struct core_option_tid(T)

#define core_option_decl(T)                       \
    struct core_option_tid(T) {                    \
        bool has_value;                            \
        T    value;                                \
    }

#define core_option_set(T, x)                     \
    ((core_option_t(T)){                           \
        .has_value = true,                         \
        .value     = (x),                          \
    })

#define core_option_none(T)                        \
    ((core_option_t(T)){                           \
        .has_value = false,                        \
        .value     = {},                           \
    })

#define core_option_get(x)                         \
    ({                                             \
        auto _bl_o = (x);                          \
        assert(_bl_o.has_value);                   \
        _bl_o.value;                               \
    })

#define core_option_is_set(x) ((x).has_value)

#define core_option_get_or_else(x, y)              \
    ({                                             \
        auto _bl_o = (x);                          \
        _bl_o.has_value ? _bl_o.value : (y);       \
    })

#define core_option_match(x, set, none) ((x).has_value ? (set) : (none))

#define core_option_from_nullable(T, ptr)          \
    ({                                             \
        core_option_t(T) _opt;                     \
        T _p = (ptr);                              \
        if (_p) {                                  \
            _opt = (core_option_t(T)){.has_value = true, .value = _p}; \
        } else {                                   \
            _opt = (core_option_t(T)){.has_value = false}; \
        }                                          \
        _opt;                                      \
    })

core_option_decl(int);
core_option_decl(int32_t);
core_option_decl(uint32_t);
core_option_decl(int64_t);
core_option_decl(size_t);
core_option_decl(core_codepoint_t);
core_option_decl(core_voidptr_t);
