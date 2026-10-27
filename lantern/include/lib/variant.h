#pragma once

#include <assert.h>
#include <stddef.h>

#define core_variant_decl_2(Name, T1, T2)                                        \
    typedef struct Name {                                                        \
        size_t tag;                                                              \
        union {                                                                  \
            T1 alt1;                                                             \
            T2 alt2;                                                             \
        } value;                                                                 \
    } Name

#define core_variant_decl_3(Name, T1, T2, T3)                                   \
    typedef struct Name {                                                        \
        size_t tag;                                                              \
        union {                                                                  \
            T1 alt1;                                                             \
            T2 alt2;                                                             \
            T3 alt3;                                                             \
        } value;                                                                 \
    } Name

#define core_variant_decl_4(Name, T1, T2, T3, T4)                              \
    typedef struct Name {                                                        \
        size_t tag;                                                              \
        union {                                                                  \
            T1 alt1;                                                             \
            T2 alt2;                                                             \
            T3 alt3;                                                             \
            T4 alt4;                                                             \
        } value;                                                                 \
    } Name

#define _CORE_VARIANT_DECL_SELECT(_1, _2, _3, _4, _5, NAME, ...) NAME
#define core_variant_decl(...)                                                   \
    _CORE_VARIANT_DECL_SELECT(__VA_ARGS__, core_variant_decl_4,                  \
                              core_variant_decl_3, core_variant_decl_2)          \
    (__VA_ARGS__)

#define core_variant_empty(Name) ((Name){0})

#define core_variant_make_1(Name, val)                                           \
    ((Name){.tag = 1, .value.alt1 = (val)})
#define core_variant_make_2(Name, val)                                           \
    ((Name){.tag = 2, .value.alt2 = (val)})
#define core_variant_make_3(Name, val)                                           \
    ((Name){.tag = 3, .value.alt3 = (val)})
#define core_variant_make_4(Name, val)                                           \
    ((Name){.tag = 4, .value.alt4 = (val)})

#define core_variant_is(variant_value, index)                                    \
    ((variant_value).tag == (size_t)(index))

#define core_variant_get_1(variant_value)                                        \
    ({                                                                           \
        auto _core_variant = (variant_value);                                    \
        assert(_core_variant.tag == 1);                                          \
        _core_variant.value.alt1;                                                \
    })
#define core_variant_get_2(variant_value)                                        \
    ({                                                                           \
        auto _core_variant = (variant_value);                                    \
        assert(_core_variant.tag == 2);                                          \
        _core_variant.value.alt2;                                                \
    })
#define core_variant_get_3(variant_value)                                        \
    ({                                                                           \
        auto _core_variant = (variant_value);                                    \
        assert(_core_variant.tag == 3);                                          \
        _core_variant.value.alt3;                                                \
    })
#define core_variant_get_4(variant_value)                                        \
    ({                                                                           \
        auto _core_variant = (variant_value);                                    \
        assert(_core_variant.tag == 4);                                          \
        _core_variant.value.alt4;                                                \
    })
