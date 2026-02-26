#pragma once

/**
 * @file c_tokenizer.h
 * @brief C/ncc tokenizer callback for the scanner framework.
 *
 * Provides `n00b_c_tokenize`, a `n00b_scan_cb_t` callback that tokenizes
 * C23 + ncc extension source code. Handles:
 *
 * - All C23 keywords and types
 * - GCC/clang extension keywords (`__attribute__`, `__builtin_*`, etc.)
 * - ncc extension keywords (`typeid`, `_kargs`, `once`, `package`,
 *   `typestr`, `typehash`, `constexpr_*`, `kw_func`, etc.)
 * - String/char literals with encoding prefixes (L, u, U, u8)
 * - Integer, float, hex, octal, binary literals with C23 digit separators
 * - All C operators (including multi-character: `<<=`, `>>=`, `->`, etc.)
 * - Line comments (`//`), block comments
 * - Preprocessor directives (skipped as trivia)
 * - `#pragma ncc off/on` tracking
 *
 * ### Usage
 *
 * ```c
 * n00b_grammar_t *g = load_c_grammar();
 * n00b_buffer_t *buf = n00b_buffer_from_bytes(src, len);
 * n00b_scanner_t *s = n00b_scanner_new(buf, n00b_c_tokenize, g);
 * n00b_token_stream_t *ts = n00b_token_stream_new(s);
 * ```
 */

#include "parsers/scanner.h"

// ============================================================================
// Tokenizer state (user_state inside the scanner)
// ============================================================================

/**
 * @brief Per-scan state for the C tokenizer.
 *
 * Stored in `scanner->user_state`. Tracks `#pragma ncc off/on` ranges
 * and `#line` source file info.
 */
typedef struct {
    const char *current_file;    /**< Current source file (from #line). */
    bool        ncc_off;         /**< True when inside `#pragma ncc off`. */
    bool        in_system_header;/**< True when inside a system header. */
} n00b_c_tokenizer_state_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Scanner callback for C/ncc source code.
 *
 * Drop-in `n00b_scan_cb_t` callback. Uses the scanner's grammar for
 * keyword/operator terminal ID lookup. Falls back to generic token
 * IDs (hash-based for literal types like `"IDENTIFIER"`, `"INTEGER"`)
 * when no grammar is attached.
 *
 * @param s  Scanner with input positioned at next token.
 * @return true if a token was emitted, false at EOF.
 */
bool n00b_c_tokenize(n00b_scanner_t *s);

/**
 * @brief Reset callback for the C tokenizer.
 *
 * Resets `ncc_off`, `in_system_header`, and `current_file` in the
 * tokenizer state. Passed as `reset_cb` to `n00b_scanner_new`.
 */
void n00b_c_tokenizer_reset(n00b_scanner_t *s);

/**
 * @brief Allocate and initialize a C tokenizer state.
 *
 * Call this and pass the result as the `state` keyword arg to
 * `n00b_scanner_new()`.
 */
n00b_c_tokenizer_state_t *n00b_c_tokenizer_state_new(void);

/**
 * @brief Check whether the tokenizer is currently in an `ncc off` region.
 */
bool n00b_c_tokenizer_is_ncc_off(n00b_scanner_t *s);
