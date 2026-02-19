/**
 * @file utf8.h
 * @brief Minimal UTF-8 encoding utility.
 *
 * Provides a single function to encode a Unicode codepoint into its
 * UTF-8 byte representation.  This is a placeholder until the full
 * text/UTF-8 subsystem is ported.
 */
#pragma once

#include <stdint.h>
#include "n00b.h"

/**
 * @brief Encode a single Unicode codepoint as UTF-8.
 *
 * Writes 1--4 bytes to @p out.  The caller must ensure @p out has
 * room for at least 4 bytes.
 *
 * @param cp   Unicode codepoint (U+0000 .. U+10FFFF, excluding surrogates).
 * @param out  Output buffer (at least 4 bytes).
 * @return     Number of bytes written (1--4), or 0 if @p cp is invalid.
 *
 * @pre @p out is non-nullptr and points to at least 4 writable bytes.
 */
static inline int
n00b_utf8_encode_codepoint(n00b_codepoint_t cp, uint8_t *out)
{
    if (cp <= 0x7F) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        // Reject surrogates (U+D800..U+DFFF).
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return 0;
        }
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (uint8_t)(0xF0 | (cp >> 18));
        out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}
