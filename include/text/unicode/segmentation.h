#pragma once
/** @file segmentation.h
 *  @brief Text segmentation: grapheme cluster, word, and sentence break iterators.
 *
 *  Provides opaque break iterators for grapheme cluster, word, and sentence
 *  boundaries per UAX #29, plus a convenience function for counting grapheme
 *  clusters.
 */

#include "text/unicode/types_ext.h"

// ===========================================================================
// Opaque break iterator
// ===========================================================================

/** @brief Opaque break iterator for grapheme/word/sentence boundaries. */
typedef struct n00b_unicode_break_iter_s n00b_unicode_break_iter_t;

// ===========================================================================
// Iterator constructors
// ===========================================================================

/** @brief Create a grapheme cluster break iterator over a string.
 *  @param s  The string to iterate.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new break iterator; caller must call
 *          n00b_unicode_break_iter_free().
 */
n00b_unicode_break_iter_t *n00b_unicode_grapheme_iter(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Create a word break iterator over a string.
 *  @param s  The string to iterate.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new break iterator; caller must call
 *          n00b_unicode_break_iter_free().
 */
n00b_unicode_break_iter_t *n00b_unicode_word_iter(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Create a sentence break iterator over a string.
 *  @param s  The string to iterate.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A new break iterator; caller must call
 *          n00b_unicode_break_iter_free().
 */
n00b_unicode_break_iter_t *n00b_unicode_sentence_iter(n00b_string_t *s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

// ===========================================================================
// Iterator navigation
// ===========================================================================

/** @brief Advance the iterator to the next break boundary.
 *  @param it  The break iterator.
 *  @return Byte offset of the next boundary, or -1 if no more boundaries.
 */
int32_t n00b_unicode_break_next(n00b_unicode_break_iter_t *it);

/** @brief Move the iterator to the previous break boundary.
 *  @param it  The break iterator.
 *  @return Byte offset of the previous boundary, or -1 if at the start.
 */
int32_t n00b_unicode_break_prev(n00b_unicode_break_iter_t *it);

/** @brief Free a break iterator.
 *  @param it  The break iterator to free.
 */
void n00b_unicode_break_iter_free(n00b_unicode_break_iter_t *it);

// ===========================================================================
// Convenience
// ===========================================================================

/** @brief Count the number of grapheme clusters in a string.
 *  @param s  The string to examine.
 *  @return The number of grapheme clusters.
 */
uint32_t n00b_unicode_grapheme_count(n00b_string_t *s);
