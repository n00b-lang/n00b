#pragma once

/**
 * @file shell_tokenizer.h
 * @brief Shell/command-line tokenizer.
 *
 * Produces tokens suitable for parsing shell-like grammars.
 * Newlines are significant (emitted as `NEWLINE`).
 *
 * ### Token types emitted
 *
 * - `NEWLINE` — Newline character (significant in shell grammars)
 * - `STRING_LIT` — Double-quoted (with escapes) or single-quoted (raw) string
 * - `VAR_REF` — `$name` or `${name}` variable reference
 * - `INTEGER` — Integer literal
 * - `FLOAT` — Floating-point literal
 * - `WORD` — Unquoted word (keyword fallback via grammar)
 * - `OTHER` — Unrecognized single character
 * - Single special characters are emitted as fixed-text terminals
 *
 * ### Usage
 *
 * ```c
 * n00b_scanner_t *s = n00b_scanner_new(buf, n00b_shell_tokenize, grammar);
 * ```
 */

#include "parsers/scanner.h"

/**
 * @brief Scanner callback for shell/command-line tokenization.
 *
 * @param s  Scanner with input positioned at next token.
 * @return `true` if a token was emitted, `false` at EOF.
 */
extern bool n00b_shell_tokenize(n00b_scanner_t *s);
