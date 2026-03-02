#pragma once

/**
 * @file json_tokenizer.h
 * @brief JSON tokenizer.
 *
 * Produces tokens suitable for parsing JSON grammars per RFC 8259.
 *
 * ### Token types emitted
 *
 * - `STRING_LIT` — Double-quoted JSON string
 * - `NUMBER` — JSON number (integer or float)
 * - `OTHER` — Unrecognized input (error recovery)
 * - Structural characters (`{`, `}`, `[`, `]`, `:`, `,`) as fixed-text terminals
 * - Keywords `true`, `false`, `null` as fixed-text terminals
 *
 * ### Usage
 *
 * ```c
 * n00b_scanner_t *s = n00b_scanner_new(buf, n00b_json_tokenize, grammar);
 * ```
 */

#include "parsers/scanner.h"

/**
 * @brief Scanner callback for JSON tokenization.
 *
 * @param s  Scanner with input positioned at next token.
 * @return `true` if a token was emitted, `false` at EOF.
 */
extern bool n00b_json_tokenize(n00b_scanner_t *s);
