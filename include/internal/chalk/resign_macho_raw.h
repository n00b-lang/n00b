/**
 * @file resign_macho_raw.h
 * @internal
 * @brief Pure-C boundary for the macOS Mach-O re-sign bridge.
 *
 * The .m file (`src/chalk/resign_macho_darwin.m`) compiles
 * through Apple's Objective-C compiler so Security.framework
 * headers resolve; it must NOT see ncc extensions like `_kargs`
 * or n00b allocator types. This header declares only plain-C
 * functions and an opaque forward declaration for the signer
 * identity handle.
 *
 * The bridge file (`src/chalk/resign_macho.c`, compiled through
 * ncc) forwards `n00b_chalk_macho_resign` to
 * `_n00b_chalk_macho_resign_darwin`, which reads cert / key
 * bytes out of the opaque handle via the
 * `_n00b_chalk_signer_identity_*` accessors. Those accessors are
 * declared in their own raw header so the .m file does not pull
 * in `n00b_chalk_resign.h` (which uses `_kargs`).
 *
 * Same pattern as `include/internal/net/quic/secret_keychain_raw.h`.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque forward declaration of the signer identity. The .m file
 * never dereferences this pointer directly; it reads field slices
 * through the `_n00b_chalk_signer_identity_*` accessors declared
 * below. */
typedef struct n00b_chalk_signer_identity n00b_chalk_signer_identity_t;

/* Opaque forward declaration of n00b_buffer_t. The .m file only
 * touches the `data` (char *) and `byte_len` fields, accessed via
 * the raw accessors below — it never includes n00b.h's full
 * buffer header. */
typedef struct n00b_buffer_t n00b_buffer_t;

/**
 * @brief Re-sign the Mach-O binary at @p path.
 *
 * Implemented in `src/chalk/resign_macho_darwin.m`; called from
 * `src/chalk/resign_macho.c` on macOS hosts.
 *
 * @param path  Filesystem path to the Mach-O binary. NUL-terminated
 *              C string.
 * @param id    Resolved signer identity, or NULL for ad-hoc
 *              signing (`codesign --sign -`). The .m file reads
 *              cert DER + issuer DN + serial + RSA (n, d) bytes
 *              out of @p id via the accessors below.
 *
 * @return 0 on success; non-zero on failure.
 */
extern int
_n00b_chalk_macho_resign_darwin(const char                   *path,
                                n00b_chalk_signer_identity_t *id);

/* -------------------------------------------------------------------------
 * Raw accessor surface for the signer identity. Mirrors the
 * `internal/chalk/resign_identity_internal.h` declarations but with
 * pure-C types only, so the .m file can call into them without
 * pulling in n00b.h. The implementations live in
 * `src/chalk/resign_identity.c`.
 *
 * Returns / writes raw pointers into the identity's interior bytes.
 * Caller does NOT own the returned bytes — they live as long as the
 * identity handle.
 * ------------------------------------------------------------------------- */

/** Write `*out_bytes` / `*out_len` to point at the cert DER bytes
 *  (whole certificate). Writes NULL / 0 if @p id is NULL. */
extern void
_n00b_chalk_signer_identity_cert_der_raw(n00b_chalk_signer_identity_t *id,
                                         const uint8_t              **out_bytes,
                                         size_t                       *out_len);

/** Write `*out_bytes` / `*out_len` to point at the PKCS#8 key DER
 *  bytes. Writes NULL / 0 if @p id is NULL or the key has been
 *  scrubbed via `n00b_chalk_signer_identity_release`. */
extern void
_n00b_chalk_signer_identity_key_der_raw(n00b_chalk_signer_identity_t *id,
                                        const uint8_t              **out_bytes,
                                        size_t                       *out_len);

#ifdef __cplusplus
}
#endif
