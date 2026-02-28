/**
 * @file variant.h
 * @brief Type-safe tagged union.
 *
 * @c n00b_variant_t(...) represents a value that can be one of several types.
 *
 * The selector uses `typehash(T)` — a compile-time 64-bit hash of the
 * normalized type — for O(1) integer comparison instead of `strcmp`.
 */
#pragma once

#include "core/macros.h"
#include <stdint.h>
#include <assert.h>

#define n00b_variant_tid(...) typeid("n00b_variant", __VA_ARGS__)
#define n00b_variant_t(...)   struct n00b_variant_tid(__VA_ARGS__)

#define N00B_VARIANT_FIELD(T)       constexpr_paste(field_, typeid(T))
#define _N00B_VARIANT_FIELD_DECL(T) T N00B_VARIANT_FIELD(T);

/**
 * @brief Declare + define a variant type. Use in variable declarations.
 * @param ...  Types held inside the variant.
 */
#define n00b_variant_decl(...)                                                                 \
    n00b_variant_t(__VA_ARGS__)                                                                \
    {                                                                                          \
        uint64_t selector;                                                                     \
        union {                                                                                \
            N00B_MAP(_N00B_VARIANT_FIELD_DECL, __VA_ARGS__)                                    \
        } value;                                                                               \
    }

// All public macros accept either a value or a pointer.
// Values are converted to pointers internally.

#define n00b_variant_empty(T) (T){.selector = 0}

#define _n00b_variant_set_ptr(vptr, T, val)                                                    \
    (*(vptr) = (typeof(*(vptr))){                                                              \
         .selector                    = typehash(T),                                           \
         .value.N00B_VARIANT_FIELD(T) = (val),                                                 \
     })

#define n00b_variant_set(VTYPE, T, val)                                                        \
    (VTYPE)                                                                                    \
    {                                                                                          \
        .selector = typehash(T), .value.N00B_VARIANT_FIELD(T) = (val),                         \
    }

#define _n00b_variant_is_set_ptr(vptr) ((vptr)->selector != 0)

#define n00b_variant_is_set(v)                                                                 \
    ({                                                                                         \
        auto _bt = &(v);                                                                       \
        _n00b_variant_is_set_ptr(_bt);                                                         \
    })

#define _n00b_variant_is_type_ptr(vptr, T) ((vptr)->selector == typehash(T))

#define n00b_variant_is_type(v, T)                                                             \
    ({                                                                                         \
        auto _bt = &(v);                                                                       \
        _n00b_variant_is_type_ptr(_bt, T);                                                     \
    })

#define _n00b_variant_get_ptr(vptr, T)                                                         \
    ({                                                                                         \
        assert(_n00b_variant_is_type_ptr(vptr, T));                                            \
        (vptr)->value.N00B_VARIANT_FIELD(T);                                                   \
    })

#define n00b_variant_get(v, T)                                                                 \
    ({                                                                                         \
        auto _bt = &(v);                                                                       \
        _n00b_variant_get_ptr(_bt, T);                                                         \
    })

#define _n00b_variant_get_or_else_ptr(vptr, T, alt)                                            \
    ({                                                                                         \
        T _bt = (alt);                                                                         \
        if (_n00b_variant_is_type_ptr(vptr, T)) {                                              \
            _bt = (vptr)->value.N00B_VARIANT_FIELD(T);                                         \
        }                                                                                      \
        _bt;                                                                                   \
    })

#define n00b_variant_get_or_else(v, T, alt)                                                    \
    ({                                                                                         \
        auto _vp = &(v);                                                                       \
        _n00b_variant_get_or_else_ptr(_vp, T, alt);                                            \
    })
