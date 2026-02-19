#pragma once
/** @file bidi.h
 *  @brief Unicode Bidirectional Algorithm (UAX #9).
 *
 *  Resolves paragraph-level embedding levels and provides a visual
 *  reordering map for rendering bidirectional text.
 */

#include "unicode/types_ext.h"

/** @brief Opaque handle for a resolved bidirectional paragraph. */
typedef struct n00b_unicode_bidi_para_s n00b_unicode_bidi_para_t;

/** @brief Open a paragraph for bidirectional analysis.
 *  @param s  The paragraph text.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A resolved bidi paragraph; caller must call
 *          n00b_unicode_bidi_free().
 */
n00b_unicode_bidi_para_t *n00b_unicode_bidi_open(n00b_string_t s)
    _kargs { n00b_allocator_t *allocator = nullptr; };

/** @brief Return the resolved paragraph embedding level.
 *  @param p  The bidi paragraph.
 *  @return 0 for LTR, 1 for RTL.
 */
uint8_t n00b_unicode_bidi_paragraph_level(
    const n00b_unicode_bidi_para_t *p);

/** @brief Return the per-character resolved embedding levels.
 *  @param p    The bidi paragraph.
 *  @param len  Out: number of levels returned.
 *  @return Pointer to an array of embedding levels (one per codepoint).
 */
const uint8_t *n00b_unicode_bidi_levels(
    const n00b_unicode_bidi_para_t *p, uint32_t *len);

/** @brief Compute a visual reordering map from resolved levels.
 *  @param p           The bidi paragraph.
 *  @param visual_map  Output array (caller-allocated, one entry per
 *                     codepoint). `visual_map[visual_pos] = logical_pos`.
 */
void n00b_unicode_bidi_reorder_visual(const n00b_unicode_bidi_para_t *p,
                                      int32_t *visual_map);

/** @brief Free a bidi paragraph handle.
 *  @param p  The paragraph to free.
 */
void n00b_unicode_bidi_free(n00b_unicode_bidi_para_t *p);
