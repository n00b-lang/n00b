#pragma once

/**
 * @file resign_identity_internal.h
 * @brief Internal accessors for the opaque
 *        @ref n00b_chalk_signer_identity_t handle (WP-005 P5 lift).
 *
 * The signer identity struct is opaque in the public header
 * (`include/chalk/n00b_chalk_resign.h`); the re-sign bodies
 * (`src/chalk/resign_pe.c`, `src/chalk/resign_macho.c`,
 * `src/chalk/resign_macho_darwin.m`) need read-only access to
 * the cert DER + issuer DN + serial + RSA (n, d) byte slices to
 * compose PKCS#7 SignedData / SecIdentityRef lookups.
 *
 * These accessors live in `src/chalk/resign_identity.c`; this
 * header declares them so the consumers don't have to repeat
 * `extern` declarations at each translation unit. The leading
 * `_n00b_` prefix is the libchalk internal-helper signal per
 * the n00b-api-guidelines §3.14 / §10.2.
 *
 * @details Lifted from `src/chalk/resign_pe.c`'s file-scope
 * `extern` block by WP-005 P5 so the Mach-O re-sign body can
 * share the same accessor surface.
 */

#include "n00b.h"
#include "chalk/n00b_chalk_resign.h"

#include <stdint.h>
#include <stddef.h>

/** Return the X.509 cert DER buffer (whole certificate, allocator-
 *  owned). Returns @c nullptr if @p id is @c nullptr. */
extern n00b_buffer_t *
_n00b_chalk_signer_identity_cert_der(n00b_chalk_signer_identity_t *id);

/** Return the cert's issuer DN as a DER-encoded SEQUENCE (Name)
 *  buffer. Returns @c nullptr if @p id is @c nullptr. */
extern n00b_buffer_t *
_n00b_chalk_signer_identity_issuer_dn(n00b_chalk_signer_identity_t *id);

/** Return the cert's serial-number raw bytes (big-endian,
 *  sign-byte preserved when present). Writes @c nullptr / 0 on
 *  @p id == @c nullptr. */
extern void
_n00b_chalk_signer_identity_serial(n00b_chalk_signer_identity_t *id,
                                   const uint8_t              **bytes,
                                   size_t                       *len);

/** Return RSA (n, d) big-endian byte slices into the identity's
 *  PKCS#8 key DER. Writes @c nullptr / 0 on @p id == @c nullptr. */
extern void
_n00b_chalk_signer_identity_rsa(n00b_chalk_signer_identity_t *id,
                                const uint8_t              **n_bytes,
                                size_t                       *n_len,
                                const uint8_t              **d_bytes,
                                size_t                       *d_len);
