/** @file src/chalk/resign_identity.c — Signer identity resolver.
 *
 *  Implements the n00b_chalk_signer_identity_resolve / _release
 *  surface declared in include/chalk/n00b_chalk_resign.h. Two URI
 *  shapes are supported (WP-005 Phase 4 disposition):
 *
 *   - `file://path/to/cert.pem,file://path/to/key.pem` — paired
 *     PEM files (cert PEM + PKCS#8 PEM private key).
 *   - `store://<name>` — XDG store lookup against
 *     `$XDG_CONFIG_HOME/n00b-attest/signing-identities/`
 *     (mirrors D-052's `registries.json` convention).
 *
 *  The cert is parsed as X.509 DER via picotls's read-side ASN.1
 *  walker to extract the issuer DN and serial number; the key is
 *  parsed as a PKCS#8 PrivateKeyInfo to extract the (n, d)
 *  RSA-private-key big-endian byte slices. The PKCS#8 walk is
 *  the same pattern as test/unit/test_pkcs7_signed_data.c — the
 *  P3 fix-ups dispatch landed picotls's ptls_load_pem_objects as
 *  the project-wide PEM decoder.
 *
 *  Test-file conventions per D-030. */

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/file.h"
#include "chalk/n00b_chalk_resign.h"
#include "adt/result.h"

#include "picotls.h"
#include "picotls/asn1.h"
#include "picotls/pembase64.h"

#include <stdint.h>
#include <stdlib.h>   // getenv (D-052 libc exception for env-var config discovery)
#include <string.h>

// ---------------------------------------------------------------------------
// Opaque identity struct
// ---------------------------------------------------------------------------
//
// All interior pointers reference allocator-owned bytes (or, for
// the n/d slices, indices into the PEM-decoded key DER which is
// itself allocator-owned via the n00b_buffer_t copy made at parse
// time). _release scrubs the private-key material before
// returning.

struct n00b_chalk_signer_identity {
    n00b_allocator_t *allocator;

    // X.509 cert DER bytes (whole certificate).
    n00b_buffer_t    *cert_der;

    // PKCS#8 key DER bytes (kept so the n/d byte slices remain
    // valid for the lifetime of the handle).
    n00b_buffer_t    *key_der;

    // Issuer DN — a DER-encoded SEQUENCE (Name) sliced out of the
    // cert's tbsCertificate.
    n00b_buffer_t    *issuer_dn_der;

    // Serial number — big-endian raw bytes from the
    // tbsCertificate.serialNumber INTEGER (sign byte preserved
    // when present).
    uint8_t          *serial_bytes;
    size_t            serial_len;

    // RSA (n, d) big-endian byte slices into key_der->data.
    const uint8_t    *rsa_n;
    size_t            rsa_n_len;
    const uint8_t    *rsa_d;
    size_t            rsa_d_len;
};

// ---------------------------------------------------------------------------
// Accessors (consumed by resign_pe.c)
//
// Declared in the internal header below; ncc generates the per-
// translation-unit linkage we need. The accessor returns const
// pointers so the PE re-sign path cannot accidentally mutate the
// identity mid-call.
// ---------------------------------------------------------------------------

n00b_buffer_t *
_n00b_chalk_signer_identity_cert_der(n00b_chalk_signer_identity_t *id)
{
    return id ? id->cert_der : nullptr;
}

n00b_buffer_t *
_n00b_chalk_signer_identity_issuer_dn(n00b_chalk_signer_identity_t *id)
{
    return id ? id->issuer_dn_der : nullptr;
}

void
_n00b_chalk_signer_identity_serial(n00b_chalk_signer_identity_t *id,
                                   const uint8_t              **bytes,
                                   size_t                       *len)
{
    if (!id) {
        *bytes = nullptr;
        *len   = 0;
        return;
    }
    *bytes = id->serial_bytes;
    *len   = id->serial_len;
}

void
_n00b_chalk_signer_identity_rsa(n00b_chalk_signer_identity_t *id,
                                const uint8_t              **n_bytes,
                                size_t                       *n_len,
                                const uint8_t              **d_bytes,
                                size_t                       *d_len)
{
    if (!id) {
        *n_bytes = nullptr;
        *n_len   = 0;
        *d_bytes = nullptr;
        *d_len   = 0;
        return;
    }
    *n_bytes = id->rsa_n;
    *n_len   = id->rsa_n_len;
    *d_bytes = id->rsa_d;
    *d_len   = id->rsa_d_len;
}

// ---------------------------------------------------------------------------
// URI parsing — internal helpers
// ---------------------------------------------------------------------------

static bool
str_starts_with(n00b_string_t *s, const char *prefix)
{
    if (s == nullptr) return false;
    size_t plen = strlen(prefix);
    if (s->u8_bytes < plen) return false;
    return memcmp(s->data, prefix, plen) == 0;
}

// Returns the substring of s starting at offset (in bytes), with
// the given byte length. Both bounds are validated against
// s->u8_bytes; out-of-range returns nullptr.
static n00b_string_t *
substring(n00b_string_t *s, size_t off, size_t len, n00b_allocator_t *alloc)
{
    if (s == nullptr) return nullptr;
    if (off > s->u8_bytes) return nullptr;
    if (off + len > s->u8_bytes) return nullptr;
    return n00b_string_from_raw(s->data + off,
                                (int64_t)len,
                                .allocator = alloc);
}

// Find the byte offset of `c` in [start, end) (half-open) within
// s->data; returns SIZE_MAX if not found.
static size_t
find_char(n00b_string_t *s, char c, size_t start)
{
    if (s == nullptr) return SIZE_MAX;
    for (size_t i = start; i < s->u8_bytes; i++) {
        if (s->data[i] == c) return i;
    }
    return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// XDG store path resolution — mirrors src/attest/oci/auth.c's
// resolve_registries_json_path (D-052). Returns
// `<base>/<name>.cert.pem` or `<base>/<name>.key.pem` depending on
// `suffix`.
// ---------------------------------------------------------------------------

static n00b_string_t *
xdg_signing_identity_path(const char       *name,
                          size_t            name_len,
                          const char       *suffix,
                          n00b_allocator_t *alloc)
{
    // getenv() per D-052 (project-local libc exception for env-var
    // config discovery). Future libn00b n00b_getenv lift = DF-010.
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *base;
    const char *base_suffix;
    size_t      base_len;
    size_t      base_suffix_len;

    if (xdg != nullptr && xdg[0] != '\0') {
        base            = xdg;
        base_len        = strlen(xdg);
        base_suffix     = "/n00b-attest/signing-identities/";
        base_suffix_len = strlen(base_suffix);
    } else {
        const char *home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return nullptr;
        }
        base            = home;
        base_len        = strlen(home);
        base_suffix     = "/.config/n00b-attest/signing-identities/";
        base_suffix_len = strlen(base_suffix);
    }

    size_t suffix_len = strlen(suffix);
    size_t total_len  = base_len + base_suffix_len + name_len + suffix_len;
    char  *buf        = n00b_alloc_array_with_opts(
        char,
        total_len + 1,
        &(n00b_alloc_opts_t){.allocator = alloc});
    size_t off = 0;
    memcpy(buf + off, base, base_len);
    off += base_len;
    memcpy(buf + off, base_suffix, base_suffix_len);
    off += base_suffix_len;
    memcpy(buf + off, name, name_len);
    off += name_len;
    memcpy(buf + off, suffix, suffix_len);
    off += suffix_len;
    buf[total_len] = '\0';

    return n00b_string_from_raw(buf, (int64_t)total_len, .allocator = alloc);
}

// ---------------------------------------------------------------------------
// PEM load — uses picotls's ptls_load_pem_objects with a libc-
// allocated iovec the helper frees after copying into an
// allocator-owned buffer. The picotls memory-mgmt future-lift
// (D-039 part 1) will eventually thread the allocator through;
// for now we copy and free here, matching the precedent in
// src/attest/oci/file.c.
// ---------------------------------------------------------------------------

static n00b_buffer_t *
load_pem_object(n00b_string_t    *path,
                const char       *label,
                n00b_allocator_t *alloc)
{
    if (path == nullptr) return nullptr;

    ptls_iovec_t vec = {0};
    size_t       n   = 0;
    int rc = ptls_load_pem_objects((const char *)path->data, label, &vec, 1, &n);
    if (rc != 0 || n != 1 || vec.base == nullptr || vec.len == 0) {
        if (vec.base != nullptr) {
            free(vec.base);
        }
        return nullptr;
    }
    n00b_buffer_t *buf = n00b_buffer_from_bytes((char *)vec.base,
                                                (int64_t)vec.len,
                                                .allocator = alloc);
    free(vec.base);
    return buf;
}

// ---------------------------------------------------------------------------
// X.509 cert walker — extracts the tbsCertificate.issuer DER blob
// and the tbsCertificate.serialNumber raw INTEGER content bytes.
//
//   Certificate ::= SEQUENCE {
//       tbsCertificate       SEQUENCE {
//           version          [0] EXPLICIT Version DEFAULT v1,
//           serialNumber     CertificateSerialNumber,  -- INTEGER
//           signature        AlgorithmIdentifier,
//           issuer           Name,
//           validity         Validity,
//           subject          Name,
//           ...
//       },
//       ...
//   }
//
// Returns true on success; false if the parse walks off the end.
// ---------------------------------------------------------------------------

static bool
extract_cert_issuer_serial(const uint8_t  *der,
                           size_t          der_len,
                           const uint8_t **issuer_dn_start,
                           size_t         *issuer_dn_total_len,
                           const uint8_t **serial_bytes,
                           size_t         *serial_len)
{
    uint32_t length            = 0;
    int      indefinite_length = 0;
    size_t   last_byte         = 0;
    int      decode_error      = 0;
    size_t   idx;

    // Outer Certificate SEQUENCE.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, 0, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;

    // tbsCertificate SEQUENCE.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;

    // Optional version [0] EXPLICIT — tag 0xA0 (context-specific
    // constructed, [0]). If present, skip; if absent, the next
    // tag is the serialNumber INTEGER 0x02.
    if (idx < der_len && der[idx] == 0xA0) {
        size_t version_last = 0;
        idx = ptls_asn1_get_expected_type_and_length(
            der, der_len, idx, 0xA0,
            &length, &indefinite_length, &version_last, &decode_error, NULL);
        if (decode_error != 0) return false;
        idx = version_last;
    }

    // serialNumber INTEGER.
    size_t serial_content_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    *serial_bytes = der + serial_content_off;
    *serial_len   = (size_t)length;
    idx = last_byte;

    // signature AlgorithmIdentifier SEQUENCE — skip.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    idx = last_byte;

    // issuer Name SEQUENCE — capture the TLV (tag + length +
    // content) for use as the SignerInfo IssuerAndSerialNumber's
    // issuer field.
    size_t issuer_start = idx;
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    *issuer_dn_start     = der + issuer_start;
    *issuer_dn_total_len = last_byte - issuer_start;

    return true;
}

// ---------------------------------------------------------------------------
// PKCS#8 walker — extracts the inner RSAPrivateKey's (n, d) byte
// slices. Mirrors the extract_rsa_nd helper in
// test/unit/test_pkcs7_signed_data.c. Returns true on success.
// ---------------------------------------------------------------------------

static bool
extract_rsa_nd_from_pkcs8(const uint8_t  *der,
                          size_t          der_len,
                          const uint8_t **out_n,
                          size_t         *out_n_len,
                          const uint8_t **out_d,
                          size_t         *out_d_len)
{
    uint32_t length            = 0;
    int      indefinite_length = 0;
    size_t   last_byte         = 0;
    int      decode_error      = 0;
    size_t   idx;

    // Outer PKCS#8 SEQUENCE.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, 0, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;

    // version INTEGER 0.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    idx = last_byte;

    // privateKeyAlgorithm SEQUENCE — skip.
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    idx = last_byte;

    // privateKey OCTET STRING (contains the RSAPrivateKey SEQUENCE).
    idx = ptls_asn1_get_expected_type_and_length(
        der, der_len, idx, 0x04,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    size_t inner_off = idx;

    // RSAPrivateKey SEQUENCE.
    inner_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, inner_off, 0x30,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;

    // version INTEGER 0.
    inner_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, inner_off, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    inner_off = last_byte;

    // modulus n INTEGER.
    inner_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, inner_off, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    *out_n     = der + inner_off;
    *out_n_len = length;
    inner_off  = last_byte;

    // publicExponent e INTEGER — skip.
    inner_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, inner_off, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    inner_off = last_byte;

    // privateExponent d INTEGER.
    inner_off = ptls_asn1_get_expected_type_and_length(
        der, der_len, inner_off, 0x02,
        &length, &indefinite_length, &last_byte, &decode_error, NULL);
    if (decode_error != 0) return false;
    *out_d     = der + inner_off;
    *out_d_len = length;

    return true;
}

// ---------------------------------------------------------------------------
// Compose: cert PEM + key PEM → resolved identity handle.
// ---------------------------------------------------------------------------

static n00b_chalk_signer_identity_t *
build_identity_from_pem(n00b_string_t    *cert_path,
                        n00b_string_t    *key_path,
                        n00b_allocator_t *alloc,
                        int              *err_out)
{
    n00b_buffer_t *cert_der = load_pem_object(cert_path, "CERTIFICATE", alloc);
    if (cert_der == nullptr) {
        *err_out = N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND;
        return nullptr;
    }
    n00b_buffer_t *key_der = load_pem_object(key_path, "PRIVATE KEY", alloc);
    if (key_der == nullptr) {
        *err_out = N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND;
        return nullptr;
    }

    const uint8_t *issuer_dn_start    = nullptr;
    size_t         issuer_dn_total    = 0;
    const uint8_t *serial_bytes       = nullptr;
    size_t         serial_len         = 0;
    if (!extract_cert_issuer_serial((const uint8_t *)cert_der->data,
                                    cert_der->byte_len,
                                    &issuer_dn_start,
                                    &issuer_dn_total,
                                    &serial_bytes,
                                    &serial_len)) {
        *err_out = N00B_CHALK_ERR_KEY_PARSE_FAILED;
        return nullptr;
    }

    const uint8_t *n_bytes = nullptr;
    size_t         n_len   = 0;
    const uint8_t *d_bytes = nullptr;
    size_t         d_len   = 0;
    if (!extract_rsa_nd_from_pkcs8((const uint8_t *)key_der->data,
                                   key_der->byte_len,
                                   &n_bytes, &n_len,
                                   &d_bytes, &d_len)) {
        *err_out = N00B_CHALK_ERR_KEY_PARSE_FAILED;
        return nullptr;
    }

    n00b_chalk_signer_identity_t *id = n00b_alloc(
        n00b_chalk_signer_identity_t,
        .allocator = alloc);
    id->allocator = alloc;
    id->cert_der  = cert_der;
    id->key_der   = key_der;

    // Issuer DN — copy into an allocator-owned buffer so the
    // pointer is independent of the cert_der lifetime semantics
    // and can be passed verbatim to n00b_pkcs7_signed_data_add_signer.
    id->issuer_dn_der = n00b_buffer_from_bytes((char *)issuer_dn_start,
                                               (int64_t)issuer_dn_total,
                                               .allocator = alloc);

    // Serial — copy into allocator-owned bytes for the same reason.
    id->serial_bytes = n00b_alloc_array_with_opts(
        uint8_t,
        serial_len > 0 ? serial_len : 1,
        &(n00b_alloc_opts_t){.allocator = alloc});
    if (serial_len > 0) {
        memcpy(id->serial_bytes, serial_bytes, serial_len);
    }
    id->serial_len = serial_len;

    // RSA (n, d) — the byte slices point into key_der->data, which
    // is allocator-owned and lives as long as the handle. No copy.
    id->rsa_n     = n_bytes;
    id->rsa_n_len = n_len;
    id->rsa_d     = d_bytes;
    id->rsa_d_len = d_len;

    *err_out = 0;
    return id;
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

n00b_result_t(n00b_chalk_signer_identity_t *)
n00b_chalk_signer_identity_resolve(n00b_string_t *uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (uri == nullptr) {
        return n00b_result_ok(n00b_chalk_signer_identity_t *, nullptr);
    }

    // file://cert,file://key — paired form.
    if (str_starts_with(uri, "file://")) {
        size_t comma_off = find_char(uri, ',', 0);
        if (comma_off == SIZE_MAX) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        // The cert path is the substring between [7, comma_off);
        // the key path's "file://" prefix starts at comma_off+1.
        n00b_string_t *cert_path = substring(uri,
                                             7,
                                             comma_off - 7,
                                             allocator);
        if (cert_path == nullptr) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        // Require the second half to also start with file://.
        if (uri->u8_bytes < comma_off + 1 + 7) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        if (memcmp(uri->data + comma_off + 1, "file://", 7) != 0) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        n00b_string_t *key_path = substring(
            uri,
            comma_off + 1 + 7,
            uri->u8_bytes - (comma_off + 1 + 7),
            allocator);
        if (key_path == nullptr) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        int err = 0;
        n00b_chalk_signer_identity_t *id = build_identity_from_pem(
            cert_path, key_path, allocator, &err);
        if (id == nullptr) {
            return n00b_result_err(n00b_chalk_signer_identity_t *, err);
        }
        return n00b_result_ok(n00b_chalk_signer_identity_t *, id);
    }

    // store://<name>
    if (str_starts_with(uri, "store://")) {
        size_t name_off = 8;
        if (uri->u8_bytes <= name_off) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        size_t name_len = uri->u8_bytes - name_off;
        n00b_string_t *cert_path = xdg_signing_identity_path(
            uri->data + name_off, name_len, ".cert.pem", allocator);
        n00b_string_t *key_path = xdg_signing_identity_path(
            uri->data + name_off, name_len, ".key.pem", allocator);
        if (cert_path == nullptr || key_path == nullptr) {
            return n00b_result_err(n00b_chalk_signer_identity_t *,
                                   N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
        }
        int err = 0;
        n00b_chalk_signer_identity_t *id = build_identity_from_pem(
            cert_path, key_path, allocator, &err);
        if (id == nullptr) {
            return n00b_result_err(n00b_chalk_signer_identity_t *, err);
        }
        return n00b_result_ok(n00b_chalk_signer_identity_t *, id);
    }

    // Unrecognized scheme → NOT_FOUND.
    return n00b_result_err(n00b_chalk_signer_identity_t *,
                           N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND);
}

void
n00b_chalk_signer_identity_release(n00b_chalk_signer_identity_t *id)
{
    if (id == nullptr) return;

    // Scrub the PKCS#8 key DER (which contains the RSA private
    // exponent d). Both copies — the buffer's backing store and
    // the SERIAL bytes — are zeroed so a subsequent GC sweep or
    // arena reset leaves no residue. The (n, d) byte slices point
    // into key_der->data, so zeroing key_der->data is sufficient
    // for them.
    if (id->key_der != nullptr && id->key_der->data != nullptr
        && id->key_der->byte_len > 0) {
        memset(id->key_der->data, 0, id->key_der->byte_len);
        id->key_der->byte_len = 0;
    }
    if (id->serial_bytes != nullptr && id->serial_len > 0) {
        memset(id->serial_bytes, 0, id->serial_len);
    }
    id->rsa_n     = nullptr;
    id->rsa_n_len = 0;
    id->rsa_d     = nullptr;
    id->rsa_d_len = 0;
}
