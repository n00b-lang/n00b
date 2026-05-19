#pragma once

/**
 * @file internal/attest/verifier_backends/file.h
 * @brief Package-private state + vtable for the file verifier
 *        backend.
 *
 * Loads an Ed25519 public key from an SPKI PEM file (FR-VM-1
 * file portion). Headers stay private to the module's
 * translation units — public consumers see only the
 * `n00b_attest_verifier_*` surface in
 * `include/attest/n00b_attest_verifier.h`.
 *
 * # Algorithm scope
 *
 * The file backend itself is algorithm-agnostic at the surface
 * level (D-016): the load path identifies the concrete algorithm
 * from the SubjectPublicKeyInfo's AlgorithmIdentifier OID.
 * WP-003 only accepts id-Ed25519 (`1.3.101.112`, RFC 8410). Any
 * other OID surfaces @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM.
 * A later WP adds id-ecPublicKey + secp256r1 (ECDSA P-256) under
 * the same backend without breaking the surface.
 *
 * # URI tolerance (D-035 OQ-4)
 *
 * The backend's `load` accepts both `file:<absolute-path>`
 * (FR-SM-1 strict form, mirrored on the verifier side) and
 * `file:///<absolute-path>` (RFC 3986) and normalizes to a
 * single internal form before opening the file.
 *
 * # PEM strictness (D-044 OQ-3)
 *
 * Only `-----BEGIN PUBLIC KEY-----` is accepted. The backend
 * does NOT accept `-----BEGIN ED25519 PUBLIC KEY-----` (a
 * legacy form some tooling emits) or raw-pubkey-without-armor
 * forms. The algorithm-identifier OID check inside the SPKI
 * walk is the algorithm-discrimination point; the PEM label
 * itself stays algorithm-agnostic.
 */

#include <n00b.h>
#include "adt/result.h"
#include "internal/attest/verifier_backends.h"

/**
 * @brief Per-verifier state held by the file backend between
 *        load and release.
 *
 * First field is the @ref n00b_attest_verifier base so the
 * resolver edge can read `verifier->backend->vtable_member(...)`
 * uniformly without knowing which backend produced the handle.
 *
 * The raw 32-byte public key is held in-struct (no separate
 * allocation, no GC tracing needed — public bytes, fixed size,
 * RFC 8032 wire form). The 44-byte SPKI DER is the same form
 * the signer-side caches (12-byte fixed Ed25519 prefix +
 * 32-byte raw pubkey); reusing the format keeps the keyid
 * byte-equality invariant with the signer trivial to satisfy
 * (D-039). The `n00b_buffer_t *spki_buffer` wrapper is the
 * pre-built return value of the `pubkey` getter and is
 * allocator-owned; the `keyid` string is similarly pre-computed
 * at load time and allocator-owned.
 *
 * **No `crypto_wipe` semantics.** Every byte this struct holds
 * is a public value. The release path frees the cached buffers
 * without zeroizing; this is the principal structural difference
 * between the signer-side and verifier-side state.
 */
typedef struct {
    /** Vtable base; first field per the verifier_backends.h convention. */
    struct n00b_attest_verifier base;

    /**
     * 32-byte raw Ed25519 public key (RFC 8032). Public value;
     * stored as a fixed-size array so the SPKI assembly +
     * keyid derivation paths at load time can read it directly
     * without an extra allocator-aware accessor, and so the
     * `check` path can hand its address to
     * `crypto_ed25519_check` without re-extracting from the
     * SPKI DER.
     */
    uint8_t pubkey[32];

    /**
     * 44-byte SubjectPublicKeyInfo DER for the public key:
     * 12-byte fixed Ed25519 prefix + 32-byte raw pubkey.
     * Constructed once at load time; referenced by
     * @c spki_buffer for the `pubkey` vtable entry; consumed by
     * the keyid derivation. Identical byte sequence to the
     * signer-side cache — the D-039 byte-equality invariant
     * follows from this.
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
     * D-039 — full SHA-256 output (32 bytes), hex-encoded
     * (64 chars), matching the cosign / sigstore convention
     * AND the signer's keyid for the same underlying key
     * material (D-039 byte-equality invariant). Public value;
     * freed without `crypto_wipe`.
     */
    n00b_string_t *keyid;

    /**
     * The allocator the load path captured. Threaded forward
     * into the keyid string + SPKI buffer allocations at load
     * time and any scratch the `check` path might allocate.
     */
    n00b_allocator_t *allocator;
} n00b_attest_verifier_file_t;

/**
 * @brief File verifier-backend vtable instance.
 *
 * File-scope (non-`const`) instance defined in
 * `src/attest/verifier_backends/file.c`. The fields are
 * populated by `n00b_attest_module_init` (which also calls
 * `n00b_attest_register_verifier_backend(&n00b_attest_verifier_backend_file)`).
 * Once init returns, the vtable is read-only for the process
 * lifetime; the const-ness is enforced by convention, not by
 * the type system.
 */
extern n00b_attest_verifier_backend_t n00b_attest_verifier_backend_file;

/**
 * @brief Populate the file verifier-backend vtable.
 *
 * Package-private helper called once by
 * `n00b_attest_module_init` to fill
 * `n00b_attest_verifier_backend_file`'s fields (scheme string,
 * function pointers) from within the `verifier_backends/file.c`
 * translation unit (where the file-static function pointers are
 * in scope). Module-init then registers the populated vtable
 * via @ref n00b_attest_register_verifier_backend.
 *
 * Must be called exactly once before any verifier-resolve
 * happens; the vtable is read-only thereafter for the process
 * lifetime.
 */
extern void
_n00b_attest_verifier_backend_file_init(void);
