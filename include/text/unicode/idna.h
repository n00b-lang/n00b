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
 */
n00b_unicode_idna_result_t
n00b_unicode_idna_to_ascii(n00b_string_t domain) _kargs
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
n00b_unicode_idna_to_unicode(n00b_string_t domain) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
