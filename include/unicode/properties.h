#pragma once
/** @file properties.h
 *  @brief Per-codepoint Unicode property lookups and string-level display width.
 *
 *  Provides access to General_Category, Canonical_Combining_Class, Script,
 *  Block, Bidi_Class, East_Asian_Width, Joining_Type, binary properties,
 *  numeric type/value, and string display width measurement.
 */

#include "unicode/types_ext.h"

// ===========================================================================
// Per-codepoint property lookups
// ===========================================================================

/** @brief Return the General_Category of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The General_Category value.
 */
n00b_unicode_gc_t n00b_unicode_general_category(n00b_codepoint_t cp);

/** @brief Return the Canonical_Combining_Class of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The combining class (0..254).
 */
uint8_t n00b_unicode_combining_class(n00b_codepoint_t cp);

/** @brief Return the Script property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return A script index (use n00b_unicode_script_name() for the name).
 */
n00b_unicode_script_t n00b_unicode_script(n00b_codepoint_t cp);

/** @brief Return the human-readable name of a script value.
 *  @param s  A script index returned by n00b_unicode_script().
 *  @return A static string such as "Latin", "Han", etc.
 */
const char *n00b_unicode_script_name(n00b_unicode_script_t s);

/** @brief Return the Block property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return A block index (use n00b_unicode_block_name() for the name).
 */
n00b_unicode_block_t n00b_unicode_block(n00b_codepoint_t cp);

/** @brief Return the human-readable name of a block value.
 *  @param b  A block index returned by n00b_unicode_block().
 *  @return A static string such as "Basic Latin", etc.
 */
const char *n00b_unicode_block_name(n00b_unicode_block_t b);

/** @brief Return the Bidi_Class of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The bidirectional class value.
 */
n00b_unicode_bidi_class_t n00b_unicode_bidi_class(n00b_codepoint_t cp);

/** @brief Return the East_Asian_Width property of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The East Asian width category.
 */
n00b_unicode_eaw_t n00b_unicode_east_asian_width(n00b_codepoint_t cp);

/** @brief Return the display column width of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return 0, 1, or 2 columns.
 */
int n00b_unicode_char_width(n00b_codepoint_t cp);

/** @brief Return the Joining_Type of a codepoint (for Arabic shaping).
 *  @param cp  The codepoint to query.
 *  @return The joining type value.
 */
n00b_unicode_jt_t n00b_unicode_joining_type(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has a given binary property.
 *  @param cp    The codepoint to query.
 *  @param prop  The binary property to test.
 *  @return true if the codepoint has the property.
 */
bool n00b_unicode_has_property(n00b_codepoint_t cp,
                               n00b_unicode_property_t prop);

/** @brief Retrieve the Script_Extensions for a codepoint (UTS #24).
 *  @param cp           The codepoint to query.
 *  @param scripts      Output array for script values.
 *  @param max_scripts  Maximum number of scripts to write.
 *  @return Number of scripts written, or total count if > max_scripts.
 */
int n00b_unicode_script_extensions(n00b_codepoint_t cp,
                                   n00b_unicode_script_t *scripts,
                                   int max_scripts);

// ===========================================================================
// Numeric properties
// ===========================================================================

/** @brief Return the Numeric_Type of a codepoint.
 *  @param cp  The codepoint to query.
 *  @return The numeric type (NONE, DECIMAL, DIGIT, or NUMERIC).
 */
n00b_unicode_numeric_type_t n00b_unicode_numeric_type(n00b_codepoint_t cp);

/** @brief Return the full Numeric_Value of a codepoint as a rational.
 *  @param cp  The codepoint to query.
 *  @return A numeric value struct with type, numerator, and denominator.
 */
n00b_unicode_numeric_value_t n00b_unicode_numeric_value(n00b_codepoint_t cp);

/** @brief Return the decimal digit value (0..9) of a codepoint, if any.
 *  @param cp  The codepoint to query.
 *  @return An option containing the digit value, or none.
 */
n00b_option_t(int32_t) n00b_unicode_digit_value(n00b_codepoint_t cp);

// ===========================================================================
// String-level display width
// ===========================================================================

/** @brief Compute the total display width of a UTF-8 string in columns.
 *  @param s  The string to measure.
 *  @return The display width (sum of character widths).
 */
int32_t n00b_unicode_display_width(n00b_string_t s);
