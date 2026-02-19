#pragma once
/** @file format_spec.h
 *  @brief Parse and apply printf-like format specifiers for substitutions.
 *
 *  A format spec string (the part after `:` in `[|#N:spec|]`) is parsed
 *  into an `n00b_format_spec_t` that describes width, precision, alignment,
 *  padding, and type conversion.
 *
 *  ### Spec syntax
 *
 *  ```
 *  [flags][width][.precision]type
 *  ```
 *
 *  **Flags:** `-` (left-align), `0` (zero-pad), `+` (force sign),
 *  ` ` (space for positive), `,` (thousands separator).
 *
 *  **Types:** `d`/`i` (signed decimal), `u` (unsigned decimal),
 *  `x`/`X` (hex), `o` (octal), `f` (fixed float), `e`/`E` (scientific),
 *  `g`/`G` (shortest), `s` (string), `c` (character), `b`/`B` (bool word),
 *  `t`/`T` (bool letter), `y`/`Y` (yes/no), `q`/`Q` (yes/no word),
 *  `p`/`P` (pointer).
 *
 *  ### Related modules
 *
 *  - `strings/fmt_numbers.h` -- base formatting functions
 *  - `strings/rich_desc.h` -- substitution parsing
 */

#include "n00b.h"
#include "core/string.h"

// ===================================================================
// Parsed format spec
// ===================================================================

/** @brief Parsed format specifier from a substitution tag. */
typedef struct {
    char    type;       /**< Conversion type character (d/x/f/s/...) or 0 */
    int16_t width;      /**< Minimum field width (-1 = unset) */
    int16_t precision;  /**< Decimal places or string max (-1 = unset) */
    bool    left_align; /**< `-` flag: left-align within width */
    bool    zero_pad;   /**< `0` flag: pad with zeros */
    bool    sign_plus;  /**< `+` flag: show + for positive numbers */
    bool    sign_space; /**< ` ` flag: space for positive numbers */
    bool    commas;     /**< `,` flag: thousands separator */
    bool    upper;      /**< Uppercase variant (X, E, G, B, T, Y, Q, P) */
    bool    word;       /**< Word form for bool types */
    bool    yn;         /**< yes/no form for bool types */
} n00b_format_spec_t;

// ===================================================================
// Parsing
// ===================================================================

/** @brief Parse a format spec string.
 *  @param spec      Spec string (e.g., `",d"`, `".2f"`, `"-20s"`).
 *  @param spec_len  Length in bytes.
 *  @return Parsed spec struct (stack-allocated, no heap).
 */
n00b_format_spec_t n00b_format_spec_parse(const char *spec, int spec_len);

// ===================================================================
// Extended formatting via spec
// ===================================================================

/** @brief Format a signed integer according to a spec.
 *  @param value  The integer to format.
 *  @param spec   Parsed format specifier (width, padding, sign, base, etc.).
 *  @kw allocator Optional allocator.
 *  @pre @p spec is non-nullptr.
 */
n00b_string_t n00b_str_fmt_int_ex(int64_t value,
                                   const n00b_format_spec_t *spec)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Format a double according to a spec (fixed-point).
 *  @param value  The double to format.
 *  @param spec   Parsed format specifier (precision, width, padding, etc.).
 *  @kw allocator Optional allocator.
 *  @pre @p spec is non-nullptr.
 */
n00b_string_t n00b_str_fmt_float_ex(double value,
                                     const n00b_format_spec_t *spec)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Format a string according to a spec (width/align/truncate).
 *  @param value  The string to format.
 *  @param spec   Parsed format specifier (width, alignment, truncation).
 *  @kw allocator Optional allocator.
 *  @pre @p spec is non-nullptr.
 */
n00b_string_t n00b_str_fmt_string_ex(n00b_string_t value,
                                      const n00b_format_spec_t *spec)
    _kargs { n00b_allocator_t *allocator = nullptr; };
