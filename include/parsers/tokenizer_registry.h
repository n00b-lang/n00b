#pragma once

/**
 * @file tokenizer_registry.h
 * @brief Global tokenizer registry mapping names to scan callbacks.
 *
 * Grammars can declare a tokenizer by name via `@tokenizer("name")`.
 * The registry resolves these names to `n00b_scan_cb_t` callbacks at
 * scanner creation time.
 *
 * Built-in tokenizers:
 *
 * | Name          | Callback                | Description                     |
 * |---------------|-------------------------|---------------------------------|
 * | `"text"`      | `n00b_text_tokenize`    | UAX#31 words + scan recipes     |
 * | `"character"` | `nullptr`               | Character-level (parser native) |
 * | `"shell"`     | `n00b_shell_tokenize`   | Shell/command-line lexing        |
 * | `"lisp"`      | `n00b_lisp_tokenize`    | S-expression lexing              |
 * | `"json"`      | `n00b_json_tokenize`    | JSON structural tokens           |
 * | `"c"`         | `n00b_c_tokenize`       | C23/ncc source code              |
 * | `"n00b"`      | `n00b_lang_tokenize`    | n00b language source             |
 *
 * ### Usage
 *
 * ```c
 * n00b_scan_cb_t cb = n00b_tokenizer_lookup("json");
 * if (cb) {
 *     n00b_scanner_t *s = n00b_scanner_new(buf, cb, grammar);
 * }
 * ```
 */

#include "parsers/scanner.h"

// ============================================================================
// Registry API
// ============================================================================

/**
 * @brief Register a named tokenizer callback.
 *
 * A `nullptr` callback is valid — it means "character-level mode"
 * (the parser's native operation with no tokenizer).
 *
 * @param name  Tokenizer name (e.g. `"text"`, `"json"`).
 * @param cb    Scan callback, or `nullptr` for character-level.
 */
extern void n00b_tokenizer_register(const char *name, n00b_scan_cb_t cb);

/**
 * @brief Look up a tokenizer by name.
 *
 * @param name  Tokenizer name.
 * @param found Set to `true` if the name was found, `false` otherwise.
 *              Distinguishes "registered as nullptr" from "not found".
 * @return The scan callback, or `nullptr` if not found or if the
 *         tokenizer is character-level.
 */
extern n00b_scan_cb_t n00b_tokenizer_lookup(const char *name, bool *found);

/**
 * @brief Initialize all built-in tokenizers.
 *
 * Called once from `n00b_init()`.  Registers `text`, `character`,
 * `shell`, `lisp`, `json`, `c`, and `n00b`.
 */
extern void n00b_tokenizers_init(void);
