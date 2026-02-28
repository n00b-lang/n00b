/**
 * @file tuple.h
 * @brief Type-safe tuples using ncc's typeid() and constexpr_paste.
 *
 * @c n00b_tuple_decl(...) declares a struct with one typed field per element.
 * Fields are named item_0, item_1, ... via constexpr_paste.
 *
 * Usage:
 * @code
 *     n00b_tuple_decl(int, char *, double) t = n00b_tuple(42, "hello", 3.14);
 *     int a    = n00b_tuple_get(t, 0);
 *     char *b  = n00b_tuple_get(t, 1);
 *     double c = n00b_tuple_get(t, 2);
 * @endcode
 */
#pragma once

#include "core/macros.h"

// ============================================================================
// Internal helpers
// ============================================================================

/** @internal Designator for field item_<i>. */
#define n00b_tuple_item(i) .constexpr_paste("item_", i)

/** @internal Struct tag via typeid. */
#define n00b_tuple_t(...) struct typeid("n00b_tuple", __VA_ARGS__)

/** @internal Emit one struct field: typeof(X) item_<count>; */
#define N00B_TUPLE_ITEM(X, count) typeof(X) constexpr_paste("item_", count);

/** @internal Emit one designated initializer: .item_<count> = val, */
#define N00B_TUPLE_SET(val, count) n00b_tuple_item(constexpr_eval(count)) = val,

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Declare a tuple type with the given element types.
 *
 * Expands to a struct definition with one field per type.
 * @param ...  Element types.
 */
#define n00b_tuple_decl(...)                                                                   \
    n00b_tuple_t(__VA_ARGS__)                                                                  \
    {                                                                                          \
        N00B_MAP_COUNT(N00B_TUPLE_ITEM, 0, __VA_ARGS__)                                        \
    }

/**
 * @brief Brace-enclosed initializer for a tuple.
 *
 * Use in declaration: @c n00b_tuple_decl(int, char *) t = n00b_tuple(1, "hi");
 * @param ...  Values matching the element types (in order).
 */
#define n00b_tuple(...) {N00B_MAP_COUNT(N00B_TUPLE_SET, 0, __VA_ARGS__)}

/**
 * @brief Access tuple element by index.
 *
 * @param tup  Tuple value.
 * @param i    Zero-based element index (must be a compile-time constant).
 * @return The element value.
 */
#define n00b_tuple_get(tup, i) ((tup)n00b_tuple_item(constexpr_eval(i)))
