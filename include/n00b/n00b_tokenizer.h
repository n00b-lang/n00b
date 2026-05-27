#pragma once

/**
 * @file n00b_tokenizer.h
 * @brief N00b language tokenizer callback for the scanner framework.
 *
 * Provides `n00b_lang_tokenize`, a `n00b_scan_cb_t` callback that tokenizes
 * the n00b language. Handles:
 *
 * - All n00b keywords (`if`, `elif`, `else`, `for`, `while`, `from`, `to`,
 *   `break`, `continue`, `return`, `yield`, `switch`, `case`, `func`, `var`,
 *   `global`, `const`, `let`, `once`, `private`, `enum`, `object`, `typeof`, `in`,
 *   `lock`, `and`, `or`, `true`, `false`, `nil`)
 * - Operators: arithmetic, comparison, logical, bitwise, assignment
 * - Compound assignment (`+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`,
 *   `<<=`, `>>=`)
 * - String literals (single-quoted, double-quoted, triple-quoted)
 * - Long literals (`[=encoder[contents]=]'modifier`, level >= 1)
 * - Character literals
 * - Integer, hex, float literals
 * - Line comments (`#`, `//`), block comments
 * - Arrow (`->`), backtick, tilde (`~`)
 * - Literal modifiers (`'modifier` after a literal)
 *
 * ### Token IDs
 *
 * Uses grammar-based terminal lookup when a grammar is attached. Falls
 * back to generic `N00B_TOK_*` constants otherwise.
 */

#include "parsers/scanner.h"

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Scanner callback for n00b language source code.
 *
 * Drop-in `n00b_scan_cb_t` callback. Uses the scanner's grammar for
 * keyword/operator terminal ID lookup when available.
 *
 * Significant whitespace: newlines are emitted as tokens (n00b uses
 * newlines as statement terminators in some contexts). Non-newline
 * whitespace is skipped as trivia.
 *
 * @param s  Scanner with input positioned at next token.
 * @return true if a token was emitted, false at EOF.
 */
bool n00b_lang_tokenize(n00b_scanner_t *s);
