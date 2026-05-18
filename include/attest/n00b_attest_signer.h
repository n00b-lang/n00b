#pragma once

/**
 * @file n00b_attest_signer.h
 * @brief Signer abstraction public surface.
 *
 * Declarations for the opaque `n00b_attest_signer_t` and its
 * resolver / sign / pubkey / release entry points. The signer
 * surface is deliberately tiny (architecture §6): load by URI,
 * sign arbitrary bytes, expose a public key, release. Backends
 * differ behind the resolver — each one implements the package-
 * private vtable in `internal/attest/backends.h` and is dispatched
 * by URI scheme.
 *
 * # Algorithm-agnostic by construction (D-016)
 *
 * The public surface deliberately bakes in no algorithm tags.
 * WP-002 ships only the Ed25519 file backend; a later WP adds
 * ECDSA P-256 (and others) by registering additional backends
 * behind the same surface. There are no `n00b_attest_signer_ed25519_*`
 * symbols — callers reach for `n00b_attest_signer_*` exclusively
 * and the backend identifies the algorithm from the loaded key
 * material.
 *
 * # URI dispatch (FR-SM-1, FR-SM-2)
 *
 * @ref n00b_attest_signer_resolve takes a `ref` URI such as
 * `file:///etc/n00b-attest/keys/ci.pem` and dispatches to the
 * matching backend. WP-002 ships only the `file://` backend;
 * unknown schemes return @ref N00B_ATTEST_ERR_UNSUPPORTED_SCHEME.
 * Per D-035 OQ-4 the file backend accepts both `file:<path>`
 * (FR-SM-1 strict) and `file:///<path>` (RFC 3986) forms.
 *
 * # Lifetime (architecture §6.2)
 *
 * A signer handle is reusable for the duration of a build session:
 * resolve once, sign many, release once. The library does NOT
 * maintain a process-wide cache — callers own the lifetime
 * explicitly. @ref n00b_attest_signer_release zeroizes any
 * private key material the backend held before freeing the
 * handle (FR-SM-3).
 *
 * # Allocator discipline
 *
 * Every allocating entry point on this surface carries
 * `.allocator = nullptr`. Threading an arena through
 * @ref n00b_attest_signer_resolve makes every byte that resolve
 * (and the subsequent sign / pubkey / release operations) produce
 * live in that arena (FR-21 / FR-22).
 *
 * # Optional-pointer kwargs (D-035 part 2)
 *
 * Optional pointer kwargs on this surface use `T * = nullptr`
 * rather than `n00b_option_t(T) = n00b_option_none(T)`, matching
 * the existing WP-001 module surface (Statement / DSSE). This is
 * a project-local exception to the cross-project canonical
 * `n00b_option_t` shape; cross-project normalization is a later
 * cleanup WP.
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_error.h>

/*
 * The 7 signer-domain error codes (-4001 … -4007) live in
 * `n00b_attest_error.h` per the WP-002 Phase 3 cleanup pass
 * (`n00b-code-auditor` W-3 finding): the full 16-code namespace
 * has a single declaration point. This header references them via
 * @ref tags only:
 *
 *   - @ref N00B_ATTEST_ERR_UNSUPPORTED_SCHEME    (-4001)
 *   - @ref N00B_ATTEST_ERR_KEY_NOT_FOUND         (-4002)
 *   - @ref N00B_ATTEST_ERR_PEM_PARSE_FAILED      (-4003)
 *   - @ref N00B_ATTEST_ERR_DER_PARSE_FAILED      (-4004)
 *   - @ref N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM (-4005)
 *   - @ref N00B_ATTEST_ERR_SIGN_FAILED           (-4006)
 *   - @ref N00B_ATTEST_ERR_NOT_IMPLEMENTED       (-4007)
 */

/**
 * @brief Opaque signer handle.
 *
 * Constructed by @ref n00b_attest_signer_resolve, consumed by
 * @ref n00b_attest_signer_sign and
 * @ref n00b_attest_signer_pubkey_spki_der, freed by
 * @ref n00b_attest_signer_release. The struct definition is
 * private to the module's `src/attest/` translation units; the
 * concrete shape (backend vtable pointer + per-backend state)
 * lives behind the internal backend headers and is not exposed
 * to library consumers.
 */
typedef struct n00b_attest_signer n00b_attest_signer_t;

/**
 * @brief Resolve a signer from a backend URI.
 *
 * @kw ref        Backend URI such as `file:///etc/n00b-attest/ci.pem`.
 *                When `nullptr` the resolver walks the default
 *                discovery chain (FR-SM-2) — in WP-002 the
 *                discovery chain is empty and a null `ref` returns
 *                @ref N00B_ATTEST_ERR_KEY_NOT_FOUND. Per D-035
 *                part 2 this kwarg is `n00b_string_t * = nullptr`
 *                rather than the cross-project canonical
 *                `n00b_option_t(n00b_string_t *) = n00b_option_none(...)`
 *                shape, matching the existing WP-001 module
 *                surface.
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator). Owns the returned signer handle plus
 *                every byte the backend produces while serving
 *                subsequent calls against that handle.
 *
 * @return `n00b_result_ok(n00b_attest_signer_t *, signer)` on
 *         success; `n00b_result_err(...)` with one of:
 *         - @ref N00B_ATTEST_ERR_UNSUPPORTED_SCHEME — the URI's
 *           scheme has no registered backend.
 *         - @ref N00B_ATTEST_ERR_KEY_NOT_FOUND — the backend
 *           could not locate the referenced key (e.g., the file
 *           path does not exist).
 *         - @ref N00B_ATTEST_ERR_PEM_PARSE_FAILED — the PEM
 *           container did not parse.
 *         - @ref N00B_ATTEST_ERR_DER_PARSE_FAILED — the inner
 *           DER did not walk to a well-formed PrivateKeyInfo.
 *         - @ref N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM — the
 *           loaded key's algorithm is not supported by any
 *           backend registered for that scheme.
 *
 * @post On success the returned handle owns a private copy of
 *       the secret-key material (whatever shape the backend
 *       requires for sign), the corresponding public key bytes,
 *       and a derived keyid. The caller MUST call
 *       @ref n00b_attest_signer_release at end-of-use to wipe
 *       the private material.
 */
extern n00b_result_t(n00b_attest_signer_t *)
n00b_attest_signer_resolve() _kargs
{
    n00b_string_t    *ref       = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Sign an arbitrary byte buffer with the signer's secret
 *        key.
 *
 * @param signer         The signer handle returned by
 *                       @ref n00b_attest_signer_resolve.
 * @param bytes_to_sign  The bytes to sign. For DSSE envelopes
 *                       this is the PAE byte string produced by
 *                       @ref n00b_attest_envelope_pae_bytes; the
 *                       signer is payload-shape-agnostic
 *                       (architecture §6: "sign arbitrary
 *                       bytes").
 *
 * @kw allocator  Optional allocator; owns the returned signature
 *                buffer. Defaults to the allocator the signer was
 *                loaded with (@ref n00b_attest_signer_resolve's
 *                `@kw allocator`); ultimately falls back to the
 *                runtime allocator if neither was specified.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, sig)` on success with
 *         the algorithm-appropriate signature bytes (64 bytes
 *         for Ed25519); `n00b_result_err(...)` on backend
 *         failure with @ref N00B_ATTEST_ERR_SIGN_FAILED.
 *
 * @pre `signer` was returned by @ref n00b_attest_signer_resolve
 *      and has not been released.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_signer_sign(n00b_attest_signer_t *signer,
                        n00b_buffer_t        *bytes_to_sign) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Return the signer's public key in SubjectPublicKeyInfo
 *        DER form.
 *
 * Per architecture §6.1 the SPKI DER bytes are constructed at
 * load time and stored on the signer state; the getter returns
 * the pre-built buffer wrapper and performs no allocation. There
 * is therefore no `_kargs` allocator slot and no
 * `n00b_result_t` wrapping — the only "failure" would be a null
 * `signer` argument, which is a precondition violation per the
 * `@pre` clause below and is not modeled as a runtime error.
 *
 * @param signer  The signer handle returned by
 *                @ref n00b_attest_signer_resolve.
 *
 * @return The standard X.509 SubjectPublicKeyInfo DER encoding
 *         of the public key. For Ed25519 the encoded form is
 *         the 44-byte sequence
 *         `SEQUENCE { AlgorithmIdentifier { OID 1.3.101.112 },
 *         BIT STRING { 0x00, <32-byte pubkey> } }`. The returned
 *         buffer is owned by the signer and remains valid until
 *         @ref n00b_attest_signer_release is called on the
 *         signer; callers must not free it independently.
 *
 * @pre `signer` was returned by @ref n00b_attest_signer_resolve
 *      and has not been released.
 */
extern n00b_buffer_t *
n00b_attest_signer_pubkey_spki_der(n00b_attest_signer_t *signer);

/**
 * @brief Return the signer's keyid.
 *
 * The keyid is the lowercase-hex encoding of SHA-256 over the
 * 44-byte SubjectPublicKeyInfo DER (per D-039; full 32-byte
 * SHA-256 output, 64 hex characters), matching the cosign /
 * sigstore ecosystem convention. The backend constructs the
 * keyid at load time and stores it on its per-signer state; the
 * getter returns the cached value with no allocation and no
 * per-call recomputation.
 *
 * The keyid is the value envelope `signatures[].keyid` carries
 * (D-016 algorithm-agnostic shape) and is the canonical handle a
 * verifier uses to associate a signature entry with a public
 * key.
 *
 * Symmetric with @ref n00b_attest_signer_pubkey_spki_der: no
 * `_kargs` slot, no `n00b_result_t` wrapper — the only failure
 * mode is a null `signer`, which is a precondition violation
 * per the @c \@pre clause below and not a runtime error.
 *
 * @param signer  The signer handle returned by
 *                @ref n00b_attest_signer_resolve.
 *
 * @return The hex-encoded SHA-256 of the signer's SPKI DER as
 *         an `n00b_string_t *`. The returned string is owned by
 *         the signer and remains valid until
 *         @ref n00b_attest_signer_release is called on the
 *         signer; callers must not free it independently.
 *
 * @pre `signer` was returned by @ref n00b_attest_signer_resolve
 *      and has not been released.
 */
extern n00b_string_t *
n00b_attest_signer_keyid(n00b_attest_signer_t *signer);

/**
 * @brief Release a signer handle and zeroize its private key
 *        material.
 *
 * @param signer  The signer handle to release. Calling on a null
 *                pointer is a no-op.
 *
 * @details
 *
 * The backend's `release` vtable entry is responsible for wiping
 * every byte of private key material the backend held — for the
 * Ed25519 file backend that is the full 64-byte expanded secret
 * key returned by `crypto_ed25519_key_pair`, not just the 32-byte
 * seed half. Public-key bytes and the derived keyid are public
 * values and are freed without `crypto_wipe`.
 *
 * @post Every private byte the backend held for `signer` is
 *       overwritten before the buffer that contained it returns
 *       to the allocator. Calling any other entry point on
 *       `signer` after release is undefined behavior (matching
 *       the libn00b release-then-use convention).
 */
extern void
n00b_attest_signer_release(n00b_attest_signer_t *signer);
