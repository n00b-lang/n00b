#pragma once

/**
 * @file text_tokenizer.h
 * @brief General-purpose text tokenizer for natural/structured text.
 *
 * Produces tokens using UAX#31 identifiers, scan recipes for strings
 * and numbers, and single-codepoint terminals for punctuation.
 * This is the default tokenizer when no `@tokenizer` annotation is
 * present in a grammar.
 *
 * ### Token types emitted
 *
 * - `IDENTIFIER` — UAX#31 identifier (keyword fallback via grammar)
 * - `STRING_LIT` — Double- or single-quoted string literal
 * - `INTEGER` — Integer literal (decimal, hex, octal, binary)
 * - `FLOAT` — Floating-point literal
 * - Single punctuation codepoints are emitted as fixed-text terminals
 *
 * ### Usage
 *
 * ```c
 * n00b_scanner_t *s = n00b_scanner_new(buf, n00b_text_tokenize, grammar);
 * ```
 */

#include "parsers/scanner.h"

/**
 * @brief Scanner callback for general-purpose text tokenization.
 *
 * @param s  Scanner with input positioned at next token.
 * @return `true` if a token was emitted, `false` at EOF.
 */
extern bool n00b_text_tokenize(n00b_scanner_t *s);
