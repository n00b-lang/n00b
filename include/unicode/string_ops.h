#pragma once
/** @file string_ops.h
 *  @brief Unicode-aware string operations: concatenation, slicing, search,
 *         replace, split, trim, pad, and more.
 *
 *  All grapheme-aware operations use grapheme cluster indices (not byte or
 *  codepoint indices) unless explicitly noted.  Functions that produce new
 *  strings accept an optional allocator keyword argument.
 */

#include "unicode/types_ext.h"

// ===================================================================
// Concatenation
// ===================================================================

/** @brief Concatenate two strings.
 *  @param a  First string.
 *  @param b  Second string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing @p a followed by @p b.
 */
n00b_string_t n00b_unicode_str_cat(n00b_string_t a, n00b_string_t b)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Concatenate an array of strings.
 *  @param parts  Array of strings.
 *  @param count  Number of elements in @p parts.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing all parts concatenated in order.
 */
n00b_string_t n00b_unicode_str_cat_many(n00b_string_t *parts, uint32_t count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Join an array of strings with a separator.
 *  @param sep    The separator string inserted between parts.
 *  @param parts  Array of strings.
 *  @param count  Number of elements in @p parts.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string with all parts joined by @p sep.
 */
n00b_string_t n00b_unicode_str_join(n00b_string_t sep, n00b_string_t *parts,
                                    uint32_t count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Slicing (grapheme-aware: indices are grapheme cluster positions)
// ===================================================================

/** @brief Extract a substring by grapheme cluster indices.
 *  @param s      The source string.
 *  @param start  Start grapheme index (negative counts from end).
 *  @param end    End grapheme index, exclusive (negative counts from end).
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing the selected grapheme clusters.
 */
n00b_string_t n00b_unicode_str_slice(n00b_string_t s, int32_t start,
                                     int32_t end)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Extract a single grapheme cluster by index.
 *  @param s      The source string.
 *  @param index  Grapheme index (negative counts from end).
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing the grapheme cluster at @p index.
 */
n00b_string_t n00b_unicode_str_grapheme_at(n00b_string_t s, int32_t index)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Byte-level slicing
// ===================================================================

/** @brief Extract a substring by byte offsets.
 *  @param s           The source string.
 *  @param byte_start  Start byte offset (inclusive).
 *  @param byte_end    End byte offset (exclusive).
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing bytes [byte_start, byte_end).
 */
n00b_string_t n00b_unicode_str_slice_bytes(n00b_string_t s,
                                           uint32_t byte_start,
                                           uint32_t byte_end)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Search
// ===================================================================

/** @brief Find the first occurrence of @p needle in @p haystack.
 *  @param haystack  The string to search in.
 *  @param needle    The string to search for.
 *  @return An option containing the byte offset, or none if not found.
 */
n00b_unicode_opt_i32_t n00b_unicode_str_find(n00b_string_t haystack,
                                             n00b_string_t needle);

/** @brief Find the last occurrence of @p needle in @p haystack.
 *  @param haystack  The string to search in.
 *  @param needle    The string to search for.
 *  @return An option containing the byte offset, or none if not found.
 */
n00b_unicode_opt_i32_t n00b_unicode_str_rfind(n00b_string_t haystack,
                                              n00b_string_t needle);

/** @brief Test whether @p haystack contains @p needle.
 *  @param haystack  The string to search in.
 *  @param needle    The string to search for.
 *  @return true if @p needle is found in @p haystack.
 */
bool n00b_unicode_str_contains(n00b_string_t haystack, n00b_string_t needle);

/** @brief Test whether a string starts with a given prefix.
 *  @param s       The string to test.
 *  @param prefix  The prefix to check.
 *  @return true if @p s starts with @p prefix.
 */
bool n00b_unicode_str_starts_with(n00b_string_t s, n00b_string_t prefix);

/** @brief Test whether a string ends with a given suffix.
 *  @param s       The string to test.
 *  @param suffix  The suffix to check.
 *  @return true if @p s ends with @p suffix.
 */
bool n00b_unicode_str_ends_with(n00b_string_t s, n00b_string_t suffix);

// ===================================================================
// Replace
// ===================================================================

/** @brief Replace the first occurrence of @p old_s with @p new_s.
 *  @param s      The source string.
 *  @param old_s  The substring to find.
 *  @param new_s  The replacement string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string with the first occurrence replaced.
 */
n00b_string_t n00b_unicode_str_replace(n00b_string_t s, n00b_string_t old_s,
                                       n00b_string_t new_s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Replace all occurrences of @p old_s with @p new_s.
 *  @param s      The source string.
 *  @param old_s  The substring to find.
 *  @param new_s  The replacement string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string with all occurrences replaced.
 */
n00b_string_t n00b_unicode_str_replace_all(n00b_string_t s,
                                           n00b_string_t old_s,
                                           n00b_string_t new_s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Split
// ===================================================================

/** @brief Split a string by a separator.
 *  @param s      The string to split.
 *  @param sep    The separator string.
 *  @param count  Out: number of resulting parts.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return An array of strings (by value); caller frees the array.
 */
n00b_string_t *n00b_unicode_str_split(n00b_string_t s, n00b_string_t sep,
                                      uint32_t *count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Split a string at word boundaries (UAX #29).
 *  @param s      The string to split.
 *  @param count  Out: number of resulting words.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return An array of strings (by value); caller frees the array.
 */
n00b_string_t *n00b_unicode_str_split_words(n00b_string_t s, uint32_t *count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Split a string into individual grapheme clusters.
 *  @param s      The string to split.
 *  @param count  Out: number of resulting grapheme clusters.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return An array of strings (by value); caller frees the array.
 */
n00b_string_t *n00b_unicode_str_split_graphemes(n00b_string_t s,
                                                uint32_t *count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Split a string into lines (at CR, LF, or CRLF).
 *  @param s      The string to split.
 *  @param count  Out: number of resulting lines.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return An array of strings (by value, without terminators); caller frees
 *          the array.
 */
n00b_string_t *n00b_unicode_str_split_lines(n00b_string_t s, uint32_t *count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Trim
// ===================================================================

/** @brief Remove leading and trailing Unicode whitespace.
 *  @param s  The string to trim.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new trimmed string.
 */
n00b_string_t n00b_unicode_str_trim(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Remove leading Unicode whitespace.
 *  @param s  The string to trim.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new left-trimmed string.
 */
n00b_string_t n00b_unicode_str_trim_left(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Remove trailing Unicode whitespace.
 *  @param s  The string to trim.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new right-trimmed string.
 */
n00b_string_t n00b_unicode_str_trim_right(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Comparison
// ===================================================================

/** @brief Compare two strings byte-by-byte (like strcmp).
 *  @param a  First string.
 *  @param b  Second string.
 *  @return Negative, zero, or positive.
 */
int n00b_unicode_str_cmp(n00b_string_t a, n00b_string_t b);

/** @brief Test whether two strings are byte-identical.
 *  @param a  First string.
 *  @param b  Second string.
 *  @return true if the strings are equal.
 */
bool n00b_unicode_str_eq(n00b_string_t a, n00b_string_t b);

/** @brief Test whether two strings are equal after NFC normalization.
 *  @param a  First string.
 *  @param b  Second string.
 *  @return true if the strings are canonically equivalent.
 */
bool n00b_unicode_str_eq_nfc(n00b_string_t a, n00b_string_t b);

/** @brief Test whether two strings are equal under case folding.
 *  @param a  First string.
 *  @param b  Second string.
 *  @return true if the strings are case-insensitively equal.
 */
bool n00b_unicode_str_eq_casefold(n00b_string_t a, n00b_string_t b);

// ===================================================================
// Width-aware padding/truncation
// ===================================================================

/** @brief Pad a string on the left to a given display width.
 *  @param s      The string to pad.
 *  @param width  Target display width in columns.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw fill       Fill codepoint (default: space U+0020).
 *  @return A new right-aligned string padded to @p width columns.
 */
n00b_string_t n00b_unicode_str_pad_left(n00b_string_t s, int32_t width)
    _kargs { n00b_allocator_t *allocator = nullptr;
             n00b_codepoint_t fill = ' '; };

/** @brief Pad a string on the right to a given display width.
 *  @param s      The string to pad.
 *  @param width  Target display width in columns.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw fill       Fill codepoint (default: space U+0020).
 *  @return A new left-aligned string padded to @p width columns.
 */
n00b_string_t n00b_unicode_str_pad_right(n00b_string_t s, int32_t width)
    _kargs { n00b_allocator_t *allocator = nullptr;
             n00b_codepoint_t fill = ' '; };

/** @brief Center a string within a given display width.
 *  @param s      The string to center.
 *  @param width  Target display width in columns.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw fill       Fill codepoint (default: space U+0020).
 *  @return A new centered string padded to @p width columns.
 */
n00b_string_t n00b_unicode_str_center(n00b_string_t s, int32_t width)
    _kargs { n00b_allocator_t *allocator = nullptr;
             n00b_codepoint_t fill = ' '; };

/** @brief Truncate a string to fit within a maximum display width.
 *  @param s          The string to truncate.
 *  @param max_width  Maximum display width in columns.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw ellipsis   Ellipsis string appended when truncation occurs
 *                 (default: "...").
 *  @return A new string truncated with an ellipsis if it exceeds
 *          @p max_width.
 */
n00b_string_t n00b_unicode_str_truncate(n00b_string_t s, int32_t max_width)
    _kargs { n00b_allocator_t *allocator = nullptr;
             const char *ellipsis = "..."; };

// ===================================================================
// Repeat
// ===================================================================

/** @brief Repeat a string a given number of times.
 *  @param s      The string to repeat.
 *  @param count  Number of repetitions.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string containing @p s repeated @p count times.
 */
n00b_string_t n00b_unicode_str_repeat(n00b_string_t s, uint32_t count)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===================================================================
// Reverse (grapheme-aware)
// ===================================================================

/** @brief Reverse a string at the grapheme cluster level.
 *  @param s  The string to reverse.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new string with grapheme clusters in reverse order.
 */
n00b_string_t n00b_unicode_str_reverse(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };
