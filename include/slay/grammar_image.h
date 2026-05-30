#pragma once

/**
 * @file grammar_image.h
 * @brief Build-time grammar baking + runtime materialization of a
 *        pre-compiled `n00b_grammar_t`.
 *
 * WP-018 (naudit static-image grammar caching), revised by WP-020. The runtime
 * cost of parsing a `.bnf` file via the BNF metagrammar (PWZ) dominates small
 * single-file naudit invocations. This module lets the grammar be *baked* at
 * build time into emitted C source that, at runtime, unmarshals an identical
 * `n00b_grammar_t` from a static marshal blob — skipping the metagrammar parse
 * entirely.
 *
 * The emitter (`n00b_grammar_image_emit`) marshals a finalized
 * `n00b_grammar_t` and writes C source containing a base64-encoded blob plus
 * a lazy materializer that decodes and calls `n00b_unmarshal_one`. It is used
 * by the build-time bake tool (`naudit-grammar-bake`) and by the
 * `container_kind grammar` path of `n00b-static-init-helper`. Runtime
 * materializers call `n00b_grammar_image_repair()` immediately after unmarshal
 * so process-local function pointers are rebound in the current executable.
 *
 * The marshal blob captures the finalized object graph exactly. That preserves
 * private fields and error-recovery markers that the old hand emitter could
 * miss.
 */

#include "slay/grammar.h"
#include "core/alloc.h"
#include "adt/result.h"

/**
 * @brief Repair process-local callbacks after unmarshaling a grammar image.
 *
 * Marshal blobs preserve pointer values exactly. That is correct for ordinary
 * heap/static objects, but not for function pointers captured by dictionary
 * hash callbacks or grammar action hooks: the bake executable and runtime
 * executable have different code addresses. Generated image materializers call
 * this after `n00b_unmarshal_one()` to rebind known dictionary hash callbacks
 * and clear optional grammar callbacks that cannot be portably serialized.
 *
 * @param g  Unmarshaled grammar, or null.
 */
extern void n00b_grammar_image_repair(n00b_grammar_t *g);

// ============================================================================
// Emitter (build-time)
// ============================================================================

/**
 * @brief Error codes for `n00b_grammar_image_emit`.
 *
 * Negative to avoid collision with `errno` (n00b-api-guidelines § 5.1).
 */
typedef enum {
    N00B_GRAMMAR_IMAGE_OK            = 0,
    N00B_GRAMMAR_IMAGE_ERR_NULL_ARG  = -1, ///< A required argument was null.
    N00B_GRAMMAR_IMAGE_ERR_NOT_FINAL = -2, ///< @p g was not finalized.
    N00B_GRAMMAR_IMAGE_ERR_MARSHAL   = -3, ///< @p g could not be marshaled.
    N00B_GRAMMAR_IMAGE_ERR_ENCODE    = -4, ///< Marshal bytes could not be encoded.
} n00b_grammar_image_err_t;

/**
 * @brief Human-readable description for a `n00b_grammar_image_emit` error.
 *
 * @param err  An `n00b_grammar_image_err_t` value (passed as the generic
 *             `n00b_err_t` carried by `n00b_result_t`).
 * @return A static description string (never null).
 */
extern n00b_string_t *n00b_grammar_image_emit_err_str(n00b_err_t err);

/**
 * @brief Emit C source materializing @p g.
 *
 * Marshals the finalized grammar @p g and returns C source declaring a
 * `n00b_grammar_t *<symbol_prefix>_build(void)` function plus a
 * `[[gnu::constructor]]` that registers a lazy materializer under
 * @p grammar_name with `n00b_static_grammar_register`. The emitted code decodes
 * a static base64 blob and calls `n00b_unmarshal_one`. It uses only
 * `[[gnu::...]]` attribute spellings (never bare `__attribute__((...))`) per
 * n00b-api-guidelines § 2.5.
 *
 * @param g              Finalized grammar to bake.
 * @param symbol_prefix  C identifier prefix for emitted symbols.
 * @param grammar_name   Lookup name registered with the runtime.
 * @kw allocator         Optional allocator (nullptr = runtime default).
 *                       Forwarded to the internal `n00b_list_new_private`
 *                       parts accumulator and the `n00b_string_from_cstr`
 *                       fragments this emitter allocates. (`n00b_cformat`
 *                       uses checked variadics and has no `.allocator`
 *                       kwarg, so its allocations use the runtime
 *                       default; this is a slay/string-API limit, not an
 *                       omission.)
 * @return `n00b_result_ok` wrapping the emitted C source as a
 *         freshly-allocated n00b string, or `n00b_result_err` with a
 *         `n00b_grammar_image_err_t` code on failure (e.g. @p g not
 *         finalized or a required argument is null).
 */
extern n00b_result_t(n00b_string_t *)
n00b_grammar_image_emit(n00b_grammar_t *g,
                        n00b_string_t  *symbol_prefix,
                        n00b_string_t  *grammar_name)
    _kargs { n00b_allocator_t *allocator = nullptr; };
