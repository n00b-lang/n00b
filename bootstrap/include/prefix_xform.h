/**
 * @file prefix_xform.h
 * @brief Literal modifier (prefix) transformation system.
 *
 * Provides infrastructure for transforming prefixed literals like:
 *   nullify{a, b, c} -> {NULL, NULL, NULL}
 *   dict{k1: v1, k2: v2} -> (some dict initialization)
 *
 * Prefixed literals are detected by the lexer (prefix_len > 0 on tokens).
 * This module provides:
 *   - Registry for prefix handlers
 *   - Callback mechanism for custom transformations
 *   - Integration with the compile flow
 */
#pragma once

#include <stdbool.h>
#include "lex.h"

/**
 * @brief Prefix handler function signature.
 *
 * Called when a prefixed literal is found. The handler receives:
 *   - The lexer state (for access to input and tokens)
 *   - The index of the prefixed token (e.g., the `{` with prefix)
 *   - The index of the closing token (e.g., matching `}`)
 *
 * The handler should modify tok->replacement for tokens it wants to change,
 * and set tok->skip_emit = true for tokens it wants to remove.
 *
 * @param state     Lexer state with tokens
 * @param start_ix  Index of the prefixed opening token
 * @param end_ix    Index of the closing token (inclusive)
 * @return true if transformation was applied, false on error
 */
typedef bool (*prefix_handler_fn)(lex_t *state, int start_ix, int end_ix);

/**
 * @brief Entry in the prefix handler registry.
 */
typedef struct prefix_handler_entry {
    const char              *prefix;  /**< Prefix string to match */
    prefix_handler_fn        handler; /**< Handler function */
    struct prefix_handler_entry *next;    /**< Next entry in list */
} prefix_handler_entry_t;

/**
 * @brief Registry of prefix handlers.
 */
typedef struct {
    prefix_handler_entry_t *handlers; /**< Linked list of handlers */
} prefix_registry_t;

/**
 * @brief Initialize a prefix registry.
 * @param reg Registry to initialize
 */
extern void prefix_registry_init(prefix_registry_t *reg);

/**
 * @brief Free a prefix registry.
 * @param reg Registry to free
 */
extern void prefix_registry_free(prefix_registry_t *reg);

/**
 * @brief Register a prefix handler.
 *
 * @param reg     Registry to add to
 * @param prefix  Prefix string to match (e.g., "nullify")
 * @param handler Handler function to call
 * @return true on success, false on allocation failure
 */
extern bool prefix_register(prefix_registry_t *reg, const char *prefix,
                            prefix_handler_fn handler);

/**
 * @brief Apply prefix transformations to a token stream.
 *
 * Scans for tokens with prefix_len > 0, looks up the prefix in the registry,
 * and calls the appropriate handler.
 *
 * @param reg   Registry of handlers
 * @param state Lexer state with tokens
 * @return true if all transformations succeeded, false on error
 */
extern bool prefix_apply_transforms(prefix_registry_t *reg, lex_t *state);

/**
 * @brief Find the matching close brace for a prefixed brace literal.
 *
 * @param state    Lexer state
 * @param start_ix Index of the opening brace token
 * @return Index of matching closing brace, or -1 if not found
 */
extern int prefix_find_matching_brace(lex_t *state, int start_ix);

/**
 * @brief Check if a string prefix is a C standard prefix.
 *
 * C standard string prefixes (u, U, L, u8) should not be transformed.
 *
 * @param prefix The prefix string
 * @return true if it's a C standard prefix
 */
extern bool prefix_is_c_standard(const char *prefix);

/* ============================================================================
 * Built-in Handlers
 * ============================================================================ */

/**
 * @brief Register all built-in prefix handlers.
 *
 * Calls ncc_register_extra_prefixes() which each backend provides.
 * Bootstrap: no-op. Main ncc: registers r"..." rich string handler.
 *
 * @param reg Registry to add handlers to
 */
extern void prefix_register_builtins(prefix_registry_t *reg);

/**
 * @brief Handler for r"..." rich string literal prefix.
 *
 * Transforms r"Hello, «b»world!«/b»" into a static pre-compiled
 * ctui_string_t * with inline styling data. For format strings with
 * named styles, @roles, or substitutions, emits a static
 * ctui_rich_desc_t and wraps with ctui_rich_desc_apply_arena().
 */
extern bool prefix_handler_rstring(lex_t *state, int start_ix, int end_ix);
