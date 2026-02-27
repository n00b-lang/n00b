#pragma once

/**
 * @file c_tokenizer.h
 * @brief C/ncc tokenizer callback for the scanner framework.
 *
 * Provides `n00b_c_tokenize`, a `n00b_scan_cb_t` callback that tokenizes
 * C23 + ncc extension source code. Handles:
 *
 * - All C23 keywords and types
 * - GCC/clang extension keywords
 * - ncc extension keywords (typeid, _kargs, once, etc.)
 * - String/char literals with encoding prefixes (L, u, U, u8)
 * - Integer, float, hex, octal, binary literals with suffixes
 * - All C operators (including multi-character)
 * - Line comments, block comments
 * - Preprocessor directives (skipped as trivia)
 * - `#pragma ncc off/on` tracking
 */

#include "parsers/scanner.h"

// ============================================================================
// Tokenizer state
// ============================================================================

typedef struct {
    const char *current_file;
    bool        ncc_off;
    bool        in_system_header;
} n00b_c_tokenizer_state_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Scanner callback for C/ncc source code.
 * @param s  Scanner with input positioned at next token.
 * @return true if a token was emitted, false at EOF.
 */
bool n00b_c_tokenize(n00b_scanner_t *s);

/**
 * @brief Reset callback for the C tokenizer.
 */
void n00b_c_tokenizer_reset(n00b_scanner_t *s);

/**
 * @brief Allocate and initialize a C tokenizer state.
 */
n00b_c_tokenizer_state_t *n00b_c_tokenizer_state_new(void);

/**
 * @brief Check whether the tokenizer is in an `ncc off` region.
 */
bool n00b_c_tokenizer_is_ncc_off(n00b_scanner_t *s);
