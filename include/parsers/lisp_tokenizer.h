#pragma once

/**
 * @file lisp_tokenizer.h
 * @brief Lisp/S-expression tokenizer.
 *
 * Produces tokens suitable for parsing Lisp-like grammars.
 *
 * ### Token types emitted
 *
 * - `STRING_LIT` — Double-quoted string literal
 * - `INTEGER` — Integer literal
 * - `FLOAT` — Floating-point literal
 * - `SYMBOL` — Lisp symbol (everything except whitespace, parens, quotes, semicolons)
 * - Parentheses and boolean literals (`#t`, `#f`) as fixed-text terminals
 *
 * ### Usage
 *
 * ```c
 * n00b_scanner_t *s = n00b_scanner_new(buf, n00b_lisp_tokenize, grammar);
 * ```
 */

#include "parsers/scanner.h"

/**
 * @brief Scanner callback for Lisp/S-expression tokenization.
 *
 * @param s  Scanner with input positioned at next token.
 * @return `true` if a token was emitted, `false` at EOF.
 */
extern bool n00b_lisp_tokenize(n00b_scanner_t *s);
