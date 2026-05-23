#pragma once

/**
 * @file n00b_attest_verifier.h
 * @brief Verifier abstraction public surface.
 *
 * Declarations for the opaque `n00b_attest_verifier_t` and its
 * resolver / check / pubkey / keyid / release entry points. The
 * verifier surface is the dual of the signer surface: the signer
 * loads a private key by URI and produces a signature over arbitrary
 * bytes; the verifier loads a public key by URI and answers the
 * pass/fail question for a signature over arbitrary bytes. Backends
 * differ behind the resolver — each one implements the package-
 * private vtable in `internal/attest/verifier_backends.h` and is
 * dispatched by URI scheme.
 *
 * # Algorithm-agnostic by construction (D-016)
 *
 * The public surface deliberately bakes in no algorithm tags.
 * WP-003 ships only the Ed25519 file backend; a later WP adds
 * ECDSA P-256 (and others) by registering additional backends
 * behind the same surface. There are no `n00b_attest_verifier_ed25519_*`
 * symbols — callers reach for `n00b_attest_verifier_*` exclusively
 * and the backend identifies the algorithm from the loaded SPKI
 * AlgorithmIdentifier OID.
 *
 * # URI dispatch (FR-VM-1)
 *
 * @ref n00b_attest_verifier_resolve takes a `ref` URI such as
 * `file:///etc/n00b-attest/keys/ci.pub.pem` and dispatches to the
 * matching backend. WP-003 ships only the `file://` backend;
 * unknown schemes return @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME.
 * Per D-035 OQ-4 the file backend accepts both `file:<path>`
 * (FR-SM-1 strict, mirrored on the verifier side) and
 * `file:///<path>` (RFC 3986) forms.
 *
 * # Lifetime (architecture §6.2)
 *
 * A verifier handle is reusable for the duration of a verify
 * session: resolve once, check many, release once. The library
 * does NOT maintain a process-wide cache — callers own the
 * lifetime explicitly. @ref n00b_attest_verifier_release does
 * NOT `crypto_wipe` any held bytes because public-key bytes are
 * not secret; the entry point exists for caller-uniform lifetime
 * management symmetric with @ref n00b_attest_signer_release.
 *
 * # Allocator discipline
 *
 * Every allocating entry point on this surface carries
 * `.allocator = nullptr`. Threading an arena through
 * @ref n00b_attest_verifier_resolve makes every byte that resolve
 * (and the subsequent check / pubkey / keyid / release operations)
 * produce live in that arena (FR-21 / FR-22).
 *
 * # Optional-pointer kwargs (D-035 part 2)
 *
 * Optional pointer kwargs on this surface use `T * = nullptr`
 * rather than `n00b_option_t(T) = n00b_option_none(T)`, matching
 * the existing WP-001 module surface (Statement / DSSE) and the
 * signer surface. This is a project-local exception to the
 * cross-project canonical `n00b_option_t` shape; cross-project
 * normalization is a later cleanup WP.
 *
 * # Verdict semantics: `Ok(false)` is NOT `Err` (D-044 OQ-1)
 *
 * @ref n00b_attest_verifier_check is unusual on the project's
 * surface in that the function's primary semantic split is
 * encoded between `Ok(true)` and `Ok(false)`, not between `Ok`
 * and `Err`. A signature that "did not verify" is a verdict
 * (the caller's signature-policy code wants to surface as a
 * non-zero exit), not a machinery failure. Machinery failures
 * (null input, sig buffer not exactly 64 bytes, allocator OOM,
 * etc.) route through `Err`. Phase 4's 3-code exit shape (exit
 * 0 = `Ok(true)`, exit 1 = `Ok(false)`, exit 2 = `Err`) depends
 * on this distinction; callers MUST NOT collapse `Ok(false)`
 * into `Err`.
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_error.h>

/*
 * The 5 verifier-domain error codes (-5001 … -5005) live in
 * `n00b_attest_error.h` per the WP-003 Phase 2 cleanup pass
 * (D-046 — supersedes D-044 OQ-2's Phase 3 phase assignment).
 * This header references them via @ref tags only:
 *
 *   - @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME    (-5001)
 *   - @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND         (-5002)
 *   - @ref N00B_ATTEST_ERR_VERIFIER_PEM_PARSE_FAILED      (-5003)
 *   - @ref N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED      (-5004)
 *   - @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM (-5005)
 *
 * WP-003 Phase 3 (D-047 W-1) adds the runtime-check-path codes
 * under the `_VERIFY_*` prefix (the `_VERIFIER_*` prefix above
 * tags resolver / load-path errors; `_VERIFY_*` tags check-path
 * errors — the prefix-split rationale lives in
 * `n00b_attest_error.h`'s "Verifier check-path errors" section
 * header):
 *
 *   - @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH          (-5006)
 *   - @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT               (-5007)
 */

/**
 * @brief Opaque verifier handle.
 *
 * Constructed by @ref n00b_attest_verifier_resolve, consumed by
 * @ref n00b_attest_verifier_check,
 * @ref n00b_attest_verifier_pubkey_spki_der and
 * @ref n00b_attest_verifier_keyid, freed by
 * @ref n00b_attest_verifier_release. The struct definition is
 * private to the module's `src/attest/` translation units; the
 * concrete shape (backend vtable pointer + per-backend state)
 * lives behind the internal verifier-backend headers and is not
 * exposed to library consumers.
 */
typedef struct n00b_attest_verifier n00b_attest_verifier_t;

/**
 * @brief Resolve a verifier from a backend URI.
 *
 * @kw ref        Backend URI such as
 *                `file:///etc/n00b-attest/ci.pub.pem`. When
 *                `nullptr` the resolver walks the default
 *                discovery chain (FR-VM-2) — in WP-003 the
 *                discovery chain is empty and a null `ref` returns
 *                @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND. Per
 *                D-035 part 2 this kwarg is
 *                `n00b_string_t * = nullptr` rather than the
 *                cross-project canonical `n00b_option_t(...)` shape,
 *                matching the existing signer / WP-001 module
 *                surfaces.
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator). Owns the returned verifier handle plus
 *                every byte the backend produces while serving
 *                subsequent calls against that handle.
 *
 * @return `n00b_result_ok(n00b_attest_verifier_t *, verifier)` on
 *         success; `n00b_result_err(...)` with one of:
 *         - @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME — the
 *           URI's scheme has no registered verifier backend.
 *         - @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND — the
 *           backend could not locate the referenced public key
 *           (e.g., the file path does not exist).
 *         - @ref N00B_ATTEST_ERR_VERIFIER_PEM_PARSE_FAILED — the
 *           PEM container did not parse or has the wrong armor
 *           label (D-044 OQ-3: strict `-----BEGIN PUBLIC KEY-----`
 *           only).
 *         - @ref N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED — the
 *           inner DER did not walk to a well-formed
 *           SubjectPublicKeyInfo.
 *         - @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM —
 *           the loaded key's algorithm is not supported by any
 *           backend registered for that scheme.
 *
 * @post On success the returned handle owns a copy of the public
 *       key bytes, the corresponding SPKI DER, and a derived
 *       keyid (byte-equal to the signer's keyid for the same
 *       underlying key material per D-039). The caller MUST call
 *       @ref n00b_attest_verifier_release at end-of-use to free
 *       the handle's cached buffers.
 */
extern n00b_result_t(n00b_attest_verifier_t *)
n00b_attest_verifier_resolve() _kargs
{
    n00b_string_t    *ref       = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Verify a signature over an arbitrary byte buffer.
 *
 * @param verifier  The verifier handle returned by
 *                  @ref n00b_attest_verifier_resolve.
 * @param bytes     The bytes the signature claims to cover. For
 *                  DSSE envelopes this is the PAE byte string
 *                  produced by @ref n00b_attest_envelope_pae_bytes;
 *                  the verifier is payload-shape-agnostic
 *                  (architecture §6: "verify arbitrary bytes").
 * @param sig       The signature bytes (64 bytes for Ed25519).
 *
 * @kw allocator  Optional allocator; owns any scratch the verify
 *                path might allocate. Defaults to the allocator
 *                the verifier was loaded with
 *                (@ref n00b_attest_verifier_resolve's
 *                `@kw allocator`); ultimately falls back to the
 *                runtime allocator if neither was specified.
 *
 * @return Verdict-encoding `n00b_result_t(bool)`:
 *         - `n00b_result_ok(bool, true)` — the signature is valid
 *           over `bytes` under the verifier's public key.
 *         - `n00b_result_ok(bool, false)` — the signature is NOT
 *           valid over `bytes` under the verifier's public key.
 *           This is a verdict, not a failure: the machinery
 *           processed the inputs and reached a definitive "no".
 *           Callers (e.g., Phase 4's `verify` CLI) MUST NOT
 *           collapse this case into `Err` — Phase 4's 3-code
 *           exit shape depends on the `Ok(true)`/`Ok(false)`
 *           split.
 *         - `n00b_result_err(...)` — machinery failure:
 *           - @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT for null
 *             `verifier` / `bytes` / `sig` (D-047 W-1).
 *           - @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH for a
 *             signature buffer whose length does not match the
 *             algorithm's expected length (64 bytes for Ed25519;
 *             D-047 W-1).
 *           - Other machinery failures (OOM, etc.) under the
 *             same `_VERIFY_*` namespace as later WPs require.
 *
 * @pre `verifier` was returned by
 *      @ref n00b_attest_verifier_resolve and has not been
 *      released.
 *
 * @note The verifier surface deliberately keeps the verdict on
 *       the `Ok` channel and the machinery-failure on the `Err`
 *       channel. The dual on the signer side is
 *       @ref n00b_attest_signer_sign returning
 *       `n00b_result_t(n00b_buffer_t *)` where `Ok` carries the
 *       signature bytes and `Err` carries the (rare) signer
 *       machinery failures.
 */
extern n00b_result_t(bool)
n00b_attest_verifier_check(n00b_attest_verifier_t *verifier,
                           n00b_buffer_t          *bytes,
                           n00b_buffer_t          *sig) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return the verifier's public key in SubjectPublicKeyInfo
 *        DER form.
 *
 * Per architecture §6.1 (mirroring the signer-side getter) the
 * SPKI DER bytes are constructed at load time and stored on the
 * verifier state; the getter returns the pre-built buffer wrapper
 * and performs no allocation. There is therefore no `_kargs`
 * allocator slot and no `n00b_result_t` wrapping — the only
 * "failure" would be a null `verifier` argument, which is a
 * precondition violation per the `@pre` clause below and is not
 * modeled as a runtime error.
 *
 * @param verifier  The verifier handle returned by
 *                  @ref n00b_attest_verifier_resolve.
 *
 * @return The standard X.509 SubjectPublicKeyInfo DER encoding
 *         of the public key. For Ed25519 the encoded form is
 *         the 44-byte sequence
 *         `SEQUENCE { AlgorithmIdentifier { OID 1.3.101.112 },
 *         BIT STRING { 0x00, <32-byte pubkey> } }`. The returned
 *         buffer is owned by the verifier and remains valid until
 *         @ref n00b_attest_verifier_release is called on the
 *         verifier; callers must not free it independently.
 *
 * @pre `verifier` was returned by
 *      @ref n00b_attest_verifier_resolve and has not been
 *      released.
 */
extern n00b_buffer_t *
n00b_attest_verifier_pubkey_spki_der(n00b_attest_verifier_t *verifier);

/**
 * @brief Return the verifier's keyid.
 *
 * The keyid is the lowercase-hex encoding of SHA-256 over the
 * 44-byte SubjectPublicKeyInfo DER (per D-039; full 32-byte
 * SHA-256 output, 64 hex characters). This matches the cosign /
 * sigstore ecosystem convention.
 *
 * **D-039 byte-equality invariant.** The verifier's keyid MUST
 * byte-equal the signer's keyid for the same underlying key
 * material. The verifier WP-003's regression
 * `test_attest_verifier_keyid.c` gates this invariant explicitly:
 * load the same RFC 8032 §7.1 vector #1 seed through the signer
 * (via PKCS#8 PEM) and through the verifier (via SPKI PEM), and
 * assert byte-equal keyid strings. Divergence here causes Phase
 * 3's envelope-verify wrapper to silently skip every signature
 * (the wrapper filters `signatures[].keyid` by string match
 * against the verifier's keyid).
 *
 * Symmetric with @ref n00b_attest_verifier_pubkey_spki_der: no
 * `_kargs` slot, no `n00b_result_t` wrapper — the only failure
 * mode is a null `verifier`, which is a precondition violation
 * per the @c \@pre clause below and not a runtime error.
 *
 * @param verifier  The verifier handle returned by
 *                  @ref n00b_attest_verifier_resolve.
 *
 * @return The hex-encoded SHA-256 of the verifier's SPKI DER as
 *         an `n00b_string_t *`. The returned string is owned by
 *         the verifier and remains valid until
 *         @ref n00b_attest_verifier_release is called on the
 *         verifier; callers must not free it independently.
 *
 * @pre `verifier` was returned by
 *      @ref n00b_attest_verifier_resolve and has not been
 *      released.
 */
extern n00b_string_t *
n00b_attest_verifier_keyid(n00b_attest_verifier_t *verifier);

/**
 * @brief Release a verifier handle.
 *
 * @param verifier  The verifier handle to release. Calling on a
 *                  null pointer is a no-op.
 *
 * @details
 *
 * Symmetric with @ref n00b_attest_signer_release. **No
 * `crypto_wipe`** is performed: the verifier holds public-key
 * bytes (raw pubkey + SPKI DER + derived keyid), none of which
 * are secret. The entry point exists for caller-uniform lifetime
 * management — a caller that already calls
 * `n00b_attest_signer_release(signer)` against a sibling signer
 * handle can call `n00b_attest_verifier_release(verifier)` with
 * the same shape and not have to special-case the wipe-vs-no-wipe
 * distinction.
 *
 * @post Every cached buffer the backend held for `verifier` is
 *       returned to the allocator. Calling any other entry point
 *       on `verifier` after release is undefined behavior
 *       (matching the libn00b release-then-use convention).
 */
extern void
n00b_attest_verifier_release(n00b_attest_verifier_t *verifier);
