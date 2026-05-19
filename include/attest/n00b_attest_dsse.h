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
#include <attest/n00b_attest_signer.h>
#include <attest/n00b_attest_verifier.h>

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
 * @brief Return the number of `{keyid, sig}` entries currently
 *        attached to the envelope's `signatures[]` list.
 *
 * Allocation-free: a plain read off the envelope's internal
 * parallel-list shape. Used to drive a bounds-checked walk via
 * @ref n00b_attest_envelope_get_signature_keyid and
 * @ref n00b_attest_envelope_get_signature_sig — the canonical
 * inspect-parsed-envelope idiom (WP-003 Phase 1 closes the
 * parse-side gap from WP-002 where `signatures[]` round-tripped
 * on the wire but was not reconstructed on parse).
 *
 * @param env  The envelope to query.
 *
 * @return The number of attached `{keyid, sig}` pairs. Zero when
 *         the envelope has no signatures (either freshly
 *         constructed, parsed from JSON without a `signatures`
 *         field, or parsed from JSON with `"signatures": []`).
 *
 * @pre `env` is non-null and was returned by @ref
 *      n00b_attest_envelope_new or @ref
 *      n00b_attest_envelope_parse. A null `env` is a precondition
 *      violation, NOT a runtime error: this accessor is not
 *      wrapped in @ref n00b_result_t because — per the precedent
 *      set by @ref n00b_attest_signer_pubkey_spki_der — counts
 *      and aliased-pointer reads with no allocation use bare
 *      returns for ergonomics. The function therefore returns `0`
 *      on null `env`; callers MUST treat null inputs as a
 *      programming error rather than relying on the zero return.
 *
 * @note Allocation-free. The returned value reflects the
 *       envelope's state at call time; appending via
 *       @ref n00b_attest_envelope_add_signature after this call
 *       changes the count without notification.
 */
extern size_t
n00b_attest_envelope_signature_count(n00b_attest_envelope_t *env);

/**
 * @brief Borrow the keyid string of the `idx`-th entry of the
 *        envelope's `signatures[]` list.
 *
 * Bounds-checked alias-read. Returned pointer is the envelope's
 * own internal storage — see borrow-semantics note.
 *
 * @param env  The envelope to query.
 * @param idx  Zero-based index in `[0, signature_count)`.
 *
 * @return `n00b_result_ok(n00b_string_t *, keyid)` on success
 *         with the envelope's internal keyid string for entry
 *         `idx`. `n00b_result_err(n00b_string_t *,
 *         N00B_ATTEST_ERR_DSSE_BAD_INPUT)` if `env` is null or
 *         `idx` is out of range (i.e., `idx >=
 *         n00b_attest_envelope_signature_count(env)`).
 *
 * @note **Borrow semantics.** The returned pointer aliases the
 *       envelope's internal storage. Callers MUST NOT free it.
 *       The pointer remains valid for as long as the envelope
 *       itself remains live — concretely, until the envelope's
 *       owning allocator is released (e.g., the per-call arena
 *       used by @ref n00b_attest_envelope_parse or
 *       @ref n00b_attest_envelope_new). The envelope module does
 *       not currently expose a per-envelope free / release; the
 *       aliased lifetime is governed by the allocator passed at
 *       construction or parse time.
 *
 * @note No `_kargs`, no allocator threading: this is a pointer
 *       alias, not an allocation. If a caller needs an
 *       independently-owned copy they should clone the bytes via
 *       the string API of their choosing.
 *
 * @note The keyid bytes are preserved verbatim from the parsed
 *       wire JSON (or from the @ref
 *       n00b_attest_envelope_add_signature private-copy). Per
 *       D-039 the canonical keyid is lowercase hex of
 *       `SHA-256(SPKI DER)`, but the envelope code does NOT
 *       validate that shape; callers that depend on hex form
 *       must verify themselves.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new or
 *      @ref n00b_attest_envelope_parse and has not yet been
 *      released. A null `env` surfaces as
 *      `n00b_result_err(..., N00B_ATTEST_ERR_DSSE_BAD_INPUT)`
 *      rather than UB, but callers MUST treat null inputs as a
 *      programming error.
 * @pre `idx` is in the range
 *      `[0, n00b_attest_envelope_signature_count(env))`.
 *      Out-of-range indices return
 *      `n00b_result_err(..., N00B_ATTEST_ERR_DSSE_BAD_INPUT)`.
 */
extern n00b_result_t(n00b_string_t *)
n00b_attest_envelope_get_signature_keyid(n00b_attest_envelope_t *env,
                                         size_t                  idx);

/**
 * @brief Borrow the (base64-decoded) signature bytes of the
 *        `idx`-th entry of the envelope's `signatures[]` list.
 *
 * Bounds-checked alias-read. Returned pointer is the envelope's
 * own internal storage — see borrow-semantics note.
 *
 * @param env  The envelope to query.
 * @param idx  Zero-based index in `[0, signature_count)`.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, sig)` on success with
 *         the envelope's internal buffer holding the **decoded**
 *         (raw, not base64) signature bytes for entry `idx`. For
 *         Ed25519 this is exactly 64 bytes. `n00b_result_err(
 *         n00b_buffer_t *, N00B_ATTEST_ERR_DSSE_BAD_INPUT)` if
 *         `env` is null or `idx` is out of range.
 *
 * @note **Borrow semantics.** The returned pointer aliases the
 *       envelope's internal storage. Callers MUST NOT free it.
 *       Lifetime is the same as the envelope itself — see the
 *       borrow-semantics note on @ref
 *       n00b_attest_envelope_get_signature_keyid.
 *
 * @note No `_kargs`, no allocator threading: pointer alias only.
 *
 * @note On the parse path the bytes are obtained by
 *       base64-decoding the wire `sig` field; on the build path
 *       (via @ref n00b_attest_envelope_add_signature) the bytes
 *       are exactly what the caller appended. Round-trip
 *       byte-equality is the contract — what goes in via
 *       add-signature serializes and re-parses back to the same
 *       bytes (modulo wire base64 encoding).
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new or
 *      @ref n00b_attest_envelope_parse and has not yet been
 *      released. A null `env` surfaces as
 *      `n00b_result_err(..., N00B_ATTEST_ERR_DSSE_BAD_INPUT)`
 *      rather than UB, but callers MUST treat null inputs as a
 *      programming error.
 * @pre `idx` is in the range
 *      `[0, n00b_attest_envelope_signature_count(env))`.
 *      Out-of-range indices return
 *      `n00b_result_err(..., N00B_ATTEST_ERR_DSSE_BAD_INPUT)`.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_get_signature_sig(n00b_attest_envelope_t *env,
                                       size_t                  idx);

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

/**
 * @brief Append one `{keyid, sig}` entry to the envelope's
 *        `signatures[]` list.
 *
 * Low-level signature-population entry point. The caller supplies
 * a pre-computed signature (typically obtained by calling
 * @ref n00b_attest_signer_sign on this envelope's PAE bytes); the
 * envelope stores the keyid + signature bytes opaquely on its
 * internal list and emits them in the rendered JSON at
 * @ref n00b_attest_envelope_serialize time. The shape is
 * deliberately algorithm-agnostic (D-016): the per-entry layout
 * is `{ "keyid": "<hex>", "sig": "<base64>" }` and the same shape
 * carries Ed25519, ECDSA P-256, or any later algorithm without a
 * header break.
 *
 * @param env    The envelope to mutate.
 * @param keyid  Opaque caller-defined key identifier (typically a
 *               hex-encoded SHA-256 of the 44-byte
 *               SubjectPublicKeyInfo DER per D-039 — the cosign /
 *               sigstore convention; supersedes D-033 OQ-3's
 *               raw-pubkey form). For the file backend this is the
 *               value returned by @ref n00b_attest_signer_keyid.
 *               The envelope stores a private copy; the caller may
 *               free `keyid` immediately after this call.
 * @param sig    The signature bytes (64 bytes for Ed25519). The
 *               envelope stores a private copy; the caller may
 *               free `sig` immediately after this call.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the private copies the envelope
 *                stores.
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` if `env`, `keyid`, or
 *         `sig` is null, or if either input is empty.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new and
 *      has had a payload attached.
 */
extern n00b_result_t(bool)
n00b_attest_envelope_add_signature(n00b_attest_envelope_t *env,
                                   n00b_string_t          *keyid,
                                   n00b_buffer_t          *sig) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Sign the envelope with a resolved signer and append the
 *        resulting signature to its `signatures[]` list.
 *
 * High-level signature-population entry point. Computes the PAE
 * bytes via @ref n00b_attest_envelope_pae_bytes, calls
 * @ref n00b_attest_signer_sign on those bytes, fetches the
 * signer's pre-computed keyid via @ref n00b_attest_signer_keyid
 * (SHA-256 of the 44-byte SPKI DER, hex-encoded per D-039 — the
 * cosign/sigstore-convention form; supersedes the prior D-033
 * OQ-3 "SHA-256 of raw pubkey" form), and dispatches the
 * `{keyid, sig}` pair through
 * @ref n00b_attest_envelope_add_signature. Implemented in terms
 * of the low-level entry point so multi-signer envelopes (FR-11,
 * deferred to a later WP) can populate by repeatedly calling the
 * low-level surface against pre-computed signatures.
 *
 * @param env     The envelope to sign.
 * @param signer  The signer handle.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns every byte produced (PAE bytes,
 *                signature bytes, keyid string, the envelope's
 *                internal copies).
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` if `env` or `signer` is
 *         null, if PAE-byte construction fails, or if the
 *         signer's sign vtable returns an error (propagated via
 *         the same error-code namespace as
 *         @ref n00b_attest_signer_sign).
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new and
 *      has had a payload attached; `signer` was returned by
 *      @ref n00b_attest_signer_resolve and has not been released.
 */
extern n00b_result_t(bool)
n00b_attest_envelope_sign(n00b_attest_envelope_t *env,
                          n00b_attest_signer_t   *signer) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Verify the signature at index `idx` of the envelope's
 *        `signatures[]` against the supplied verifier.
 *
 * Low-level signature-verification entry point — the dual of
 * @ref n00b_attest_envelope_add_signature on the verify side.
 * Re-derives the envelope's PAE bytes, fetches the entry's
 * `{keyid, sig}` pair, and calls @ref n00b_attest_verifier_check
 * against the PAE bytes. **Does NOT check keyid match against
 * the verifier's keyid** — single-entry verification is "verify
 * THIS index, no policy"; keyid-match policy lives in the
 * high-level @ref n00b_attest_envelope_verify wrapper.
 *
 * @param env       The envelope holding the signature at `idx`.
 * @param idx       Zero-based index into `signatures[]`. Must be
 *                  in the range `[0,
 *                  n00b_attest_envelope_signature_count(env))`.
 * @param verifier  The verifier handle to check against (typically
 *                  obtained by @ref n00b_attest_verifier_resolve).
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); threaded through PAE-byte derivation
 *                and any scratch the @ref n00b_attest_verifier_check
 *                dispatch produces.
 *
 * @return Verdict-encoding `n00b_result_t(bool)` propagated
 *         verbatim from @ref n00b_attest_verifier_check:
 *         - `n00b_result_ok(bool, true)` — the signature at `idx`
 *           verifies under `verifier`'s public key for the
 *           envelope's current PAE bytes.
 *         - `n00b_result_ok(bool, false)` — the signature at `idx`
 *           does NOT verify (verdict). Common causes: the
 *           signature was over different bytes (tampered payload)
 *           or under a different key. **Callers MUST NOT collapse
 *           this into `Err`** — Phase 4's 3-code exit shape
 *           depends on the `Ok(true)`/`Ok(false)` split.
 *         - `n00b_result_err(bool,
 *           N00B_ATTEST_ERR_DSSE_BAD_INPUT)` — `env` or `verifier`
 *           is null, or `idx` is out of range.
 *         - `n00b_result_err(bool, ...)` — any other machinery
 *           failure from PAE-byte construction or
 *           @ref n00b_attest_verifier_check (e.g.,
 *           @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH,
 *           @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT,
 *           @ref N00B_ATTEST_ERR_DSSE_NO_PAYLOAD).
 *
 * @details
 *
 * **PAE-derivation cost.** This wrapper re-derives the envelope's
 * PAE bytes on every call (the entry point is self-contained).
 * Callers verifying multiple signatures on the same envelope
 * SHOULD use @ref n00b_attest_envelope_verify, which derives the
 * PAE bytes once and walks `signatures[]` with the cached
 * derivation.
 *
 * **No keyid-match.** The low-level wrapper does not compare the
 * entry's `keyid` field to the verifier's keyid; it simply runs
 * the crypto check. A caller that wants the cosign / sigstore
 * any-signature-passes shape (skip entries whose keyid doesn't
 * match the verifier; succeed if ANY matching entry verifies)
 * should use the high-level @ref n00b_attest_envelope_verify
 * wrapper.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new or
 *      @ref n00b_attest_envelope_parse and has a payload attached.
 * @pre `verifier` was returned by
 *      @ref n00b_attest_verifier_resolve and has not been
 *      released.
 */
extern n00b_result_t(bool)
n00b_attest_envelope_verify_signature(n00b_attest_envelope_t *env,
                                      size_t                  idx,
                                      n00b_attest_verifier_t *verifier) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Verify the envelope against the supplied verifier using
 *        sigstore-style "any matching signature passes" semantics.
 *
 * High-level signature-verification entry point — the dual of
 * @ref n00b_attest_envelope_sign on the verify side. Walks the
 * envelope's `signatures[]` and answers a single verdict for the
 * whole envelope: `Ok(true)` if any entry whose `keyid` matches
 * the verifier's keyid verifies under the verifier's public key;
 * otherwise `Ok(false)`.
 *
 * Per D-044 Q3 (sigstore-style any-passes) the wrapper:
 *   1. Derives the envelope's PAE bytes ONCE at the top.
 *   2. Fetches the verifier's keyid ONCE at the top.
 *   3. Iterates each entry; for an entry whose `keyid` equals
 *      the verifier's keyid (byte-equal string match per D-039),
 *      calls @ref n00b_attest_verifier_check against the cached
 *      PAE bytes.
 *      - `Ok(true)` short-circuits the walk and returns `Ok(true)`.
 *      - `Ok(false)` continues to the next entry.
 *      - `Err(...)` propagates immediately (machinery failure
 *         aborts the walk).
 *   4. Entries whose `keyid` does NOT match the verifier's keyid
 *      are **skipped silently** — the function neither calls
 *      `_verifier_check` against them nor emits a diagnostic.
 *      (Per Known Deferral 1 in WP-003 Phase 3, a diagnostic
 *      surface for non-matching-keyid entries is deferred to a
 *      future verifier-result-type lift.)
 *   5. If the walk completes without any entry returning
 *      `Ok(true)`, the wrapper returns `Ok(false)`. An envelope
 *      with no signatures at all (empty `signatures[]`, per
 *      architecture §9 `FAIL_NO_SIGNATURES`) returns `Ok(false)`
 *      via the same path.
 *
 * @param env       The envelope to verify.
 * @param verifier  The verifier handle to check against.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns the PAE-byte derivation and any
 *                scratch the per-entry @ref
 *                n00b_attest_verifier_check dispatch produces.
 *
 * @return Verdict-encoding `n00b_result_t(bool)`:
 *         - `n00b_result_ok(bool, true)` — at least one entry
 *           whose `keyid` matched the verifier's keyid verified
 *           under the verifier's public key for the envelope's
 *           PAE bytes.
 *         - `n00b_result_ok(bool, false)` — no matching-keyid
 *           entry verified (either no entry has a matching
 *           keyid, or every matching-keyid entry returned
 *           `Ok(false)`, or `signatures[]` is empty). **Callers
 *           MUST NOT collapse this into `Err`** — Phase 4's
 *           3-code exit shape depends on the
 *           `Ok(true)`/`Ok(false)` split.
 *         - `n00b_result_err(bool,
 *           N00B_ATTEST_ERR_DSSE_BAD_INPUT)` — `env` or `verifier`
 *           is null.
 *         - `n00b_result_err(bool, ...)` — any other machinery
 *           failure from PAE-byte construction or
 *           @ref n00b_attest_verifier_check; the walk aborts on
 *           the first such failure (i.e., later matching-keyid
 *           entries are NOT attempted).
 *
 * @details
 *
 * **Canonicalization is sign-side only (D-043).** The verify
 * path consumes the envelope's wire bytes verbatim through
 * @ref n00b_attest_envelope_pae_bytes; the wrapper performs no
 * canonicalization. A caller verifying a parsed envelope (where
 * the bytes came off disk as JSON) gets the same PAE bytes that
 * went on disk; the verify wrapper does not re-canonicalize.
 *
 * **Keyid-match policy (D-039 + sigstore alignment).** The
 * keyid match is a byte-equal string comparison: the wrapper
 * does NOT hex-decode, does NOT case-fold, does NOT length-
 * validate either keyid. Both sides are produced by the same
 * D-039 derivation (`lowercase-hex(SHA-256(SPKI DER))`) — the
 * canonical 64-char lowercase-hex form — so byte-equality is the
 * correct policy. Callers passing a verifier whose keyid was
 * mutated (e.g., uppercased) will see ALL matching entries skip
 * silently and the wrapper return `Ok(false)`.
 *
 * **PAE-derivation cost (vs. low-level wrapper).** Unlike
 * @ref n00b_attest_envelope_verify_signature (which re-derives
 * PAE bytes on every call so that single-entry callers don't
 * need to pre-derive), this wrapper derives PAE ONCE at the top
 * and re-uses the buffer for every matching-keyid entry in the
 * walk. For an N-signature envelope this is the difference
 * between O(N) and O(1) PAE allocations.
 *
 * @pre `env` was returned by @ref n00b_attest_envelope_new or
 *      @ref n00b_attest_envelope_parse and has a payload attached.
 * @pre `verifier` was returned by
 *      @ref n00b_attest_verifier_resolve and has not been
 *      released.
 */
extern n00b_result_t(bool)
n00b_attest_envelope_verify(n00b_attest_envelope_t *env,
                            n00b_attest_verifier_t *verifier) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
