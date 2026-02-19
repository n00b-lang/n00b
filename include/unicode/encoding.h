#pragma once

/** @file encoding.h
 *  @brief UTF-8/16/32 encoding, decoding, validation, and BOM detection.
 *
 *  Low-level UTF-8 decode/encode operations on raw byte buffers, plus
 *  string-level validation, codepoint counting, and transcoding to/from
 *  UTF-16 and UTF-32.
 */

#include "unicode/types_ext.h"
#include "core/array.h"

n00b_array_decl(uint16_t);
n00b_array_decl(n00b_codepoint_t);

// ===========================================================================
// Low-level UTF-8 operations (raw byte buffers)
// ===========================================================================

/** @brief Decode one UTF-8 codepoint from @p src starting at byte *pos.
 *  @param src  Source UTF-8 byte buffer.
 *  @param len  Length of @p src in bytes.
 *  @param pos  In/out byte offset; advanced past the decoded codepoint.
 *  @return The decoded codepoint, or a negative value on error.
 */
int32_t n00b_unicode_utf8_decode(const char *src, uint32_t len, uint32_t *pos);

/** @brief Encode a single codepoint as UTF-8.
 *  @param cp   The codepoint to encode.
 *  @param dst  Destination buffer (must have room for up to 4 bytes).
 *  @return Number of bytes written.
 */
uint32_t n00b_unicode_utf8_encode(n00b_codepoint_t cp, char *dst);

/** @brief Validate that a raw byte buffer is well-formed UTF-8.
 *  @param src  Source byte buffer.
 *  @param len  Length of @p src in bytes.
 *  @return true if the entire buffer is valid UTF-8.
 */
bool n00b_unicode_utf8_validate(const char *src, uint32_t len);

/** @brief Detect a Byte Order Mark at the start of a byte buffer.
 *  @param data     Raw byte data.
 *  @param len      Length of @p data in bytes.
 *  @param bom_len  Out: number of bytes consumed by the BOM (0 if none).
 *  @return The detected BOM type, or N00B_UNICODE_BOM_NONE.
 */
n00b_unicode_bom_t n00b_unicode_detect_bom(const char *data, uint32_t len, uint32_t *bom_len);

// ===========================================================================
// String-level operations
// ===========================================================================

/** @brief Count the number of codepoints in a UTF-8 string.
 *  @param s  The string to examine.
 *  @return Number of codepoints, or negative on invalid UTF-8.
 */
int64_t n00b_unicode_utf8_count_codepoints(n00b_string_t s);

/** @brief Validate that an n00b_string_t contains well-formed UTF-8.
 *  @param s  The string to validate.
 *  @return true if valid UTF-8.
 */
bool n00b_unicode_str_validate(n00b_string_t s);

// ===========================================================================
// Transcoding
// ===========================================================================

/** @brief Convert a UTF-8 string to a UTF-16 array.
 *  @param s        The source UTF-8 string.
 *  @kw allocator   Optional allocator (defaults to the runtime allocator).
 *  @return An array of uint16_t code units.
 */
n00b_array_t(uint16_t)
n00b_unicode_to_utf16(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Convert a UTF-8 string to a UTF-32 codepoint array.
 *  @param s        The source UTF-8 string.
 *  @kw allocator   Optional allocator (defaults to the runtime allocator).
 *  @return An array of codepoints.
 */
n00b_array_t(n00b_codepoint_t)
n00b_unicode_to_utf32(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Convert a UTF-16 array to a new UTF-8 n00b_string_t.
 *  @param src  Source UTF-16 code units.
 *  @param len  Number of uint16_t code units in @p src.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new n00b_string_t containing the UTF-8 representation.
 */
n00b_string_t
n00b_unicode_from_utf16(const uint16_t *src, uint32_t len) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Convert a UTF-32 codepoint array to a new UTF-8 n00b_string_t.
 *  @param src  Source codepoint array.
 *  @param len  Number of codepoints in @p src.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new n00b_string_t containing the UTF-8 representation.
 */
n00b_string_t
n00b_unicode_from_utf32(const n00b_codepoint_t *src, uint32_t len) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
