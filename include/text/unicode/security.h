#pragma once
/** @file security.h
 *  @brief Unicode security mechanisms: confusable detection, skeleton mapping,
 *         and script restriction (UTS #39).
 *
 *  Two strings are confusable if and only if their skeletons are identical.
 *  Script restriction analysis classifies strings by the diversity of scripts
 *  they contain.
 */

#include "text/unicode/types_ext.h"

/** @brief Compute the skeleton of a string for confusable detection (UTS #39).
 *
 *  Two strings are confusable if and only if their skeletons are identical.
 *
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new skeleton string.
 */
n00b_string_t *n00b_unicode_skeleton(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Test whether two strings are visually confusable (UTS #39).
 *  @param a  First string.
 *  @param b  Second string.
 *  @return true if the strings have identical skeletons.
 */
bool n00b_unicode_is_confusable(n00b_string_t *a, n00b_string_t *b);

/** @brief Determine the script restriction level of a string (UTS #39).
 *  @param s  The string to analyze.
 *  @return The restriction level (from ASCII_ONLY to UNRESTRICTED).
 */
n00b_unicode_restriction_level_t n00b_unicode_script_restriction(
    n00b_string_t *s);

/** @brief Test whether a string contains mixed scripts (UTS #39).
 *  @param s  The string to analyze.
 *  @return true if the string mixes scripts beyond Common/Inherited.
 */
bool n00b_unicode_has_mixed_scripts(n00b_string_t *s);
