#pragma once
/** @file linebreak.h
 *  @brief Unicode line break analysis and text wrapping (UAX #14).
 *
 *  Computes per-codepoint line break actions and wraps text to a target
 *  column width using Unicode line break rules.
 */

#include "unicode/types_ext.h"

/** @brief Compute line break actions for each codepoint in a string.
 *
 *  `out[i]` contains the break action BEFORE codepoint i. The caller must
 *  allocate @p out with at least num_codepoints elements.
 *
 *  @param s    The input string.
 *  @param out  Output array of break actions (caller-allocated).
 */
void n00b_unicode_linebreaks(n00b_string_t s, n00b_unicode_lb_action_t *out);

/** @brief Wrap a string to a given column width using Unicode line break rules.
 *  @param s           The input string.
 *  @param num_breaks  Out: number of break offsets returned.
 *  @kw width      Target line width in columns (default: 80).
 *  @kw allocator  Optional allocator (defaults to the runtime allocator).
 *  @return Allocated array of byte offsets where lines break; caller frees.
 */
uint32_t *n00b_unicode_linebreak_wrap(n00b_string_t s, uint32_t *num_breaks)
    _kargs { int width = 80; n00b_allocator_t *allocator = nullptr; };
