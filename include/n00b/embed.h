#pragma once

/**
 * @file embed.h
 * @brief Embed literal handler registration and dispatch.
 *
 * Embed literals (`[=[...]=]'modifier`) allow mini-languages to be
 * embedded directly in n00b source code.  Each modifier name (e.g.
 * `ffi`, `regex`, `x509`) is backed by a registered handler.
 *
 * Two kinds of handlers:
 *
 * - **Grammar-based**: provide a BNF string.  The embed content is
 *   tokenized and parsed using that grammar; the handler receives the
 *   resulting parse tree.
 *
 * - **Buffer-based**: the embed content (after encoding decode) is
 *   handed directly to the handler as a buffer.
 *
 * Grammar-based handlers get their grammar cached after the first use
 * so subsequent embeds with the same modifier reuse it.
 *
 * The encoding field (`[=enc[...]=]`) specifies how to decode the raw
 * bytes before handing them to the handler.  Encoding support is not
 * yet implemented; when absent the content is passed as-is.
 */

#include "slay/grammar.h"
#include "slay/parse_forest.h"
#include "slay/parse_tree.h"
#include "parsers/scanner.h"
#include "core/buffer.h"
#include "adt/dict_untyped.h"
#include "adt/variant.h"

// ============================================================================
// Embed handler input — variant of parse tree or buffer
// ============================================================================

/**
 * @brief Input to an embed handler callback.
 *
 * For grammar-based handlers this holds an `n00b_parse_tree_t *`.
 * For buffer-based handlers this holds an `n00b_buffer_t *`.
 */
typedef n00b_variant_t(n00b_parse_tree_t *, n00b_buffer_t *) n00b_embed_input_t;

// ============================================================================
// Handler callback (forward-declared session; returns void* cast by caller)
// ============================================================================

/**
 * @brief Embed handler callback signature.
 *
 * The return value is an opaque 16-byte blob (same layout as
 * `n00b_cg_val_t`); the codegen layer casts it back.  This avoids a
 * circular dependency on codegen.h.
 *
 * @param session   Active codegen session (opaque to handlers that
 *                  don't need it).
 * @param input     Variant holding parse tree or buffer.
 * @param user_data Opaque pointer from handler registration.
 */
typedef struct n00b_cg_session_t n00b_cg_session_t;

typedef struct {
    uint8_t data[16];
} n00b_embed_result_t;

typedef n00b_embed_result_t (*n00b_embed_handler_fn)(
    n00b_cg_session_t  *session,
    n00b_embed_input_t  input,
    void               *user_data);

// ============================================================================
// Handler descriptor
// ============================================================================

/**
 * @brief Registered embed literal handler.
 *
 * @details Grammar-based handlers set `bnf` to a BNF string.  On
 * first use the grammar is built via `n00b_bnf_load()` and cached in
 * `grammar`.  An optional custom `tokenizer` overrides the default
 * tokenizer derived from the BNF.
 *
 * Buffer-based handlers leave `bnf` NULL; the embed content (after
 * encoding decode) is passed directly as a buffer.
 */
typedef struct n00b_embed_handler_t {
    n00b_string_t         *name;       /**< Modifier name (lookup key). */
    n00b_string_t         *bnf;        /**< BNF grammar text (NULL = buffer mode). */
    n00b_scan_cb_t         tokenizer;  /**< Custom tokenizer (NULL = auto from BNF). */
    n00b_grammar_t        *grammar;    /**< Cached grammar (built on first use). */
    n00b_embed_handler_fn  handler;    /**< Handler callback. */
    void                  *user_data;  /**< Opaque data for handler. */
    bool                   const_eval; /**< Pre-evaluate at compile time. */
} n00b_embed_handler_t;

// ============================================================================
// Registry lifecycle
// ============================================================================

/**
 * @brief Allocate an empty embed handler registry.
 * @return New dict for use with `n00b_embed_register()`.
 */
extern n00b_dict_untyped_t *n00b_embed_registry_new(void);

// ============================================================================
// Registration
// ============================================================================

/**
 * @brief Register an embed literal handler.
 *
 * @param registry  Dict mapping modifier name → handler.
 * @param handler   Handler descriptor (copied into registry).
 *
 * @pre `handler->name` must be non-NULL.
 * @pre `handler->handler` must be non-NULL.
 */
extern void n00b_embed_register(n00b_dict_untyped_t  *registry,
                                 n00b_embed_handler_t *handler);

/**
 * @brief Look up a registered embed handler by modifier name.
 *
 * @param registry  Handler registry.
 * @param name      Modifier name to look up.
 * @return Handler pointer, or NULL if not registered.
 */
extern n00b_embed_handler_t *
n00b_embed_lookup(n00b_dict_untyped_t *registry,
                   n00b_string_t       *name);

// ============================================================================
// Dispatch
// ============================================================================

/**
 * @brief Parse and dispatch an embed literal.
 *
 * 1. Looks up the handler by modifier name.
 * 2. For grammar-based handlers: builds/caches the grammar, tokenizes
 *    the content, parses it, and calls the handler with the parse tree.
 * 3. For buffer-based handlers: wraps content as a buffer and calls
 *    the handler directly.
 *
 * @param registry  Handler registry.
 * @param session   Active codegen session.
 * @param content   Raw embed content string.
 * @param modifier  Modifier name (handler key).
 * @return Opaque result (cast to `n00b_cg_val_t` by caller).
 */
extern n00b_embed_result_t
n00b_embed_dispatch(n00b_dict_untyped_t *registry,
                     n00b_cg_session_t   *session,
                     n00b_string_t       *content,
                     n00b_string_t       *modifier);
