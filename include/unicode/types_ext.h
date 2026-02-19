#pragma once
/** @file types_ext.h
 *  @brief Extended types for the n00b unicode public API (requires ncc).
 *
 *  Builds on `unicode/types.h` with option types and result structures
 *  that depend on the ncc compiler extensions.
 */

#include "unicode/types.h"
#include "n00b.h"
#include "core/option.h"
#include "core/string.h"

// ===========================================================================
// Option types for API functions that may fail
// ===========================================================================

n00b_option_decl(int32_t);
n00b_option_decl(uint32_t);
n00b_option_decl(n00b_codepoint_t);

/** @brief Optional int32_t result (used by find/rfind, digit_value, etc.) */
typedef n00b_option_t(int32_t) n00b_unicode_opt_i32_t;

// ===========================================================================
// Result types
// ===========================================================================

/** @brief Result of an IDNA domain name conversion. */
typedef struct n00b_unicode_idna_result_t {
    n00b_string_t              value; /**< The converted domain string (empty on error) */
    n00b_unicode_idna_error_t  error; /**< Error code (N00B_UNICODE_IDNA_OK on success) */
} n00b_unicode_idna_result_t;

/** @brief Result of an emoji sequence scan at a given byte position. */
typedef struct n00b_unicode_emoji_scan_result_t {
    n00b_unicode_emoji_type_t  type;      /**< The type of emoji sequence found */
    uint32_t                   seq_bytes; /**< Number of bytes consumed by the sequence */
} n00b_unicode_emoji_scan_result_t;
