/**
 * @file variant.h
 * @brief Type-safe tagged union — pure macros, no IMPL required.
 *
 * @c base_variant_t(...) represents a value that can be one of several types.
 * Uses BASE_MAP for variadic iteration, eliminating hardcoded arity limits.
 *
 * Usage:
 * @code
 *     base_variant_t(int, char *, double) v =
 *         base_variant_ctor(0, int, 42, int, char *, double);
 *     if (base_variant_is(v, 0)) {
 *         int x = base_variant_get(v, int);
 *     }
 * @endcode
 */
#pragma once

#include "generic.h"
#include "macros.h"
#include "option.h"
#include <stdint.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Struct name generation
// ============================================================================

/**
 * @brief Generate a unique struct tag for a variant with the given types.
 * @param ...  Alternative types.
 */
#define BASE_VARIANT_NAME(...) typeid("base_variant", __VA_ARGS__)

// ============================================================================
// Internal helpers — variadic iteration via BASE_MAP
// ============================================================================

/** @internal Emit sizeof(T), for constexpr_max argument list. */
#define _VARIANT_SIZEOF(T) sizeof(T),

/** @internal Emit typestr(T), for type string array initializer. */
#define _VARIANT_TYPESTR(T) typestr(T),

// ============================================================================
// Type definition
// ============================================================================

/**
 * @brief Declare + define a variant type. Use in variable declarations.
 *
 * No arity limit — works with any number of alternative types.
 *
 * @param ...  Alternative types.
 */
#define base_variant_t(...)                                                    \
    struct BASE_VARIANT_NAME(__VA_ARGS__) {                                    \
        uint8_t     _tag;                                                      \
        const char *_types[BASE_VA_COUNT(__VA_ARGS__)];                        \
        _Alignas(max_align_t) char _data[                                      \
            constexpr_max(BASE_MAP(_VARIANT_SIZEOF, __VA_ARGS__) 0)];          \
    }

/**
 * @brief Tag-only reference (no body) — use after the type has already been
 *        defined by @c base_variant_t in the same scope.
 */
#define base_variant_tag_ref(...) struct BASE_VARIANT_NAME(__VA_ARGS__)

// ============================================================================
// Construction
// ============================================================================

/**
 * @brief Construct a variant holding @p val at tag index @p n.
 *
 * @param n    Tag index for the active alternative.
 * @param T    Type of the value.
 * @param val  Value to store.
 * @param ...  The variant's alternative types.
 * @return A variant value.
 */
#define base_variant_ctor(n, T, val, ...)                                      \
    ({                                                                         \
        base_variant_tag_ref(__VA_ARGS__) _v = {                               \
            ._tag   = (n),                                                     \
            ._types = { BASE_MAP(_VARIANT_TYPESTR, __VA_ARGS__) },             \
        };                                                                     \
        *(T *)_v._data = (val);                                                \
        _v;                                                                    \
    })

// ============================================================================
// Queries
// ============================================================================

/**
 * @brief Get the current type tag of the variant.
 * @param v  Variant value.
 * @return Zero-based index of the active alternative.
 */
#define base_variant_tag(v) ((v)._tag)

/**
 * @brief Check if the variant currently holds the alternative at index @p n.
 * @param v  Variant value.
 * @param n  Zero-based alternative index.
 * @return @c true if the variant's tag equals @p n.
 */
#define base_variant_is(v, n) ((v)._tag == (n))

/**
 * @brief Check if the active alternative's type matches @p T.
 *
 * Compares the runtime type string against @c typestr(T).
 *
 * @param v  Variant value.
 * @param T  Expected type to check against.
 * @return @c true if the type matches.
 */
#define base_variant_type_matches(v, T)                                        \
    (strcmp((v)._types[(v)._tag], typestr(T)) == 0)

// ============================================================================
// Extraction
// ============================================================================

/**
 * @brief Extract the value as type @p T (asserts correct type at runtime).
 *
 * Triggers an assertion failure if the variant does not hold type @p T.
 *
 * @param v  Variant value.
 * @param T  Expected type.
 * @return The value cast to type @p T.
 */
#define base_variant_get(v, T)                                                 \
    (assert(base_variant_type_matches(v, T) && "variant type mismatch"),       \
     *(T *)(v)._data)

/**
 * @brief Extract the value as type @p T without checking.
 *
 * @warning Undefined behavior if the variant does not hold type @p T.
 *
 * @param v  Variant value.
 * @param T  Expected type.
 * @return The value cast to type @p T.
 */
#define base_variant_get_unchecked(v, T) (*(T *)(v)._data)

/**
 * @brief Extract the value or return a default if the tag/type doesn't match.
 *
 * @param v    Variant value.
 * @param n    Expected tag index.
 * @param T    Expected type.
 * @param def  Default value to return if the variant doesn't match.
 * @return The extracted value or @p def.
 */
#define base_variant_get_safe(v, n, T, def)                                    \
    ((base_variant_is(v, n) && base_variant_type_matches(v, T))                \
         ? base_variant_get_unchecked(v, T)                                    \
         : (def))

/**
 * @brief Try to extract the value as an @c base_option_t(T).
 *
 * Returns Some if the variant holds alternative @p n of type @p T,
 * otherwise returns None.
 *
 * @param v  Variant value.
 * @param n  Expected tag index.
 * @param T  Expected type.
 * @return @c option_t(T) — Some(value) or None.
 */
#define base_variant_as(v, n, T)                                               \
    ({                                                                         \
        struct BASE_OPTION_NAME(T) _opt;                                       \
        if (base_variant_is(v, n) && base_variant_type_matches(v, T)) {        \
            _opt = (struct BASE_OPTION_NAME(T)){                               \
                .has_value = true,                                             \
                .value     = base_variant_get_unchecked(v, T)};                \
        } else {                                                               \
            _opt = (struct BASE_OPTION_NAME(T)){.has_value = false};           \
        }                                                                      \
        _opt;                                                                  \
    })

// ============================================================================
// Mutation
// ============================================================================

/**
 * @brief Set the variant to hold a value of type @p T at tag index @p n.
 *
 * @param v    Pointer to the variant.
 * @param n    Tag index for the alternative.
 * @param T    Type of the value.
 * @param val  Value to store.
 * @param ...  The variant's alternative types (must match the definition).
 */
#define base_variant_set(v, n, T, val, ...)                                    \
    do {                                                                       \
        (v)->_tag = (n);                                                       \
        const char *_types[] = { BASE_MAP(_VARIANT_TYPESTR, __VA_ARGS__) };    \
        memcpy((v)->_types, _types, sizeof((v)->_types));                       \
        *(T *)(v)->_data = (val);                                              \
    } while (0)
