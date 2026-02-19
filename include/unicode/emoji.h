#pragma once
/** @file emoji.h
 *  @brief Emoji detection and emoji sequence scanning.
 *
 *  Per-codepoint emoji property tests and a sequence scanner that identifies
 *  the type and byte length of emoji sequences (basic, presentation,
 *  keycap, modifier, flag, tag, ZWJ).
 */

#include "unicode/types_ext.h"

/** @brief Test whether a codepoint has the Emoji property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint is an emoji.
 */
bool n00b_unicode_is_emoji(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the Emoji_Presentation property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint has default emoji presentation.
 */
bool n00b_unicode_is_emoji_presentation(n00b_codepoint_t cp);

/** @brief Scan for an emoji sequence starting at a byte position in a string.
 *  @param s         The string to scan.
 *  @param byte_pos  Byte offset at which to begin scanning.
 *  @return A result containing the emoji type and number of bytes consumed.
 */
n00b_unicode_emoji_scan_result_t n00b_unicode_emoji_scan(n00b_string_t s,
                                                         uint32_t byte_pos);
