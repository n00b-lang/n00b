#pragma once

/** @file bidi.h
 *  @brief Unicode Bidirectional Algorithm (UAX #9).
 *
 *  Resolves paragraph-level embedding levels and provides a visual
 *  reordering map for rendering bidirectional text.
 */

#include "text/unicode/types_ext.h"
#include "adt/array.h"

n00b_array_decl(uint8_t);
n00b_array_decl(int32_t);

/** @brief Opaque handle for a resolved bidirectional paragraph. */
typedef struct n00b_unicode_bidi_para_s n00b_unicode_bidi_para_t;

/** @brief Open a paragraph for bidirectional analysis.
 *
 *  @note The constructor/destructor pair is `_open`/`_free` (not
 *        `_close`) because the handle owns allocated memory.
 *
 *  @param s  The paragraph text.
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return A resolved bidi paragraph; caller must call
 *          n00b_unicode_bidi_free().
 */
n00b_unicode_bidi_para_t *
n00b_unicode_bidi_open(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/** @brief Return the resolved paragraph embedding level.
 *  @param p  The bidi paragraph.
 *  @return 0 for LTR, 1 for RTL.
 */
uint8_t n00b_unicode_bidi_paragraph_level(const n00b_unicode_bidi_para_t *p);

/** @brief Return a copy of the per-character resolved embedding levels.
 *  @param p    The bidi paragraph.
 *  @return An array of embedding levels (one per codepoint).
 */
n00b_array_t(uint8_t) n00b_unicode_bidi_levels(const n00b_unicode_bidi_para_t *p);

/** @brief Compute a visual reordering map from resolved levels.
 *  @param p  The bidi paragraph.
 *  @return An array where `result.data[visual_pos] = logical_pos`.
 */
n00b_array_t(int32_t) n00b_unicode_bidi_reorder_visual(const n00b_unicode_bidi_para_t *p);

/** @brief Free a bidi paragraph handle.
 *  @param p  The paragraph to free.
 */
void n00b_unicode_bidi_free(n00b_unicode_bidi_para_t *p);
