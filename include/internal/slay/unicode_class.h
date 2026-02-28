#pragma once

/**
 * @file unicode_class.h
 * @internal
 * @brief Character class matching for parser engines.
 *
 * Dispatches `n00b_char_class_t` values against n00b's unicode
 * property tables (`unicode/identifiers.h`, `unicode/properties.h`).
 */

#include "slay/types.h"
#include "text/unicode/identifiers.h"
#include "text/unicode/properties.h"

/**
 * @brief Test if a codepoint matches a character class.
 * @param cp  Unicode codepoint.
 * @param cc  Character class to test against.
 * @return True if the codepoint belongs to the class.
 */
bool n00b_codepoint_matches_class(int32_t cp, n00b_char_class_t cc);
