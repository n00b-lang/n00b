#pragma once
/** @file string_style.h
 *  @brief Attach, query, and manipulate styling on `n00b_string_t`.
 *
 *  All functions that produce new strings return `n00b_string_t` by
 *  value.  The original string is never mutated -- a new string with
 *  the same data but updated `styling` pointer is returned.
 *
 *  ### Related modules
 *
 *  - `strings/text_style.h` -- type definitions
 *  - `strings/style_ops.h` -- style merge / compare
 *  - `core/string.h` -- `n00b_string_t` layout
 */

#include "strings/style_ops.h"
#include "core/string.h"

// ===================================================================
// Attach
// ===================================================================

/** @brief Return a copy of @p s with its base style set.
 *
 *  Replaces any existing base style.  Per-range style records are
 *  preserved.
 *
 *  @param s      Source string (not modified).
 *  @param style  Base style (ownership is *not* transferred).
 *  @kw allocator Optional allocator.
 *  @return New string value with base style set.
 */
n00b_string_t n00b_str_set_base_style(n00b_string_t s,
                                       const n00b_text_style_t *style)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Append a ranged style record to @p s.
 *
 *  The range `[start, end)` is in byte offsets.  Pass
 *  `n00b_option_none(size_t)` for @p end_opt to extend to end-of-string.
 *
 *  @param s        Source string.
 *  @param style    Style for the range (copied).
 *  @param start    Start byte offset.
 *  @param end_opt  End byte offset (exclusive), or none for open-ended.
 *  @kw allocator   Optional allocator.
 *  @return New string value with the style record appended.
 */
n00b_string_t n00b_str_add_style(n00b_string_t s,
                                  const n00b_text_style_t *style,
                                  size_t start,
                                  n00b_option_t(size_t) end_opt)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Query
// ===================================================================

/** @brief Get the raw styling metadata from a string.
 *  @return Pointer to the style info, or nullptr if no styling is attached.
 */
n00b_string_style_info_t *n00b_str_get_style_info(n00b_string_t s);

/** @brief Resolve the effective style at a byte position.
 *
 *  Merges the base style with all overlapping range records.
 *
 *  @param s         The styled string.
 *  @param byte_pos  Byte offset to query.
 *  @kw allocator    Optional allocator for the returned style.
 *  @return Merged style, or an empty style if no styling is attached.
 */
n00b_text_style_t *n00b_str_resolve_style_at(n00b_string_t s,
                                              size_t byte_pos)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Strip
// ===================================================================

/** @brief Return a copy of @p s with all styling removed.
 *  @return String with `styling == nullptr`.
 */
n00b_string_t n00b_str_strip_styles(n00b_string_t s);
