#pragma once

/**
 * @file n00b_attest_statement.h
 * @brief in-toto Statement v1 builder/parser surface.
 *
 * Declarations for the opaque `n00b_attest_statement_t` builder
 * and its constructor / mutator / serializer / parser entry
 * points. Bodies arrive in WP-001 Phase 2; Phase 1 lays the
 * surface down so callers (and the DSSE envelope code) can be
 * written against stable signatures.
 *
 * # Statement v1 model
 *
 * An in-toto Statement v1 document carries:
 * - `_type` — fixed at `https://in-toto.io/Statement/v1`.
 * - `subject` — a list of `{ name, digest: { sha256: <hex> } }`
 *   entries naming the artifact(s) the statement refers to.
 * - `predicateType` — a URI identifying the predicate schema
 *   (caller-supplied, FR-3 / FR-5).
 * - `predicate` — an opaque caller-supplied JSON blob; the
 *   builder does not validate against any predicate schema.
 *
 * # Serialization shape
 *
 * The serializer uses libn00b's `n00b_json_encode(.pretty = false)`
 * directly (D-024). Output is compact JSON, byte-stable per
 * construction order; callers that need cross-producer byte
 * identity should not rely on it (RFC 8785 JCS is not applied).
 *
 * # Allocator discipline
 *
 * Every allocating entry point on this surface accepts an
 * `.allocator` kwarg defaulting to `nullptr` (use the runtime
 * default). Threading an arena into `n00b_attest_statement_new`
 * makes every byte that statement and its derived serialization
 * produce live in that arena (FR-21 / FR-22, per architecture
 * §4).
 */

#include <n00b.h>
#include "adt/result.h"

/**
 * @brief Opaque builder/parser handle for an in-toto Statement v1.
 *
 * Constructed by @ref n00b_attest_statement_new, mutated via
 * the `_add_subject` / `_set_predicate_*` family, then either
 * serialized via @ref n00b_attest_statement_serialize or
 * obtained back from a serialized form via @ref
 * n00b_attest_statement_parse. The struct definition is private
 * to the module's `src/attest/` translation units.
 */
typedef struct n00b_attest_statement n00b_attest_statement_t;

/**
 * @brief Allocate a new, empty Statement builder.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator). When set, every internal allocation
 *                this builder and its serializer perform threads
 *                through this allocator (FR-21).
 *
 * @return A fresh Statement handle. The `_type` field is
 *         pre-populated; `subject`, `predicateType`, and
 *         `predicate` are unset and must be supplied by the
 *         caller before serialization.
 *
 * @post The returned handle owns no caller-visible state until
 *       a mutator is called; it is safe to discard without an
 *       explicit free (the allocator owns its lifetime).
 */
extern n00b_attest_statement_t *
n00b_attest_statement_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Append a `{name, digest}` entry to the Statement's
 *        `subject` list.
 *
 * @param st  The Statement builder to mutate.
 *
 * @kw name       Subject name (required at serialization time;
 *                may be left null here only so the kwarg syntax
 *                accepts no-name calls in tests, which fail at
 *                run time).
 * @kw digest     Raw digest bytes (32-byte sha256 in v1; other
 *                algorithms are accepted by adding entries to
 *                the digest map but only sha256 is exercised
 *                this WP).
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator). Threaded through to the underlying
 *                list growth and string copies.
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` with a module-domain
 *         error code on a missing required field or an
 *         out-of-memory condition.
 *
 * @pre `st` was returned by @ref n00b_attest_statement_new.
 */
extern n00b_result_t(bool)
n00b_attest_statement_add_subject(n00b_attest_statement_t *st) _kargs
{
    n00b_string_t    *name      = nullptr;
    n00b_buffer_t    *digest    = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Set the Statement's `predicateType` URI.
 *
 * @param st        The Statement builder to mutate.
 * @param type_uri  Predicate-type URI (e.g.
 *                  `https://slsa.dev/provenance/v1`). FR-3 names
 *                  the SLSA URI; FR-5 admits arbitrary
 *                  caller-supplied URIs. The builder does not
 *                  validate the URI against any registry. The
 *                  setter retains a private copy; the caller may
 *                  free `type_uri` immediately after this call.
 *
 * @kw allocator   Optional allocator for the private copy
 *                 (defaults to the runtime allocator).
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` if `type_uri` is null or
 *         empty.
 *
 * @pre `st` was returned by @ref n00b_attest_statement_new.
 */
extern n00b_result_t(bool)
n00b_attest_statement_set_predicate_type(n00b_attest_statement_t *st,
                                         n00b_string_t           *type_uri)
_kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Attach the opaque predicate JSON blob.
 *
 * @param st              The Statement builder to mutate.
 * @param predicate_json  The caller-supplied predicate, already
 *                        serialized as JSON. The setter retains
 *                        a private copy of the bytes; the caller
 *                        may free `predicate_json` immediately
 *                        after this call. At serialization time
 *                        the builder re-parses and re-serializes
 *                        the stored copy through libn00b's JSON
 *                        primitives so the embedded form matches
 *                        the rest of the Statement's
 *                        canonicalization.
 *
 * @kw allocator   Optional allocator for the private copy
 *                 (defaults to the runtime allocator).
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` if the blob is null,
 *         empty, or not a parseable JSON value.
 *
 * @pre `st` was returned by @ref n00b_attest_statement_new.
 */
extern n00b_result_t(bool)
n00b_attest_statement_set_predicate_json(n00b_attest_statement_t *st,
                                         n00b_buffer_t           *predicate_json)
_kargs {
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Serialize the Statement to canonical (compact) JSON
 *        bytes.
 *
 * @param st  The Statement builder to serialize.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the returned buffer.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, bytes)` on success;
 *         `n00b_result_err(n00b_buffer_t *, ...)` if a required
 *         field is missing (`subject` empty, `predicateType`
 *         unset, `predicate` unset) or libn00b's JSON encoder
 *         fails.
 *
 * @pre `st` was returned by @ref n00b_attest_statement_new and
 *      has had at least one subject added plus a predicate type
 *      and predicate blob set.
 * @post The returned buffer is byte-stable for the same
 *       construction order on the same builder.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_statement_serialize(n00b_attest_statement_t *st) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Parse a canonical-JSON Statement blob back into a
 *        builder handle.
 *
 * @param bytes  The serialized Statement bytes (as produced by
 *               @ref n00b_attest_statement_serialize or by any
 *               conformant in-toto Statement v1 emitter).
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the returned builder and its
 *                fields.
 *
 * @return `n00b_result_ok(n00b_attest_statement_t *, st)` on
 *         success; `n00b_result_err(...)` if the bytes do not
 *         parse as a JSON object, if `_type` is missing or
 *         differs from the expected URI, or if a required
 *         sub-field is malformed.
 */
extern n00b_result_t(n00b_attest_statement_t *)
n00b_attest_statement_parse(n00b_buffer_t *bytes) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
