/**
 * @file static_image.h
 * @brief Build-time static-image contract helpers.
 */
#pragma once

#include "n00b.h"
#include "core/type_info.h"

// Forward declaration: the static-grammar API (WP-018) traffics only in
// `n00b_grammar_t *`, so the full slay grammar header is not pulled in
// here (it is not part of the `n00b.h` umbrella).
typedef struct n00b_grammar_t n00b_grammar_t;

typedef enum n00b_static_image_status_t : uint8_t {
    N00B_STATIC_IMAGE_OK = 0,
    N00B_STATIC_IMAGE_ERR_NULL_REQUEST,
    N00B_STATIC_IMAGE_ERR_VERSION,
    N00B_STATIC_IMAGE_ERR_ABI,
    N00B_STATIC_IMAGE_ERR_PAYLOAD,
    N00B_STATIC_IMAGE_ERR_ARGUMENT,
    N00B_STATIC_IMAGE_ERR_UNREGISTERED_TYPE,
    N00B_STATIC_IMAGE_ERR_UNSUPPORTED_POLICY,
    N00B_STATIC_IMAGE_ERR_SCAN_KIND,
    N00B_STATIC_IMAGE_ERR_NO_INITIALIZER,
    N00B_STATIC_IMAGE_ERR_INITIALIZER,
} n00b_static_image_status_t;

/**
 * @brief Accumulator passed to a registered static initializer.
 *
 * `expr` and `decls` are written by the initializer; `error` is set by
 * `n00b_static_image_builder_fail()` (or by the dispatch path) when
 * `status` is non-OK.  All three are n00b strings — the public surface
 * does not expose raw `char *` buffers.
 */
typedef struct n00b_static_image_builder_t {
    const n00b_static_image_request_t *request;
    n00b_string_t                     *expr;
    n00b_string_t                     *decls;
    n00b_string_t                     *error;
    n00b_static_image_status_t         status;
} n00b_static_image_builder_t;

typedef n00b_static_image_status_t (*n00b_static_initializer_fn)(
    n00b_static_image_builder_t *);

extern n00b_string_t *
n00b_static_image_status_name(n00b_static_image_status_t status);

extern bool
n00b_static_image_abi_matches_host(const n00b_static_image_abi_t *abi);

extern n00b_static_image_status_t
n00b_static_image_validate_request(const n00b_static_image_request_t *request);

extern void
n00b_static_image_builder_init(n00b_static_image_builder_t *builder,
                               const n00b_static_image_request_t *request);

extern void
n00b_static_image_builder_destroy(n00b_static_image_builder_t *builder);

/**
 * @brief Set the builder's `expr` field to a caller-formatted string.
 *
 * The caller is expected to pre-format with `n00b_cformat()` /
 * `n00b_format()` if substitution is needed; this entry point takes
 * the finished n00b string and stores it.
 */
extern void
n00b_static_image_builder_set_expr(n00b_static_image_builder_t *builder,
                                   n00b_string_t *expr);

/**
 * @brief Append `chunk` to the builder's `decls` field.
 *
 * Concatenates `chunk` onto whatever the initializer has accumulated
 * so far.  Pre-format via `n00b_cformat()` / `n00b_format()` if you
 * need substitution.
 */
extern void
n00b_static_image_builder_append(n00b_static_image_builder_t *builder,
                                 n00b_string_t *chunk);

/**
 * @brief Record a static-image build failure.
 *
 * Stores @p msg on the builder, marks the builder with @p status, and
 * returns @p status so callers can `return` the call directly.
 */
extern n00b_static_image_status_t
n00b_static_image_builder_fail(n00b_static_image_builder_t *builder,
                               n00b_static_image_status_t status,
                               n00b_string_t *msg);

extern n00b_static_image_status_t
n00b_static_image_build(const n00b_static_image_request_t *request,
                        n00b_static_image_builder_t *builder);

// ============================================================================
// Static grammar images (WP-018)
// ============================================================================

/**
 * @brief Builder for a baked grammar image.
 *
 * Emitted grammar-image C source (see `slay/grammar_image.h`) defines a
 * function of this shape that reconstructs the grammar by replaying the
 * `n00b_grammar_image_*` primitives. It is registered — not invoked — at
 * static-constructor time; the grammar is materialized lazily on the
 * first `n00b_static_grammar_lookup`, after the n00b runtime is up.
 */
typedef n00b_grammar_t *(*n00b_static_grammar_builder_fn)(void);

/**
 * @brief Register a baked grammar image under @p name.
 *
 * Records the (name, builder) pair so a later
 * `n00b_static_grammar_lookup(name)` can materialize the grammar. Safe
 * to call from a `[[gnu::constructor]]`: it only stores the pair in a
 * fixed-capacity table and does not allocate via the n00b runtime (which
 * is not yet initialized at constructor time). A second registration of
 * the same @p name replaces the prior builder.
 *
 * @note Invoked from a `[[gnu::constructor]]` in the emitted
 *       grammar-image source, BEFORE `n00b_init`. The emitter passes
 *       @p name as an r-string (`r"..."`) — a static `n00b_string_t`
 *       emitted by ncc and fully available pre-runtime — so this takes
 *       `n00b_string_t *` directly. No `const char *` C-ABI boundary is
 *       needed; § 2.2 is satisfied. The matching reader
 *       (`n00b_static_grammar_lookup`) compares via `n00b_unicode_str_eq`.
 *
 * @param name     Lookup name (an r-string `n00b_string_t *`).
 * @param builder  Function that reconstructs and finalizes the grammar.
 */
extern void
n00b_static_grammar_register(n00b_string_t                  *name,
                             n00b_static_grammar_builder_fn  builder);

/**
 * @brief Look up (and lazily materialize) a baked grammar by name.
 *
 * On the first lookup for a given name the registered builder runs and
 * its result is cached; subsequent lookups return the same pointer. The
 * materialized grammar lives on the GC heap, exactly like a
 * runtime-parsed grammar (WP-018 DF-ED).
 *
 * @param name  The name passed to `n00b_static_grammar_register`
 *              (`n00b_string_t *`; matched via `n00b_unicode_str_eq`
 *              against the registered r-string name).
 * @return `n00b_option_set` wrapping the materialized grammar when
 *         @p name is registered, or `n00b_option_none` when @p name is
 *         unknown (a normal, non-error outcome — the static image is a
 *         fast path, not a hard dependency).
 */
extern n00b_option_t(n00b_grammar_t *)
n00b_static_grammar_lookup(n00b_string_t *name);
