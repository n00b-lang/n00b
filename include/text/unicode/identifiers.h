#pragma once

/** @file identifiers.h
 *  @brief Unicode identifier character classification and validation.
 *
 *  Per-codepoint tests for ID_Start, ID_Continue, XID_Start, XID_Continue,
 *  Pattern_Syntax, Pattern_White_Space, and UTS #39 Identifier_Status, plus
 *  whole-string identifier validation.
 */

#include "text/unicode/types_ext.h"

/** @brief Test whether a codepoint has the ID_Start property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint can start an identifier.
 */
bool n00b_unicode_is_id_start(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the ID_Continue property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint can continue an identifier.
 */
bool n00b_unicode_is_id_continue(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the XID_Start property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint can start an identifier (NFKC-stable).
 */
bool n00b_unicode_is_xid_start(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the XID_Continue property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint can continue an identifier (NFKC-stable).
 */
bool n00b_unicode_is_xid_continue(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the Pattern_Syntax property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint is pattern syntax.
 */
bool n00b_unicode_is_pattern_syntax(n00b_codepoint_t cp);

/** @brief Test whether a codepoint has the Pattern_White_Space property.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint is pattern white space.
 */
bool n00b_unicode_is_pattern_white_space(n00b_codepoint_t cp);

/** @brief Test whether a codepoint is allowed in identifiers per UTS #39.
 *  @param cp  The codepoint to test.
 *  @return true if the codepoint has Identifier_Status=Allowed.
 */
bool n00b_unicode_is_identifier_allowed(n00b_codepoint_t cp);

/** @brief Validate a whole string as a Unicode identifier
 *         (XID_Start + XID_Continue*).
 *  @param s  The string to validate.
 *  @return true if the string is a valid Unicode identifier.
 */
bool n00b_unicode_is_valid_identifier(n00b_string_t *s);
