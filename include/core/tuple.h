/**
 * @file tuple.h
 * @brief Type-safe tuples using ncc's typeid() and constexpr_paste.
 *
 * @c base_tuple_t(...) declares a struct with one typed field per element.
 * Fields are named item_0, item_1, ... via constexpr_paste.
 *
 * Usage:
 * @code
 *     base_tuple_t(int, char *, double) t = base_tuple(42, "hello", 3.14);
 *     int a    = base_tuple_get(t, 0);
 *     char *b  = base_tuple_get(t, 1);
 *     double c = base_tuple_get(t, 2);
 * @endcode
 */
#pragma once

#include "macros.h"

// ============================================================================
// Internal helpers
// ============================================================================

/** @internal Designator for field item_<i>. */
#define base_tuple_item(i)         .constexpr_paste("item_", i)

/** @internal Struct tag via typeid. */
#define BASE_TUPLE(...)            struct typeid("base_tuple", __VA_ARGS__)

/** @internal Emit one struct field: typeof(X) item_<count>; */
#define BASE_TUPLE_ITEM(X, count)  typeof(X) constexpr_paste("item_", count);

/** @internal Emit one designated initializer: .item_<count> = val, */
#define BASE_TUPLE_SET(val, count) base_tuple_item(constexpr_eval(count)) = val,

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Declare a tuple type with the given element types.
 *
 * Expands to a struct definition with one field per type.
 * @param ...  Element types.
 */
#define base_tuple_t(...)                                                      \
    BASE_TUPLE(__VA_ARGS__)                                                    \
    {                                                                          \
        BASE_MAP_COUNT(BASE_TUPLE_ITEM, 0, __VA_ARGS__)                        \
    }

/**
 * @brief Brace-enclosed initializer for a tuple.
 *
 * Use in declaration: @c base_tuple_t(int, char *) t = base_tuple(1, "hi");
 * @param ...  Values matching the element types (in order).
 */
#define base_tuple(...)                                                        \
    { BASE_MAP_COUNT(BASE_TUPLE_SET, 0, __VA_ARGS__) }

/**
 * @brief Access tuple element by index.
 *
 * @param tup  Tuple value.
 * @param i    Zero-based element index (must be a compile-time constant).
 * @return The element value.
 */
#define base_tuple_get(tup, i) ((tup)base_tuple_item(constexpr_eval(i)))
