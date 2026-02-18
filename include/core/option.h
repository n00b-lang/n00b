/**
 * @file option.h
 * @brief Type-safe optional values using typeid().
 *
 * @c n00b_option_t(T) represents a value that may or may not be present.
 */
#pragma once

#define n00b_option_tid(T) typeid("option", T)
#define n00b_option_t(T)   struct n00b_option_tid(T)

#define n00b_option_decl(T)                                                                    \
    struct n00b_option_tid(T) {                                                                \
        bool has_value;                                                                        \
        T    value;                                                                            \
    }

#define n00b_option_set(T, x)                                                                  \
    ((n00b_option_t(T)){                                                                       \
        .has_value = true,                                                                     \
        .value     = (x),                                                                      \
    })
#define n00b_option_none(T)                                                                    \
    ((n00b_option_t(T)){                                                                       \
        .has_value = false,                                                                    \
        .value     = {},                                                                       \
    })
#define n00b_option_get(x)                                                                     \
    ({                                                                                         \
        auto _bl_o = (x);                                                                      \
        *(_bl_o.has_value ? &_bl_o.value : (void *)0);                                          \
    })
#define n00b_option_is_set(x) ((x).has_value)
#define n00b_option_get_or_else(x, y)                                                          \
    ({                                                                                         \
        auto _bl_o = (x);                                                                      \
        _bl_o.has_value ? _bl_o.value : (y);                                                    \
    })
#define n00b_option_match(x, set, none) ((x).has_value ? (set) : (none))

/**
 * @brief Convert a nullable pointer to an option_t.
 *
 * @c n00b_option_from_nullable(T, ptr) produces a "some" option if
 * @p ptr is non-null, or a "none" option if null.
 */
#define n00b_option_from_nullable(T, ptr)                                                      \
    ({                                                                                         \
        n00b_option_t(T) _opt;                                                                 \
        auto _p = (ptr);                                                                       \
        if (_p) {                                                                              \
            _opt = (n00b_option_t(T)){.has_value = true, .value = _p};                         \
        }                                                                                      \
        else {                                                                                 \
            _opt = (n00b_option_t(T)){.has_value = false};                                     \
        }                                                                                      \
        _opt;                                                                                  \
    })
