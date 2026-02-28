#pragma once
/** @file style_ops.h
 *  @brief Constructor, merge, comparison, and copy for `n00b_text_style_t`.
 *
 *  All functions that allocate accept an optional `.allocator` keyword
 *  argument.
 *
 *  ### Related modules
 *
 *  - `strings/text_style.h` -- type definitions
 *  - `strings/string_style.h` -- attach styles to strings
 */

#include "text/strings/text_style.h"
#include "core/alloc.h"

// ===================================================================
// Construction
// ===================================================================

/** @brief Allocate a new style with all fields unspecified / default.
 *
 *  Tristates are `N00B_TRI_UNSPECIFIED`, palette indices are -1,
 *  direct colors are 0 (invalid), enums are 0 (NONE/DEFAULT).
 *
 *  @kw allocator Optional allocator.
 *  @return Heap-allocated style.
 *  @post Caller must free with `n00b_free()`.
 */
n00b_text_style_t *n00b_str_style_new()
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Merge / overlay
// ===================================================================

/** @brief Merge two styles, producing a new one.
 *
 *  For each field, the overlay value is used unless it is
 *  unspecified / sentinel, in which case the base value is kept.
 *
 *  @param base     Base (parent) style.
 *  @param overlay  Style whose set fields override @p base.
 *  @kw allocator   Optional allocator.
 *  @return A newly allocated merged style.
 *  @pre @p base and @p overlay are non-nullptr.
 *  @post Returned style is independent of both inputs.
 */
n00b_text_style_t *n00b_str_style_merge(const n00b_text_style_t *base,
                                         const n00b_text_style_t *overlay)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Comparison
// ===================================================================

/** @brief Test whether two styles are identical field-by-field.
 *  @return true if every field matches (including unspecified-ness).
 */
bool n00b_str_style_eq(const n00b_text_style_t *a,
                        const n00b_text_style_t *b);

// ===================================================================
// Copy
// ===================================================================

/** @brief Deep-copy a style.
 *  @kw allocator Optional allocator.
 *  @return A new style with the same values as @p src.
 *  @pre @p src is non-nullptr.
 *  @post Returned style is independent of @p src.
 */
n00b_text_style_t *n00b_str_style_copy(const n00b_text_style_t *src)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Query
// ===================================================================

/** @brief Test whether a style has all fields at their default/unspecified
 *         values.
 */
bool n00b_str_style_is_empty(const n00b_text_style_t *s);
