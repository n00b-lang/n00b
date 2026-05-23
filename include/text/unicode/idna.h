#pragma once

/** @file idna.h
 *  @brief Internationalized Domain Names in Applications (IDNA / UTS #46).
 *
 *  Converts between Unicode domain names and their ASCII-Compatible Encoding
 *  (ACE / Punycode) form, applying UTS #46 mapping, normalization, and
 *  validity checks in both directions.
 */

#include "text/unicode/types_ext.h"

/** @brief Convert a domain name to its ASCII-Compatible Encoding (ACE) form.
 *
 *  Applies UTS #46 mapping, normalization, validity checks, and Punycode
 *  encoding.
 *
 *  @param domain  The Unicode domain name to convert.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A result containing the ACE domain string and an error code.
 *
 *  @details On invalid UTF-8 in the input (truncated multi-byte sequence,
 *  overlong encoding, lone surrogate, lone continuation byte, etc.) the
 *  error code is `N00B_UNICODE_IDNA_PROCESSING_ERROR` and the result value
 *  is an empty string; callers must check `.error` rather than relying on
 *  the value being non-empty.  An `N00B_UNICODE_IDNA_OK` return on a
 *  non-empty input is guaranteed to carry a non-empty ACE form.
 */
n00b_unicode_idna_result_t
n00b_unicode_idna_to_ascii(n00b_string_t *domain) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Convert an ACE domain name back to its Unicode form.
 *
 *  Applies Punycode decoding, UTS #46 mapping, and validity checks.
 *
 *  @param domain  The ACE domain name to convert.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A result containing the Unicode domain string and an error code.
 */
n00b_unicode_idna_result_t
n00b_unicode_idna_to_unicode(n00b_string_t *domain) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
