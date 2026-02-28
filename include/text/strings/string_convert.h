#pragma once
/** @file string_convert.h
 *  @brief Conversion between `n00b_string_t` and other representations.
 *
 *  Provides functions for converting strings to/from integers,
 *  hexadecimal, C strings, literals, codepoints, and files.
 *
 *  ### Related modules
 *
 *  - `strings/string_ops.h` -- higher-level string operations
 *  - `strings/fmt_numbers.h` -- numeric formatting (uses these conversions)
 *  - `unicode/encoding.h` -- UTF-8 encoding/decoding
 */

#include "text/unicode/types_ext.h"
#include "adt/array.h"
#include "text/strings/string_ops.h" // for n00b_array_t(n00b_string_t)

typedef char *n00b_cstr_t;

// ===================================================================
// Integer ↔ string
// ===================================================================

/** @brief Convert a signed 64-bit integer to its decimal string representation.
 *  @param n  The integer to convert.
 *  @kw allocator  Optional allocator.
 *  @return A string containing the decimal representation.
 */
n00b_string_t *n00b_unicode_str_from_int(int64_t n)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Hex encoding
// ===================================================================

/** @brief Encode the raw bytes of a string as hexadecimal.
 *  @param s      The string whose bytes to encode.
 *  @kw upper      Use uppercase hex digits (default: false).
 *  @kw allocator  Optional allocator.
 *  @return A string containing two hex digits per input byte.
 */
n00b_string_t *n00b_unicode_str_to_hex(n00b_string_t *s)
    _kargs {
        bool              upper     = false;
        n00b_allocator_t *allocator = nullptr;
    };

// ===================================================================
// C string conversion
// ===================================================================

/** @brief Copy string data to a NUL-terminated C string.
 *  @param s  The string to convert.
 *  @kw allocator  Optional allocator.
 *  @return A newly allocated NUL-terminated `char *`.
 */
char *n00b_unicode_str_to_cstr(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Literal form
// ===================================================================

/** @brief Produce a quoted, escaped string literal (e.g., `"hello\n"`).
 *  @param s  The string to convert.
 *  @kw allocator  Optional allocator.
 *  @return A string wrapped in double quotes with contents escaped.
 */
n00b_string_t *n00b_unicode_str_to_literal(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Codepoint → string
// ===================================================================

/** @brief Create a string from a single codepoint.
 *
 *  Returns an empty string for invalid codepoints (surrogates, > 0x10FFFF).
 *
 *  @param cp  The codepoint.
 *  @kw allocator  Optional allocator.
 *  @return A string containing the UTF-8 encoding of @p cp.
 */
n00b_string_t *n00b_unicode_str_from_codepoint(n00b_codepoint_t cp)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// File I/O
// ===================================================================

/** @brief Read an entire file as a UTF-8 string.
 *
 *  Uses POSIX `open`/`read`/`close`.  Validates UTF-8 after reading.
 *
 *  @param path  NUL-terminated file path.
 *  @kw allocator  Optional allocator.
 *  @return Ok(string) on success, or an error code (`errno`) on failure.
 */
n00b_result_t(n00b_string_t *) n00b_unicode_str_from_file(const char *path)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// C string array
// ===================================================================

/** @brief Convert an array of strings to a `char *` array.
 *  @param parts  Array of strings.
 *  @kw allocator  Optional allocator.
 *  @return An array of NUL-terminated C strings.
 */
n00b_array_t(n00b_cstr_t) n00b_unicode_make_cstr_array(
    n00b_array_t(n00b_string_t *) parts)
    _kargs { n00b_allocator_t *allocator = nullptr; };
