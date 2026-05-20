/* src/attest/oci/manifest.c — OCI 1.1 artifact-manifest serializer
 * + SHA-256-of-buffer helper.
 *
 * Implements:
 *   - n00b_attest_oci_digest_of_buffer
 *   - n00b_attest_oci_manifest_build
 *
 * Phase 2 of WP-004 lands the producer-side substrate the push
 * verb shim sits on. The manifest serializer's output is the
 * cross-tool interop surface — cosign / sigstore-python /
 * sigstore-rs read this manifest later — so the bytes must be
 * byte-stable per D-024 (canonical wire JSON `.pretty = false`)
 * AND emit fields in the fixed spec §8.2 order.
 *
 * # Field-order discipline (load-bearing)
 *
 * `n00b_dict_t` iteration order is not guaranteed; we cannot use
 * a dict-iter encoder to produce a byte-stable manifest. The
 * serializer instead builds the JSON via ordered string
 * concatenation, with explicit JSON escaping (RFC 8259 § 7) on
 * every string-valued field whose contents are caller-supplied.
 * The fixed field order (matching docs/attest/02-architecture.md
 * §8.2):
 *
 *   schemaVersion
 *   mediaType
 *   artifactType
 *   config
 *     mediaType
 *     digest
 *     size
 *   layers[0]
 *     mediaType
 *     digest
 *     size
 *   subject
 *     mediaType
 *     digest
 *     size
 *   annotations
 *     com.crashoverride.attestation.predicate-type
 *     com.crashoverride.attestation.signer-keyid
 *
 * The empty-config-blob digest is the SHA-256 of the literal two-
 * byte JSON value `{}` (the canonical empty descriptor per OCI
 * 1.1). It is a constant — same for every artifact manifest in
 * the OCI ecosystem.
 *
 * # SHA-256 via libn00b
 *
 * `n00b_attest_oci_digest_of_buffer` wraps libn00b's
 * `n00b_sha256_hash` from `core/sha256.h` — the same primitive
 * the signer backend's SPKI-keyid derivation uses (per WP-002
 * D-039 / src/attest/backends/file.c). The digest words are in
 * host byte order; we emit them big-endian per SHA-256's
 * canonical byte view (matching every other SHA-256
 * implementation against the same input).
 *
 * D-045 `alloc_for_call` idiom: every allocating function threads
 * one `alloc_for_call` local through every downstream allocation.
 *
 * D-016 algorithm-agnostic: the OCI substrate ferries opaque
 * bytes; no Ed25519 / ECDSA / algorithm-tag symbols appear here.
 */

#include "internal/attest/oci/registry.h"
#include <attest/n00b_attest_oci.h>
#include <attest/n00b_attest_error.h>

#include "core/string.h"
#include "core/buffer.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/sha256.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Constants — OCI media types + annotation keys + spec-§8.2 boilerplate.
// ---------------------------------------------------------------------------

static const char k_media_manifest[] =
    "application/vnd.oci.image.manifest.v1+json";
static const char k_artifact_type[] =
    "application/vnd.in-toto+dsse";
static const char k_layer_media[] =
    "application/vnd.in-toto+json";

// The empty-config descriptor per OCI 1.1 — canonical SHA-256 of
// the two-byte JSON value `{}` is well-known across the OCI
// ecosystem (cosign / oras / sigstore-rs all emit this exact
// shape). We compute it lazily in the serializer instead of
// pre-baking the hex literal so the digest construction is
// audit-trail-visible from the implementation itself.
static const char k_empty_config_bytes[] = "{}";

// Annotation keys per docs/attest/02-architecture.md §8.2.
static const char k_annotation_predicate_type[] =
    "com.crashoverride.attestation.predicate-type";
static const char k_annotation_signer_keyid[] =
    "com.crashoverride.attestation.signer-keyid";

static const char k_hex_lower[] = "0123456789abcdef";

// ---------------------------------------------------------------------------
// Helpers — buffer construction + SHA-256 + JSON escape.
// ---------------------------------------------------------------------------

// Compute `sha256:<lowercase-hex>` of `data[0..len)` into a freshly-
// allocated string owned by `alloc_for_call`.
static n00b_string_t *
sha256_prefixed_hex(const uint8_t   *data,
                    size_t           len,
                    n00b_allocator_t *alloc_for_call)
{
    n00b_sha256_digest_t digest;
    n00b_sha256_hash(data, len, digest);

    // Spell out the byte view per SHA-256's big-endian word
    // convention so the digest matches what every other SHA-256
    // implementation produces against the same input.
    uint8_t bytes[32];
    for (int i = 0; i < 8; i++) {
        uint32_t w = digest[i];
        bytes[i * 4 + 0] = (uint8_t)((w >> 24) & 0xff);
        bytes[i * 4 + 1] = (uint8_t)((w >> 16) & 0xff);
        bytes[i * 4 + 2] = (uint8_t)((w >> 8) & 0xff);
        bytes[i * 4 + 3] = (uint8_t)(w & 0xff);
    }

    // 7-byte prefix `sha256:` + 64-byte hex = 71 bytes.
    char *out = n00b_alloc_array_with_opts(
        char,
        72,
        &(n00b_alloc_opts_t){.allocator = alloc_for_call});
    memcpy(out, "sha256:", 7);
    for (size_t i = 0; i < 32; i++) {
        out[7 + i * 2]     = k_hex_lower[(bytes[i] >> 4) & 0xf];
        out[7 + i * 2 + 1] = k_hex_lower[bytes[i] & 0xf];
    }
    out[71] = '\0';
    return n00b_string_from_raw(out,
                                (int64_t)71,
                                .allocator = alloc_for_call);
}

// Compute the size of `s` after RFC 8259 § 7 JSON-string escaping
// (without the surrounding quotes). Returns the byte count.
static size_t
json_escaped_size(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            n += 2;
            break;
        default:
            if (c < 0x20) {
                n += 6;  // \uXXXX
            }
            else {
                n += 1;
            }
            break;
        }
    }
    return n;
}

// Write JSON-string-escaped `s` (no surrounding quotes) into
// `out`, returning the byte count written.
static size_t
json_escape_into(char *out, const char *s, size_t len)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            out[o++] = '\\';
            out[o++] = '"';
            break;
        case '\\':
            out[o++] = '\\';
            out[o++] = '\\';
            break;
        case '\b':
            out[o++] = '\\';
            out[o++] = 'b';
            break;
        case '\f':
            out[o++] = '\\';
            out[o++] = 'f';
            break;
        case '\n':
            out[o++] = '\\';
            out[o++] = 'n';
            break;
        case '\r':
            out[o++] = '\\';
            out[o++] = 'r';
            break;
        case '\t':
            out[o++] = '\\';
            out[o++] = 't';
            break;
        default:
            if (c < 0x20) {
                out[o++] = '\\';
                out[o++] = 'u';
                out[o++] = '0';
                out[o++] = '0';
                out[o++] = k_hex_lower[(c >> 4) & 0xf];
                out[o++] = k_hex_lower[c & 0xf];
            }
            else {
                out[o++] = (char)c;
            }
            break;
        }
    }
    return o;
}

// Decimal ASCII encoding of `v`. Returns the byte count.
static size_t
u64_to_dec(char *out, uint64_t v)
{
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[24];
    size_t n = 0;
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    // reverse.
    for (size_t i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

// ---------------------------------------------------------------------------
// Public-internal surface — buffer-digest helper.
// ---------------------------------------------------------------------------

n00b_result_t(n00b_string_t *)
n00b_attest_oci_digest_of_buffer(n00b_buffer_t *buf)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    n00b_allocator_t *alloc_for_call = allocator;

    if (buf == nullptr) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *digest = sha256_prefixed_hex(
        (const uint8_t *)buf->data,
        (size_t)buf->byte_len,
        alloc_for_call);
    return n00b_result_ok(n00b_string_t *, digest);
}

// ---------------------------------------------------------------------------
// Public-internal surface — manifest serializer.
// ---------------------------------------------------------------------------
//
// The output is a single contiguous byte buffer assembled by
// appending pre-sized fragments into a growing char* with a
// known total-length-upper-bound. We compute the upper bound by
// summing the literal-text bytes + each user-string's escaped
// size + each integer's decimal width; this avoids the need to
// resize mid-build.

n00b_result_t(n00b_buffer_t *)
n00b_attest_oci_manifest_build(n00b_string_t *image_digest,
                               uint64_t       image_manifest_size,
                               n00b_string_t *envelope_digest,
                               uint64_t       envelope_size,
                               n00b_string_t *predicate_type,
                               n00b_string_t *signer_keyid)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    }
{
    n00b_allocator_t *alloc_for_call = allocator;

    if (image_digest == nullptr || image_digest->u8_bytes == 0
        || envelope_digest == nullptr || envelope_digest->u8_bytes == 0
        || predicate_type == nullptr || predicate_type->u8_bytes == 0
        || signer_keyid == nullptr || signer_keyid->u8_bytes == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    // The empty-config descriptor's digest is sha256("{}"), 2 bytes.
    n00b_string_t *empty_cfg_digest = sha256_prefixed_hex(
        (const uint8_t *)k_empty_config_bytes,
        2,
        alloc_for_call);

    // Pre-compute escaped sizes for every user-supplied string.
    size_t escaped_image_digest = json_escaped_size(
        image_digest->data, image_digest->u8_bytes);
    size_t escaped_envelope_digest = json_escaped_size(
        envelope_digest->data, envelope_digest->u8_bytes);
    size_t escaped_predicate_type = json_escaped_size(
        predicate_type->data, predicate_type->u8_bytes);
    size_t escaped_signer_keyid = json_escaped_size(
        signer_keyid->data, signer_keyid->u8_bytes);
    size_t escaped_empty_cfg_digest = empty_cfg_digest->u8_bytes;  // ASCII-safe

    // Decimal widths for sizes.
    char    image_size_buf[24];
    size_t  image_size_w = u64_to_dec(image_size_buf, image_manifest_size);
    char    envelope_size_buf[24];
    size_t  envelope_size_w = u64_to_dec(envelope_size_buf, envelope_size);

    // Compute upper-bound output size. Each literal text fragment
    // below is counted via its source length. The total is exact
    // for the all-printable-ASCII case (which is the realistic
    // shape for digests / OCI media types) and only an upper bound
    // when an escape expansion fires; we allocate the upper bound
    // then trim the buffer length at the end.
    size_t cap = 0;
    cap += sizeof("{") - 1;
    cap += sizeof("\"schemaVersion\":2,") - 1;
    cap += sizeof("\"mediaType\":\"") - 1 + sizeof(k_media_manifest) - 1
           + sizeof("\",") - 1;
    cap += sizeof("\"artifactType\":\"") - 1 + sizeof(k_artifact_type) - 1
           + sizeof("\",") - 1;
    cap += sizeof("\"config\":{") - 1;
    cap += sizeof("\"mediaType\":\"application/vnd.oci.empty.v1+json\",") - 1;
    cap += sizeof("\"digest\":\"") - 1 + escaped_empty_cfg_digest
           + sizeof("\",") - 1;
    cap += sizeof("\"size\":2") - 1;
    cap += sizeof("},") - 1;
    cap += sizeof("\"layers\":[{") - 1;
    cap += sizeof("\"mediaType\":\"") - 1 + sizeof(k_layer_media) - 1
           + sizeof("\",") - 1;
    cap += sizeof("\"digest\":\"") - 1 + escaped_envelope_digest
           + sizeof("\",") - 1;
    cap += sizeof("\"size\":") - 1 + envelope_size_w;
    cap += sizeof("}],") - 1;
    cap += sizeof("\"subject\":{") - 1;
    cap += sizeof("\"mediaType\":\"") - 1 + sizeof(k_media_manifest) - 1
           + sizeof("\",") - 1;
    cap += sizeof("\"digest\":\"") - 1 + escaped_image_digest
           + sizeof("\",") - 1;
    cap += sizeof("\"size\":") - 1 + image_size_w;
    cap += sizeof("},") - 1;
    cap += sizeof("\"annotations\":{") - 1;
    cap += sizeof("\"") - 1 + sizeof(k_annotation_predicate_type) - 1
           + sizeof("\":\"") - 1 + escaped_predicate_type
           + sizeof("\",") - 1;
    cap += sizeof("\"") - 1 + sizeof(k_annotation_signer_keyid) - 1
           + sizeof("\":\"") - 1 + escaped_signer_keyid
           + sizeof("\"") - 1;
    cap += sizeof("}}") - 1;
    cap += 1;  // trailing NUL

    char *out = n00b_alloc_array_with_opts(
        char,
        cap,
        &(n00b_alloc_opts_t){.allocator = alloc_for_call});
    size_t o = 0;

    // Helper macro — append a string literal (computes length via
    // sizeof so we cannot miscount by hand). Inline rather than a
    // separate function so the audit reviewer's eye traces the
    // exact byte order being assembled.
#define APPEND_LIT(s) do {                                              \
        memcpy(out + o, (s), sizeof(s) - 1);                            \
        o += sizeof(s) - 1;                                             \
    } while (0)
#define APPEND_RAW(p, n) do {                                           \
        memcpy(out + o, (p), (n));                                      \
        o += (n);                                                       \
    } while (0)

    // schemaVersion + mediaType + artifactType.
    APPEND_LIT("{\"schemaVersion\":2,\"mediaType\":\"");
    APPEND_RAW(k_media_manifest, sizeof(k_media_manifest) - 1);
    APPEND_LIT("\",\"artifactType\":\"");
    APPEND_RAW(k_artifact_type, sizeof(k_artifact_type) - 1);

    // config (empty descriptor).
    APPEND_LIT("\",\"config\":{\"mediaType\":"
               "\"application/vnd.oci.empty.v1+json\",\"digest\":\"");
    APPEND_RAW(empty_cfg_digest->data, escaped_empty_cfg_digest);
    APPEND_LIT("\",\"size\":2},\"layers\":[{\"mediaType\":\"");

    // layers[0].
    APPEND_RAW(k_layer_media, sizeof(k_layer_media) - 1);
    APPEND_LIT("\",\"digest\":\"");
    o += json_escape_into(out + o,
                          envelope_digest->data,
                          envelope_digest->u8_bytes);
    APPEND_LIT("\",\"size\":");
    APPEND_RAW(envelope_size_buf, envelope_size_w);
    APPEND_LIT("}],\"subject\":{\"mediaType\":\"");

    // subject.
    APPEND_RAW(k_media_manifest, sizeof(k_media_manifest) - 1);
    APPEND_LIT("\",\"digest\":\"");
    o += json_escape_into(out + o,
                          image_digest->data,
                          image_digest->u8_bytes);
    APPEND_LIT("\",\"size\":");
    APPEND_RAW(image_size_buf, image_size_w);
    APPEND_LIT("},\"annotations\":{\"");

    // annotations.predicate-type.
    APPEND_RAW(k_annotation_predicate_type,
               sizeof(k_annotation_predicate_type) - 1);
    APPEND_LIT("\":\"");
    o += json_escape_into(out + o,
                          predicate_type->data,
                          predicate_type->u8_bytes);
    APPEND_LIT("\",\"");

    // annotations.signer-keyid.
    APPEND_RAW(k_annotation_signer_keyid,
               sizeof(k_annotation_signer_keyid) - 1);
    APPEND_LIT("\":\"");
    o += json_escape_into(out + o,
                          signer_keyid->data,
                          signer_keyid->u8_bytes);
    APPEND_LIT("\"}}");

#undef APPEND_LIT
#undef APPEND_RAW

    n00b_buffer_t *buf = n00b_buffer_from_bytes(out,
                                                (int64_t)o,
                                                .allocator = alloc_for_call);
    return n00b_result_ok(n00b_buffer_t *, buf);
}
