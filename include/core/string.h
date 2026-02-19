/**
 * @file string.h
 * @brief Stub layout for `n00b_string_t`.
 *
 * This header defines only the struct layout so that other modules
 * (e.g., buffer) can allocate and populate string objects.  The full
 * string API will be ported separately.
 */
#pragma once

#include "n00b.h"

/**
 * @brief Immutable UTF-8 string with optional styling.
 *
 * @c data points to the raw UTF-8 bytes (not necessarily NUL-terminated).
 * @c u32_data is a lazily-computed UTF-32 expansion (may be NULL).
 * @c styling is reserved for rich-text formatting metadata (TBD).
 */
struct n00b_string_t {
    char             *data;
    n00b_codepoint_t *u32_data;
    void             *styling;
    int64_t           codepoints;
    int64_t           u8_bytes;
};
