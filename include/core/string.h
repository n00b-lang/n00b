/**
 * @file string.h
 * @brief Layout and construction helpers for `n00b_string_t`.
 *
 * `n00b_string_t` is a **value type** (40 bytes, passed/returned by
 * value).  This contrasts with `n00b_buffer_t`, which is a **heap
 * type** (passed by pointer).
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

// Declare option_t(n00b_string_t) here (next to the struct definition)
// so any header using optional strings can rely on this single declaration.
#include "adt/option.h"
n00b_option_decl(n00b_string_t);

/**
 * @brief Static initializer for an `n00b_string_t` from an ASCII literal.
 *
 * Usable at file scope (static struct initializers) where `*r"..."`
 * cannot appear because ncc's statement-expression lowering is not
 * valid outside function bodies.
 *
 * @param lit  A string literal (must be pure ASCII — codepoints == bytes).
 */
#define N00B_STRING_STATIC(lit) \
    ((n00b_string_t){ .data = (char *)(lit), .u8_bytes = sizeof(lit) - 1, \
                      .codepoints = sizeof(lit) - 1, .styling = nullptr })

/**
 * @brief Construct an `n00b_string_t` by value from UTF-8 data with known length.
 *
 * Allocates a copy of @p src (plus NUL terminator), counts UTF-8
 * codepoints internally, and returns a populated string struct.
 *
 * @param src        Source UTF-8 bytes (may be nullptr if byte_len == 0).
 * @param byte_len   Number of bytes to copy.
 * @return           A populated `n00b_string_t` (by value).
 *
 * @kw allocator  Allocator to use (nullptr = runtime default).
 * @kw cp_count   Optional output pointer for the codepoint count.
 *
 * @pre @p byte_len >= 0.
 * @post Returned string's data is NUL-terminated.
 */
extern n00b_string_t n00b_string_from_raw(const char *src,
                                          int64_t     byte_len)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        int64_t          *cp_count  = nullptr;
    };

/**
 * @brief Construct an `n00b_string_t` from a NUL-terminated C string.
 *
 * Computes byte length and counts UTF-8 codepoints.
 *
 * @param src  NUL-terminated C string.
 * @return     A populated `n00b_string_t` (by value).
 *
 * @kw allocator  Allocator to use (nullptr = runtime default).
 */
extern n00b_string_t n00b_string_from_cstr(const char *src)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/**
 * @brief Return an empty `n00b_string_t`.
 *
 * @kw allocator  Allocator (nullptr = runtime default).
 * @return        An empty string (0 bytes, 0 codepoints).
 */
extern n00b_string_t n00b_string_empty()
    _kargs { n00b_allocator_t *allocator = nullptr; };
