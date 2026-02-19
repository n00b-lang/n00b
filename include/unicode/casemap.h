#pragma once
/** @file casemap.h
 *  @brief Unicode case mapping and case-insensitive comparison.
 *
 *  Per-codepoint simple case mappings (upper, lower, title, casefold) and
 *  full string-level locale-aware conversions.  String-level functions
 *  produce new strings; per-codepoint functions return a single codepoint.
 */

#include "unicode/types_ext.h"

// ===========================================================================
// Per-codepoint simple case mappings
// ===========================================================================

/** @brief Return the simple uppercase mapping of a codepoint.
 *  @param cp  The codepoint to map.
 *  @return The uppercase codepoint (or @p cp itself if no mapping exists).
 */
n00b_codepoint_t n00b_unicode_toupper_cp(n00b_codepoint_t cp);

/** @brief Return the simple lowercase mapping of a codepoint.
 *  @param cp  The codepoint to map.
 *  @return The lowercase codepoint (or @p cp itself if no mapping exists).
 */
n00b_codepoint_t n00b_unicode_tolower_cp(n00b_codepoint_t cp);

/** @brief Return the simple titlecase mapping of a codepoint.
 *  @param cp  The codepoint to map.
 *  @return The titlecase codepoint (or @p cp itself if no mapping exists).
 */
n00b_codepoint_t n00b_unicode_totitle_cp(n00b_codepoint_t cp);

/** @brief Return the simple case fold of a codepoint.
 *  @param cp  The codepoint to fold.
 *  @return The case-folded codepoint (or @p cp itself if no mapping exists).
 */
n00b_codepoint_t n00b_unicode_casefold_cp(n00b_codepoint_t cp);

// ===========================================================================
// String-level case conversions (locale-aware)
// ===========================================================================

/** @brief Full string-level uppercase conversion (locale-aware).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw locale     Locale string for context-sensitive mappings (e.g. "tr"),
 *                 or NULL for default behavior.
 *  @return A new uppercased string.
 */
n00b_string_t n00b_unicode_toupper(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr;
             char *locale = nullptr; };

/** @brief Full string-level lowercase conversion (locale-aware).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw locale     Locale string for context-sensitive mappings (e.g. "tr"),
 *                 or NULL for default behavior.
 *  @return A new lowercased string.
 */
n00b_string_t n00b_unicode_tolower(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr;
             char *locale = nullptr; };

/** @brief Full string-level titlecase conversion (locale-aware).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @kw locale     Locale string for context-sensitive mappings (e.g. "tr"),
 *                 or NULL for default behavior.
 *  @return A new titlecased string.
 */
n00b_string_t n00b_unicode_totitle(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr;
             char *locale = nullptr; };

/** @brief Full string-level case fold (locale-independent).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new case-folded string.
 */
n00b_string_t n00b_unicode_casefold(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Case-insensitive string comparison using Unicode case folding.
 *  @param a  First string.
 *  @param b  Second string.
 *  @return Negative, zero, or positive (like strcmp).
 */
int n00b_unicode_casecmp(n00b_string_t a, n00b_string_t b);
