#pragma once

/**
 * @file naudit/tokenizer_registry.h
 * @brief Tokenizer registry for naudit per-file dispatch (WP-009).
 *
 * Naudit looks up a `n00b_scan_cb_t` + per-language scanner-state
 * factory + reset callback by name when constructing the scanner
 * for each audited file. The registry's keys (`"c"`, future
 * `"n00b"`, etc.) match the corresponding language descriptor's
 * `tokenizer_name` field in `naudit/languages.h`.
 *
 * The registry is hand-populated by naudit at startup — it is
 * deliberately **not** libn00b's `parsers/tokenizer_registry.h`
 * surface, which keys by string but does not expose the
 * paired state-new / reset callbacks naudit needs to drive
 * `n00b_scanner_new`.
 *
 * Per project DECISIONS.md D-006, the header filename is
 * unprefixed (`naudit/tokenizer_registry.h`); symbol-level prefix
 * discipline (the `n00b_naudit_` prefix matching WP-008's
 * `n00b_naudit_register_rule_file_tokenizer` precedent) is in
 * force.
 *
 * Headers under `include/naudit/` may be #included standalone, so
 * this file pulls `<n00b.h>` defensively and `parsers/scanner.h`
 * explicitly for the `n00b_scan_cb_t` / `n00b_scan_reset_cb_t`
 * typedefs.
 */

#include <n00b.h>
#include "parsers/scanner.h"

/**
 * @brief One row in the naudit tokenizer registry.
 *
 * The tokenizer-specific scanner state struct is concrete in each
 * language's header (e.g. `n00b_c_tokenizer_state_t` for C), but
 * the registry erases that to `void *` so naudit can dispatch in a
 * language-agnostic fashion. The state pointer round-trips
 * through `n00b_scanner_new`'s `.state` keyword arg, which itself
 * is `void *` — no information is lost.
 *
 * Field meanings:
 *  - `name`        registry key (e.g. `r"c"`).
 *  - `scan_cb`     slay-style scanner callback (e.g.
 *                  `n00b_c_tokenize`).
 *  - `state_new`   allocates a fresh scanner-state struct for the
 *                  tokenizer (returned as `void *`).
 *  - `reset_cb`    optional reset hook; may be `nullptr` for
 *                  tokenizers that hold no resettable state.
 */
typedef struct {
    n00b_string_t       *name;
    n00b_scan_cb_t       scan_cb;
    void                *(*state_new)(void);
    n00b_scan_reset_cb_t reset_cb;
} n00b_naudit_tokenizer_info_t;

/**
 * @brief Look up a tokenizer descriptor by name.
 *
 * Returns the registered row whose `name` matches `name`, or
 * `nullptr` if no tokenizer is registered under that key. Names
 * are case-sensitive bytewise compares.
 *
 * Per project DECISIONS.md D-005 / D-017, this function carries
 * no `_kargs` block — naudit's surface does not expose
 * `.allocator` keyword arguments.
 *
 * @param name  Tokenizer name to look up.
 *
 * @return Pointer to the registered descriptor, or `nullptr` if
 *         not found. The returned pointer has process lifetime.
 */
extern n00b_naudit_tokenizer_info_t *
n00b_naudit_lookup_tokenizer(n00b_string_t *name);
