#pragma once
/** @file normalization.h
 *  @brief Unicode normalization (NFC, NFD, NFKC, NFKD) and streaming normalizer.
 *
 *  Provides one-shot normalization of entire strings, quick-check predicates,
 *  and a streaming normalizer for incremental codepoint-by-codepoint processing.
 */

#include "unicode/types_ext.h"

// ===========================================================================
// One-shot normalization
// ===========================================================================

/** @brief Normalize a string to NFC (Canonical Decomposition + Canonical
 *         Composition).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new NFC-normalized string.
 */
n00b_string_t n00b_unicode_nfc(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Normalize a string to NFD (Canonical Decomposition).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new NFD-normalized string.
 */
n00b_string_t n00b_unicode_nfd(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Normalize a string to NFKC (Compatibility Decomposition + Canonical
 *         Composition).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new NFKC-normalized string.
 */
n00b_string_t n00b_unicode_nfkc(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Normalize a string to NFKD (Compatibility Decomposition).
 *  @param s  The input string.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new NFKD-normalized string.
 */
n00b_string_t n00b_unicode_nfkd(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===========================================================================
// Quick-check predicates
// ===========================================================================

/** @brief Quick-check whether a string is already in NFC form.
 *  @param s  The string to test.
 *  @return true if the string is already NFC.
 */
bool n00b_unicode_is_nfc(n00b_string_t s);

/** @brief Quick-check whether a string is already in NFD form.
 *  @param s  The string to test.
 *  @return true if the string is already NFD.
 */
bool n00b_unicode_is_nfd(n00b_string_t s);

// ===========================================================================
// Streaming normalizer
// ===========================================================================

/** @brief Opaque streaming normalizer for incremental codepoint processing. */
typedef struct n00b_unicode_normalizer_s n00b_unicode_normalizer_t;

/** @brief Create a new streaming normalizer.
 *  @param form  The normalization form to produce.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new normalizer instance; caller must call
 *          n00b_unicode_normalizer_free().
 */
n00b_unicode_normalizer_t *n00b_unicode_normalizer_new(
    n00b_unicode_norm_form_t form)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Feed a single codepoint into the streaming normalizer.
 *  @param n   The normalizer instance.
 *  @param cp  The codepoint to feed.
 */
void n00b_unicode_normalizer_feed(n00b_unicode_normalizer_t *n,
                                  n00b_codepoint_t cp);

/** @brief Read available normalized codepoints from the normalizer.
 *  @param n    The normalizer instance.
 *  @param out  Output buffer for normalized codepoints.
 *  @param max  Maximum number of codepoints to read.
 *  @return Number of codepoints written to @p out.
 */
size_t n00b_unicode_normalizer_read(n00b_unicode_normalizer_t *n,
                                    n00b_codepoint_t *out, size_t max);

/** @brief Flush remaining buffered codepoints from the normalizer.
 *  @param n    The normalizer instance.
 *  @param out  Output buffer for remaining codepoints.
 *  @param max  Maximum number of codepoints to read.
 *  @return Number of codepoints written to @p out.
 */
size_t n00b_unicode_normalizer_flush(n00b_unicode_normalizer_t *n,
                                     n00b_codepoint_t *out, size_t max);

/** @brief Free a streaming normalizer instance.
 *  @param n  The normalizer to free.
 */
void n00b_unicode_normalizer_free(n00b_unicode_normalizer_t *n);
