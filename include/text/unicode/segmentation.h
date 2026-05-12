#pragma once
/** @file segmentation.h
 *  @brief Text segmentation: grapheme cluster, word, and sentence break iterators.
 *
 *  Provides opaque break iterators for grapheme cluster, word, and sentence
 *  boundaries per UAX #29, plus a convenience function for counting grapheme
 *  clusters.
 *
 *  Also provides inverse-range accessors that turn a Grapheme_Cluster_Break /
 *  Word_Break / Sentence_Break / Line_Break enum value into a sorted, merged
 *  list of `[lo, hi]` codepoint ranges, for use by regex property-class
 *  resolution (e.g. `\p{Grapheme_Cluster_Break=Extend}`).
 */

#include "text/unicode/types_ext.h"
#include <stddef.h>

// `n00b_codepoint_pair_t` is defined in `text/unicode/types.h` (transitively
// included via `types_ext.h` above).

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

// ===========================================================================
// Inverse-range accessors
//
// Each function returns, via @p out / @p len, a sorted, non-overlapping,
// non-contiguous array of `[lo, hi]` codepoint ranges covering every
// codepoint whose break property has the given enum value. The surrogate
// hole (U+D800..U+DFFF) is excluded. The returned array has static
// lifetime (it is computed lazily on first call and cached for the
// lifetime of the process). The caller MUST NOT free or modify it.
//
// If @p v is out of range for its enum, both `*out = nullptr` and
// `*len = 0` are written and the call returns.
// ===========================================================================

/** @brief Get the codepoint ranges with a given Grapheme_Cluster_Break value.
 *  @param v    The Grapheme_Cluster_Break enum value to enumerate.
 *  @param out  Receives a pointer to a static array of `[lo, hi]` pairs.
 *  @param len  Receives the number of pairs.
 */
void n00b_unicode_grapheme_break_ranges(n00b_unicode_gcb_t            v,
                                        const n00b_codepoint_pair_t **out,
                                        size_t                       *len);

/** @brief Get the codepoint ranges with a given Word_Break value.
 *  @param v    The Word_Break enum value to enumerate.
 *  @param out  Receives a pointer to a static array of `[lo, hi]` pairs.
 *  @param len  Receives the number of pairs.
 */
void n00b_unicode_word_break_ranges(n00b_unicode_wb_t             v,
                                    const n00b_codepoint_pair_t **out,
                                    size_t                       *len);

/** @brief Get the codepoint ranges with a given Sentence_Break value.
 *  @param v    The Sentence_Break enum value to enumerate.
 *  @param out  Receives a pointer to a static array of `[lo, hi]` pairs.
 *  @param len  Receives the number of pairs.
 */
void n00b_unicode_sentence_break_ranges(n00b_unicode_sb_t             v,
                                        const n00b_codepoint_pair_t **out,
                                        size_t                       *len);

/** @brief Get the codepoint ranges with a given Line_Break value.
 *  @param v    The Line_Break enum value to enumerate.
 *  @param out  Receives a pointer to a static array of `[lo, hi]` pairs.
 *  @param len  Receives the number of pairs.
 */
void n00b_unicode_line_break_ranges(n00b_unicode_lb_t             v,
                                    const n00b_codepoint_pair_t **out,
                                    size_t                       *len);

// ===========================================================================
// Property-name -> enum lookups (for regex \p{...} resolution)
//
// These take a segmentation property-value name (as written in `\p{...}` in
// a regex) and return the corresponding enum value via an out parameter.
// Matching is "loose" per UAX #44 LM3: case-insensitive (ASCII fold) and
// ignoring whitespace, underscores, and hyphens.
//
// All return false (and leave *out unchanged) if the name does not match.
// All accept @c nullptr for @p name (return false) and @p out (skip write).
// ===========================================================================

/** @brief Look up a Grapheme_Cluster_Break value by name (e.g. "Extend",
 *         "EX").
 *  @param name  The property-value name to look up.
 *  @param out   Out-parameter receiving the enum value on success.
 *  @return      True on a match, false otherwise.
 */
bool n00b_unicode_gcb_by_name(const char *name, n00b_unicode_gcb_t *out);

/** @brief Look up a Word_Break value by name (e.g. "ALetter", "LE").
 *  @param name  The property-value name to look up.
 *  @param out   Out-parameter receiving the enum value on success.
 *  @return      True on a match, false otherwise.
 */
bool n00b_unicode_wb_by_name(const char *name, n00b_unicode_wb_t *out);

/** @brief Look up a Sentence_Break value by name (e.g. "ATerm", "AT").
 *  @param name  The property-value name to look up.
 *  @param out   Out-parameter receiving the enum value on success.
 *  @return      True on a match, false otherwise.
 */
bool n00b_unicode_sb_by_name(const char *name, n00b_unicode_sb_t *out);

/** @brief Look up a Line_Break value by name (e.g. "Alphabetic", "AL").
 *  @param name  The property-value name to look up.
 *  @param out   Out-parameter receiving the enum value on success.
 *  @return      True on a match, false otherwise.
 */
bool n00b_unicode_lb_by_name(const char *name, n00b_unicode_lb_t *out);
