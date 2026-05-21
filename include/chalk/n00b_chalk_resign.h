#pragma once

/**
 * @file n00b_chalk_resign.h
 * @brief Cross-platform binary re-sign API for libchalk (WP-005 P4).
 *
 * Composes the Phase 3 Authenticode primitives (PE parser/builder,
 * `n00b_pe_authentihash_sha256`, the DER encoder, the PKCS#7
 * SignedData builder, and the RSA-PKCS1-v1.5 SHA-256 sign
 * primitive) into a single re-sign entry point. The Phase 4 body
 * ships `n00b_chalk_pe_resign`; `n00b_chalk_macho_resign` is
 * declared here for Phase 5.
 *
 * # Concept
 *
 * Re-signing means: strip any prior platform signature on a binary,
 * compute the platform-specific code hash (Authenticode for PE,
 * Mach-O code-directory hash for Mach-O), build the platform
 * signature blob using the caller-supplied signer identity, and
 * write the signed binary back to disk.
 *
 * If no signer identity is supplied (`signer_identity = nullptr`),
 * the re-sign collapses to strip-only: the prior signature is
 * removed and the binary is written back unsigned, with a
 * structured warning emitted to stderr ("the binary is no longer
 * Authenticode-signed").
 *
 * # Signer identity
 *
 * Identities resolve via URI:
 *
 * - `file://path/to/cert.pem,file://path/to/key.pem` — paired
 *   PEM files; the cert is X.509 PEM and the key is PKCS#8 PEM
 *   (RSA-2048 or larger). Comma-separated; both halves are
 *   required.
 * - `store://<name>` — XDG lookup against
 *   `$XDG_CONFIG_HOME/n00b-attest/signing-identities/<name>.cert.pem`
 *   plus `<name>.key.pem` (paired by basename). When
 *   `$XDG_CONFIG_HOME` is unset, falls back to
 *   `$HOME/.config/n00b-attest/signing-identities/`.
 * - `nullptr` — returns `Ok(nullptr)` from the resolver; callers
 *   pass that through to `n00b_chalk_pe_resign` to opt into
 *   strip-only.
 *
 * Single-file PKCS#12 / .p12 / .pfx bundles are NOT supported in
 * WP-005 — future ergonomics WP if a real consumer asks.
 *
 * # Deterministic signing
 *
 * The signature is deterministic over the input bytes + signer
 * identity. No timestamp authority (RFC 3161 counter-signature)
 * is consulted in v1 — future ergonomics WP if a consumer needs
 * it. Same inputs → same output bytes byte-for-byte.
 *
 * # Allocator discipline
 *
 * Every entry point accepts `.allocator = nullptr`. The opaque
 * identity handle stores the caller's allocator at construction
 * time; subsequent re-sign calls thread the allocator through
 * every internal sub-allocation.
 *
 * # Symbol prefix
 *
 * `n00b_chalk_*`. Error code macros use `N00B_CHALK_ERR_*` in the
 * libchalk reserved range `-7000..-7099` (per WP-005 plan §Phase
 * 4 "Public symbols added"). Phase 4 reserves -7010, -7011, -7012.
 */

#include <n00b.h>
#include "adt/result.h"

/**
 * @brief Opaque resolved signer identity.
 *
 * Holds the X.509 cert DER + the RSA private-key (n, d) bytes +
 * the cert's issuer DN + serial number, all sliced/decoded out of
 * the PEM input. The handle's lifetime ends at
 * @ref n00b_chalk_signer_identity_release; until then the
 * handle's interior pointers are GC-tracked or arena-tracked per
 * the caller's allocator.
 */
typedef struct n00b_chalk_signer_identity n00b_chalk_signer_identity_t;

/* Error codes (negative, libn00b convention; libchalk reserved
 * range -7000..-7099 per WP-005 plan). */

/** Signer identity URI resolved to a missing or unreadable
 *  file/store entry. */
#define N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND (-7010)

/** PE re-sign attempted but the underlying parse / hash / build /
 *  write path failed. */
#define N00B_CHALK_ERR_RESIGN_FAILED             (-7011)

/** PEM cert or PKCS#8 key could not be parsed. Covers malformed
 *  base64, missing PEM markers, ASN.1 walk failure, and the
 *  unsupported-algorithm case (non-RSA keys). */
#define N00B_CHALK_ERR_KEY_PARSE_FAILED          (-7012)

/**
 * @brief Resolve a signer identity URI.
 *
 * @param uri  URI selecting the signer:
 *             - `file://cert.pem,file://key.pem` — paired PEM
 *               files.
 *             - `store://<name>` — XDG store lookup.
 *             - `nullptr` — yields `Ok(nullptr)` (caller opts into
 *               strip-only mode).
 *
 * @kw allocator  Optional allocator (default: runtime). Owned by
 *                the returned handle and threaded through every
 *                internal allocation.
 *
 * @return On success, `Ok(handle)` (or `Ok(nullptr)` for the
 *         null-URI strip-only case). On failure,
 *         `Err(N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND)` (no
 *         such file / store entry) or
 *         `Err(N00B_CHALK_ERR_KEY_PARSE_FAILED)` (PEM / PKCS#8
 *         malformed or unsupported algorithm).
 *
 * @details
 *
 * `file://` paths are taken verbatim (no shell expansion). The
 * URI separator is a single literal `,` between the two
 * `file://` prefixes; whitespace is not stripped.
 *
 * `store://` lookups search the directory described above for
 * `<name>.cert.pem` and `<name>.key.pem`. Both must exist;
 * missing either half is `_NOT_FOUND`.
 */
extern n00b_result_t(n00b_chalk_signer_identity_t *)
n00b_chalk_signer_identity_resolve(n00b_string_t *uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Release a resolved signer identity.
 *
 * @param id  Handle returned by
 *            @ref n00b_chalk_signer_identity_resolve. Safe on
 *            nullptr.
 *
 * @details Zeroes the private-key bytes before returning. The
 * handle itself is GC-tracked / arena-tracked, so this function
 * does NOT free the handle's backing storage — it merely
 * scrubs sensitive material so a subsequent GC sweep / arena
 * reset leaves no key residue in memory.
 */
extern void
n00b_chalk_signer_identity_release(n00b_chalk_signer_identity_t *id);

/**
 * @brief Re-sign a PE binary at @p path.
 *
 * Strips any prior Authenticode signature, computes the
 * Authenticode SHA-256 hash, builds an `SpcIndirectDataContent`
 * blob, wraps it in a PKCS#7 SignedData using the supplied
 * identity, embeds the SignedData in the PE cert table, and
 * writes the result back to @p path.
 *
 * @param path  Filesystem path to the PE binary. Must be a
 *              read+writable regular file; the file's contents
 *              are read in full, parsed, rebuilt, and written
 *              back.
 *
 * @kw signer_identity  Resolved identity (from
 *                      @ref n00b_chalk_signer_identity_resolve).
 *                      nullptr selects strip-only mode (any prior
 *                      Authenticode signature is removed; the
 *                      binary is written back unsigned with a
 *                      structured warning).
 * @kw allocator        Optional allocator (default: runtime).
 *
 * @return `Ok(true)` on success;
 *         `Err(N00B_CHALK_ERR_RESIGN_FAILED)` on any parse /
 *         hash / build / write failure.
 *
 * @pre @p path is non-nullptr.
 *
 * @post On success, the file at @p path is a valid PE32+ binary.
 *       In strip-only mode it carries no Authenticode signature;
 *       in signed mode it carries a fresh PKCS#7 SignedData blob
 *       in the certificate table.
 *
 * @details
 *
 * Signing is deterministic — repeated calls with the same input
 * bytes and the same signer identity produce byte-identical
 * output.
 */
extern n00b_result_t(bool)
n00b_chalk_pe_resign(n00b_string_t *path) _kargs
{
    n00b_chalk_signer_identity_t *signer_identity = nullptr;
    n00b_allocator_t             *allocator       = nullptr;
};

/**
 * @brief Re-sign a Mach-O binary at @p path.
 *
 * **Declaration only in WP-005 Phase 4.** The body lands in Phase
 * 5. Calling this entry point pre-P5 returns
 * `Err(N00B_CHALK_ERR_RESIGN_FAILED)` (no-op body), which the
 * test harness verifies as the intentional pre-P5 stub.
 *
 * @param path  Filesystem path to the Mach-O binary (or fat
 *              archive). Must be a read+writable regular file.
 *
 * @kw signer_identity  Resolved identity. nullptr selects
 *                      strip-only mode.
 * @kw allocator        Optional allocator (default: runtime).
 *
 * @return `Ok(true)` on success;
 *         `Err(N00B_CHALK_ERR_RESIGN_FAILED)` on failure.
 */
extern n00b_result_t(bool)
n00b_chalk_macho_resign(n00b_string_t *path) _kargs
{
    n00b_chalk_signer_identity_t *signer_identity = nullptr;
    n00b_allocator_t             *allocator       = nullptr;
};
