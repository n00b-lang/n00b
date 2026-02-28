#pragma once
/** @file linebreak.h
 *  @brief Unicode line break analysis and text wrapping (UAX #14).
 *
 *  Computes per-codepoint line break actions and wraps text to a target
 *  column width using Unicode line break rules.
 */

#include "text/unicode/types_ext.h"
#include "adt/array.h"
#include "core/runtime.h" // for n00b_array_decl(uint32_t)

/** @brief Compute line break actions for each codepoint in a string.
 *
 *  `out[i]` contains the break action BEFORE codepoint i.
 *
 *  @param s    The input string.
 *  @param out  Output array of break actions (caller-allocated).
 *
 *  @pre @p out must have at least `n00b_unicode_utf8_count_codepoints(s)`
 *       elements.  If `s.codepoints` has been computed, that value may
 *       be used directly.
 */
void n00b_unicode_linebreaks(n00b_string_t s, n00b_unicode_lb_action_t *out);

/** @brief Wrap a string to a given column width using Unicode line break rules.
 *
 *  The first line uses the full @p width; subsequent lines use
 *  `width - hang` to support hanging indentation.
 *
 *  When no valid soft-break exists before the column limit, the text is
 *  hard-wrapped (forced break mid-word) unless @p no_hard_wrap is set.
 *
 *  @param s           The input string.
 *  @kw width         Target line width in columns (default: 80).
 *  @kw hang          Hanging indent in columns for continuation lines
 *                    (default: 0).
 *  @kw no_hard_wrap  If true, never force-break inside a word (default:
 *                    false).
 *  @kw allocator     Optional allocator (defaults to the runtime allocator).
 *  @return An array of byte offsets where lines break.
 */
n00b_array_t(uint32_t) n00b_unicode_linebreak_wrap(n00b_string_t s)
    _kargs { int width = 80; int hang = 0; bool no_hard_wrap = false;
             n00b_allocator_t *allocator = nullptr; };
