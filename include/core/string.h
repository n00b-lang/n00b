/**
 * @file string.h
 * @brief Layout and construction helpers for `n00b_string_t`.
 *
 * This header defines the struct layout and declares low-level
 * construction helpers so that other modules (e.g., buffer, unicode)
 * can allocate and populate string objects.  The full string API will
 * be ported separately.
 */
#pragma once

#include "n00b.h"

/**
 * @brief Immutable UTF-8 string with optional styling.
 *
 * @c data points to the raw UTF-8 bytes (not necessarily NUL-terminated).
 * @c styling is reserved for rich-text formatting metadata (TBD).
 */
struct n00b_string_t {
    char  *data;
    size_t u8_bytes;
    size_t codepoints;
    void  *styling;
};

/**
 * @brief Construct an `n00b_string_t` by value from raw UTF-8 data.
 *
 * Allocates a copy of @p src (plus NUL terminator) using @p allocator
 * and returns a populated string struct.  If @p src is nullptr or
 * @p byte_len is 0, returns an empty string.
 *
 * @param allocator  Allocator to use (nullptr = runtime default).
 * @param src        Source UTF-8 bytes (may be nullptr if byte_len == 0).
 * @param byte_len   Number of bytes to copy.
 * @param cp_count   Number of codepoints in the string.
 * @return           A populated `n00b_string_t` (by value).
 *
 * @pre @p byte_len >= 0.
 * @post Returned string's data is NUL-terminated.
 */
extern n00b_string_t n00b_string_from_raw(n00b_allocator_t *allocator,
                                          const char       *src,
                                          int64_t           byte_len,
                                          int64_t           cp_count);

/**
 * @brief Return an empty `n00b_string_t`.
 *
 * @param allocator  Allocator (nullptr = runtime default).
 * @return           An empty string (0 bytes, 0 codepoints).
 */
extern n00b_string_t n00b_string_empty(n00b_allocator_t *allocator);
