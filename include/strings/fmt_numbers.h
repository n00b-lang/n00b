#pragma once
/** @file fmt_numbers.h
 *  @brief Numeric formatting: hex, int, float, bool, codepoint, pointer.
 *
 *  All functions return `n00b_string_t` **by value** and accept an optional
 *  `.allocator` keyword argument.  Integer formatting supports optional
 *  comma-separated thousands grouping; float formatting uses Grisu2 via
 *  `n00b_fptostr()` from `strings/fptostr.h`.
 *
 *  ### Related modules
 *
 *  - `strings/fptostr.h` -- Grisu2 double-to-string backend
 *  - `strings/string_convert.h` -- lower-level string ↔ integer/hex
 *  - `unicode/properties.h` -- `n00b_unicode_general_category()` for
 *    codepoint classification in `n00b_fmt_codepoint()`
 */

#include "unicode/types_ext.h"

// ===================================================================
// Hex formatting
// ===================================================================

/** @brief Format a 64-bit unsigned integer as hexadecimal (no prefix).
 *  @param value  The integer to format.
 *  @kw caps       Use uppercase hex digits (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A hex string (e.g. `"dead"` or `"DEAD"`), or `"0"` for zero.
 */
n00b_string_t n00b_fmt_hex(uint64_t value)
    _kargs {
        bool              caps      = false;
        n00b_allocator_t *allocator = nullptr;
    };

// ===================================================================
// Integer formatting
// ===================================================================

/** @brief Format a signed 64-bit integer as decimal.
 *  @param value   The integer to format.
 *  @kw commas     Insert comma thousands separators (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A decimal string (e.g. `"-1,234"` or `"42"`).
 */
n00b_string_t n00b_fmt_int(int64_t value)
    _kargs {
        bool              commas    = false;
        n00b_allocator_t *allocator = nullptr;
    };

/** @brief Format an unsigned 64-bit integer as decimal.
 *  @param value   The integer to format.
 *  @kw commas     Insert comma thousands separators (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A decimal string (e.g. `"18,446,744,073,709,551,615"`).
 */
n00b_string_t n00b_fmt_uint(uint64_t value)
    _kargs {
        bool              commas    = false;
        n00b_allocator_t *allocator = nullptr;
    };

// ===================================================================
// Float formatting
// ===================================================================

/** @brief Format a double using Grisu2, with optional width/zero-pad.
 *  @param value  The double to format.
 *  @kw width      Minimum output width (default: 0, no padding).
 *  @kw fill       If true and width > result length, zero-pad the left
 *                 (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A decimal float string (e.g. `"3.14"`, `"nan"`, `"-inf"`).
 */
n00b_string_t n00b_fmt_float(double value)
    _kargs {
        int               width     = 0;
        bool              fill      = false;
        n00b_allocator_t *allocator = nullptr;
    };

// ===================================================================
// Boolean formatting
// ===================================================================

/** @brief Format a boolean with configurable output style.
 *  @param value  The boolean value.
 *  @kw upper      Capitalize first letter (default: false).
 *  @kw word       Use full word ("true"/"false") vs single letter ("t"/"f")
 *                 (default: false).
 *  @kw yn         Use "yes"/"no" / "y"/"n" instead of "true"/"false" / "t"/"f"
 *                 (default: false).
 *  @kw allocator  Optional allocator.
 *  @return One of 16 possible strings (see source for table).
 */
n00b_string_t n00b_fmt_bool(bool value)
    _kargs {
        bool              upper     = false;
        bool              word      = false;
        bool              yn        = false;
        n00b_allocator_t *allocator = nullptr;
    };

// ===================================================================
// Codepoint formatting
// ===================================================================

/** @brief Format a Unicode codepoint as its character or `U+XXXX` escape.
 *
 *  Printable characters are returned as-is; control characters and
 *  non-characters are formatted as `U+XXXX` (or `U+1XXXX` for supplementary).
 *  Invalid codepoints (> U+10FFFF) are replaced with U+FFFD.
 *
 *  @param cp  The codepoint to format.
 *  @kw allocator  Optional allocator.
 *  @return A string containing the character or its `U+` representation.
 */
n00b_string_t n00b_fmt_codepoint(n00b_codepoint_t cp)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Pointer formatting
// ===================================================================

/** @brief Format a pointer as `@` followed by 16 hex digits.
 *  @param ptr   The pointer to format.
 *  @kw caps       Use uppercase hex digits (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A 17-character string (e.g. `"@00007fff5fbffa00"`).
 */
n00b_string_t n00b_fmt_pointer(void *ptr)
    _kargs {
        bool              caps      = false;
        n00b_allocator_t *allocator = nullptr;
    };
