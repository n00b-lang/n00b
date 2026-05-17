#pragma once

/**
 * @file n00b_attest_dsse.h
 * @brief DSSE envelope encode/decode surface.
 *
 * Declarations for the opaque `n00b_attest_envelope_t` and its
 * constructor / mutator / serializer / parser / accessor entry
 * points, plus the PAE (Pre-Authentication Encoding) byte-string
 * helper the signer (WP-002) will hash over.
 *
 * # DSSE envelope model
 *
 * A DSSE envelope wraps an in-toto Statement payload:
 * - `payloadType` — fixed at `application/vnd.in-toto+json`.
 * - `payload` — the (base64-encoded) Statement bytes.
 * - `signatures` — a JSON array; empty in WP-001 (signing
 *   arrives in WP-002 and only ever appends to this array).
 *
 * The PAE byte string follows the DSSE v1 spec:
 *
 *     DSSEv1 <payloadType-len> <payloadType> <payload-len> <payload>
 *
 * Joined with single spaces; lengths are decimal ASCII; the
 * payload bytes inside the PAE are the **unencoded** Statement
 * bytes (not the base64 form embedded in the JSON envelope).
 *
 * # Allocator discipline
 *
 * As with the Statement surface, every allocating entry point
 * accepts `.allocator = nullptr`. Threading an arena through
 * envelope construction keeps the envelope, its payload buffer,
 * and the produced PAE bytes all in that arena (FR-21 / FR-22).
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_statement.h>

/**
 * @brief Opaque DSSE envelope handle.
 *
 * Constructed by @ref n00b_attest_envelope_new with a payload
 * already in hand, then either serialized via @ref
 * n00b_attest_envelope_serialize or fetched back via @ref
 * n00b_attest_envelope_parse. The struct definition is private
 * to the module's `src/attest/` translation units.
 *
 * @note Per D-016 the envelope shape is deliberately
 *       algorithm-agnostic: the `signatures[]` JSON shape
 *       carries `keyid` and `sig` as opaque strings so an
 *       Ed25519 or ECDSA-P-256 signer (WP-002) can drop in
 *       without a header break.
 */
typedef struct n00b_attest_envelope n00b_attest_envelope_t;

/**
 * @brief Allocate a new, empty DSSE envelope handle.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the envelope and every byte
 *                it subsequently produces.
 *
 * @return A fresh envelope handle with `payloadType` set to
 *         `application/vnd.in-toto+json`, an empty
 *         `signatures[]` array, and no payload. Call @ref
 *         n00b_attest_envelope_set_payload before serializing.
 */
extern n00b_attest_envelope_t *
n00b_attest_envelope_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Attach the (unencoded) Statement payload to the
 *        envelope.
 *
 * @param env      The envelope to mutate.
 * @param payload  The Statement bytes, as produced by @ref
 *                 n00b_attest_statement_serialize (or any
 *                 conformant Statement v1 emitter). The
 *                 envelope **borrows** the buffer — it stores
 *                 the pointer, not a copy. The caller MUST keep
 *                 `payload`'s bytes valid (live, unfreed,
 *                 unmodified) until @ref
 *                 n00b_attest_envelope_serialize runs (DSSE PAE
 *                 signs over these bytes). The typical lifecycle
 *                 — serialize Statement, attach to envelope,
 *                 serialize envelope, drop both — meets this
 *                 contract naturally. Arena callers satisfy it
 *                 by allocating `payload` in the same arena as
 *                 the envelope.
 *
 *                 This is intentionally distinct from
 *                 @ref n00b_attest_statement_set_predicate_type
 *                 and @ref n00b_attest_statement_set_predicate_json,
 *                 which **copy** their inputs. The payload is
 *                 borrowed because it can be large (the full
 *                 serialized Statement) and is already owned by
 *                 the caller after
 *                 `n00b_attest_statement_serialize`; copying
 *                 would double the in-arena footprint for every
 *                 envelope.
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` if `payload` is null or
 *         empty.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new.
 */
extern n00b_result_t(bool)
n00b_attest_envelope_set_payload(n00b_attest_envelope_t *env,
                                 n00b_buffer_t          *payload);

/**
 * @brief Serialize the envelope to JSON bytes.
 *
 * @param env  The envelope to serialize.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the returned buffer.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, bytes)` on success
 *         with the JSON envelope (payload base64-encoded,
 *         `signatures` emitted as `[]` in WP-001);
 *         `n00b_result_err(...)` if the envelope has no payload
 *         set or the JSON encoder fails.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new and
 *      has had a payload attached.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_serialize(n00b_attest_envelope_t *env) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Parse a JSON envelope back into an envelope handle.
 *
 * @param bytes  The serialized envelope bytes.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the returned envelope and the
 *                base64-decoded payload buffer it exposes.
 *
 * @return `n00b_result_ok(n00b_attest_envelope_t *, env)` on
 *         success; `n00b_result_err(...)` if the bytes do not
 *         parse as a JSON object, if `payloadType` is missing
 *         or differs from the expected value, or if `payload`
 *         is malformed base64.
 */
extern n00b_result_t(n00b_attest_envelope_t *)
n00b_attest_envelope_parse(n00b_buffer_t *bytes) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Borrow the envelope's (base64-decoded) payload bytes.
 *
 * @param env  The envelope to query.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, payload)` on success
 *         with a buffer holding the unencoded Statement bytes;
 *         `n00b_result_err(...)` if the envelope has no payload
 *         attached.
 *
 * @post The returned buffer is owned by the envelope (or its
 *       allocator). Callers must not free it independently.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_get_payload(n00b_attest_envelope_t *env);

/**
 * @brief Produce the DSSE Pre-Authentication Encoding (PAE) byte
 *        string for the envelope's payload.
 *
 * The byte sequence is the canonical input the signer hashes
 * and signs (WP-002 work; this WP only emits the bytes):
 *
 *     DSSEv1 <payloadType-len> <payloadType> <payload-len> <payload>
 *
 * single-space-separated, lengths in decimal ASCII, payload
 * bytes uncoded (not the base64 form).
 *
 * @param env  The envelope to query.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the returned buffer.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, pae_bytes)` on
 *         success; `n00b_result_err(...)` if the envelope has
 *         no payload attached.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_pae_bytes(n00b_attest_envelope_t *env) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
