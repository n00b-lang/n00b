#pragma once

/**
 * @file internal/attest/backends/file.h
 * @brief Package-private state + vtable for the file signer
 *        backend.
 *
 * Loads an Ed25519 secret key from a PKCS#8 PEM file (FR-SM-1
 * file portion + FR-7 raw-key path). Headers stay private to the
 * module's translation units — public consumers see only the
 * `n00b_attest_signer_*` surface in
 * `include/attest/n00b_attest_signer.h`.
 *
 * # Algorithm scope
 *
 * The file backend itself is algorithm-agnostic at the surface
 * level (D-016): the load path identifies the concrete algorithm
 * from the PrivateKeyInfo's AlgorithmIdentifier OID. WP-002 only
 * accepts id-Ed25519 (`1.3.101.112`, RFC 8410). Any other OID
 * surfaces @ref N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM. A later
 * WP adds id-ecPublicKey + secp256r1 (ECDSA P-256) under the
 * same backend without breaking the surface.
 *
 * # URI tolerance (D-035 OQ-4)
 *
 * The backend's `load` accepts both `file:<absolute-path>`
 * (FR-SM-1 strict) and `file:///<absolute-path>` (RFC 3986) and
 * normalizes to a single internal form before opening the file.
 */

#include <n00b.h>
#include "adt/result.h"
#include "internal/attest/backends.h"

/**
 * @brief Per-signer state held by the file backend between load
 *        and release.
 *
 * First field is the @ref n00b_attest_signer base so the
 * resolver edge can read `signer->backend->vtable_member(...)`
 * uniformly without knowing which backend produced the handle.
 *
 * The expanded secret-key buffer is the **full 64-byte** form
 * returned by Monocypher's `crypto_ed25519_key_pair` (seed half
 * + pubkey half, RFC 8032 expanded shape). The seed buffer
 * Monocypher consumed at load time is wiped by Monocypher itself
 * as part of the derivation; this struct never holds the raw
 * seed. The release path wipes the full 64 bytes via
 * `crypto_wipe` before returning the buffer to the allocator
 * (FR-SM-3 "zeroize on release").
 *
 * The SubjectPublicKeyInfo DER bytes for Ed25519 are the fixed
 * 12-byte prefix + the 32-byte raw pubkey (= 44 bytes total).
 * They are constructed once at load time and stored on this
 * struct so the @c pubkey vtable entry can return a pre-built
 * `n00b_buffer_t *` without any per-call allocation or
 * reconstruction. The same 44-byte SPKI form is the input to
 * the keyid derivation (`SHA-256(SPKI DER)`, hex-encoded), per
 * D-039 (resolves DF-003) — full SHA-256, 64 hex characters,
 * matching the cosign / sigstore ecosystem convention. (D-039
 * is not yet logged; the orchestrator will log it after this
 * dispatch returns clean — pre-stage the reference in source
 * comments and the spec text.)
 */
typedef struct {
    /** Vtable base; first field per the backends.h convention. */
    struct n00b_attest_signer base;

    /**
     * 64-byte expanded Ed25519 secret key. Wiped via
     * `crypto_wipe` at release time.
     */
    uint8_t expanded_sk[64];

    /**
     * 32-byte raw Ed25519 public key (RFC 8032). Public value;
     * freed without `crypto_wipe`. Stored as a fixed-size array
     * so the SPKI-DER assembly path at load time can read it
     * directly without an extra allocator-aware accessor.
     */
    uint8_t pubkey[32];

    /**
     * 44-byte SubjectPublicKeyInfo DER for the public key:
     * 12-byte fixed Ed25519 prefix + 32-byte raw pubkey.
     * Constructed once at load time; referenced by
     * @c spki_buffer for the `pubkey` vtable entry; consumed by
     * the keyid derivation. Public value; release-time wipe is
     * defense-in-depth, not strictly required.
     */
    uint8_t spki_der[44];

    /**
     * Pre-built `n00b_buffer_t *` wrapper around @c spki_der.
     * Allocated under the load-time allocator and returned
     * unchanged on every `pubkey` getter call (the getter is
     * allocation-free post-load).
     */
    n00b_buffer_t *spki_buffer;

    /**
     * Keyid: hex-encoded SHA-256 of the 44-byte SPKI DER per
     * D-039 (resolves DF-003) — full SHA-256 output (32 bytes),
     * hex-encoded (64 chars), matching cosign / sigstore.
     * Public value; freed without `crypto_wipe`.
     */
    n00b_string_t *keyid;

    /**
     * The allocator the load path captured. Threaded forward
     * into the keyid string + SPKI buffer allocations at load
     * time and the sign path (Phase 3) so the signer's outputs
     * land in the same allocation scope as the handle itself.
     */
    n00b_allocator_t *allocator;
} n00b_attest_signer_file_t;

/**
 * @brief File backend vtable instance.
 *
 * File-scope (non-`const`) instance defined in
 * `src/attest/backends/file.c`. The fields are populated by
 * `n00b_attest_module_init` (which also calls
 * `n00b_attest_register_backend(&n00b_attest_backend_file)`).
 * Once init returns, the vtable is read-only for the process
 * lifetime; the const-ness is enforced by convention, not by
 * the type system (the resolver dispatches through the same
 * struct it would dispatch through if the vtable were
 * file-scope `static const`).
 */
extern n00b_attest_backend_t n00b_attest_backend_file;

/**
 * @brief Populate the file-backend vtable.
 *
 * Package-private helper called once by
 * `n00b_attest_module_init` to fill `n00b_attest_backend_file`'s
 * fields (scheme string, function pointers) from within the
 * `backends/file.c` translation unit (where the file-static
 * function pointers are in scope). Module-init then registers
 * the populated vtable via @ref n00b_attest_register_backend.
 *
 * Must be called exactly once before any signer-resolve happens;
 * the vtable is read-only thereafter for the process lifetime.
 */
extern void
_n00b_attest_backend_file_init(void);
