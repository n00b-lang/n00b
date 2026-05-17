/* src/attest/dsse.c — DSSE envelope encode/decode body.
 *
 * Implements the surface declared in include/attest/n00b_attest_dsse.h:
 *   - n00b_attest_envelope_new
 *   - n00b_attest_envelope_set_payload    (BORROW semantics, D-030/W-3)
 *   - n00b_attest_envelope_serialize
 *   - n00b_attest_envelope_parse
 *   - n00b_attest_envelope_get_payload
 *   - n00b_attest_envelope_pae_bytes
 *
 * Per WP-001 scope: signatures[] is always emitted as []. The shape
 * matches the canonical DSSE per-entry layout `{keyid, sig}` (D-016)
 * so WP-002 can populate it without a header break.
 *
 * PAE byte string (DSSE v1 spec): `DSSEv1 SP <decimal-len(payloadType)>
 * SP <payloadType> SP <decimal-len(payload)> SP <payload>`, where
 * <payload> is the **unencoded** Statement bytes (NOT the base64-wrapped
 * form). The architecture text in docs/attest/02-architecture.md §5.2
 * agrees with this; no drift surfaced during implementation.
 *
 * Base64 (RFC 4648 with '=' padding) is implemented inline in this
 * file as a file-scope static. libn00b does not currently expose a
 * first-class base64 primitive (a grep over include/ + src/core + src/util
 * turns up nothing); libchalk's macos_wrap codec carries the same
 * inline implementation. This is flagged in the sub-agent return so
 * the orchestrator can decide whether to lift it into a shared
 * libn00b util in a later WP.
 */

#include <attest/n00b_attest.h>

#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "parsers/json.h"
#include "adt/result.h"

#include "internal/attest/json_util.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Module-domain error codes.
// ---------------------------------------------------------------------------

#define N00B_ATTEST_ERR_DSSE_BAD_INPUT       (-2001)
#define N00B_ATTEST_ERR_DSSE_NO_PAYLOAD      (-2002)
#define N00B_ATTEST_ERR_DSSE_BAD_JSON        (-2003)
#define N00B_ATTEST_ERR_DSSE_WRONG_TYPE      (-2004)
#define N00B_ATTEST_ERR_DSSE_BAD_BASE64      (-2005)

// ---------------------------------------------------------------------------
// DSSE constants.
// ---------------------------------------------------------------------------

static const char k_dsse_payload_type[] = "application/vnd.in-toto+json";
static const char k_dsse_pae_prefix[]   = "DSSEv1";

// ---------------------------------------------------------------------------
// Private type.
//
// The envelope holds a borrowed pointer to the payload buffer per D-030
// W-3 (no copy, no allocator threading on set_payload).
// ---------------------------------------------------------------------------

struct n00b_attest_envelope {
    n00b_buffer_t    *payload;    // borrowed; lifetime managed by caller
    n00b_allocator_t *allocator;  // for everything the envelope itself produces
};

// ---------------------------------------------------------------------------
// Base64 (RFC 4648, '=' padded).
// ---------------------------------------------------------------------------

static const char k_b64_alphabet[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

static n00b_string_t *
base64_encode(const uint8_t   *data,
              size_t           len,
              n00b_allocator_t *allocator)
{
    size_t triples = len / 3;
    size_t rem     = len - triples * 3;
    size_t outlen  = triples * 4 + (rem ? 4 : 0);

    char *out = n00b_alloc_array_with_opts(char,
                                           outlen,
                                           &(n00b_alloc_opts_t){
                                               .allocator = allocator,
                                           });
    size_t op = 0;
    size_t ip = 0;
    for (size_t i = 0; i < triples; i++) {
        uint32_t v = ((uint32_t)data[ip]     << 16)
                   | ((uint32_t)data[ip + 1] << 8)
                   | (uint32_t)data[ip + 2];
        ip += 3;
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 6)  & 0x3f];
        out[op++] = k_b64_alphabet[v & 0x3f];
    }
    if (rem == 1) {
        uint32_t v = (uint32_t)data[ip] << 16;
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = '=';
        out[op++] = '=';
    }
    else if (rem == 2) {
        uint32_t v = ((uint32_t)data[ip]     << 16)
                   | ((uint32_t)data[ip + 1] << 8);
        out[op++] = k_b64_alphabet[(v >> 18) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 12) & 0x3f];
        out[op++] = k_b64_alphabet[(v >> 6)  & 0x3f];
        out[op++] = '=';
    }
    return n00b_string_from_raw(out,
                                (int64_t)outlen,
                                .allocator = allocator);
}

static int
b64_dec_char(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static n00b_buffer_t *
base64_decode(const char       *src,
              size_t            len,
              n00b_allocator_t *allocator)
{
    // Trim trailing whitespace.
    while (len > 0 && (src[len - 1] == '\n' || src[len - 1] == '\r'
                       || src[len - 1] == ' '  || src[len - 1] == '\t')) {
        len--;
    }
    if ((len & 3) != 0) {
        return nullptr;
    }
    if (len == 0) {
        return n00b_buffer_from_bytes(nullptr,
                                      0,
                                      .allocator = allocator);
    }

    size_t pad = 0;
    if (src[len - 1] == '=') pad++;
    if (len > 1 && src[len - 2] == '=') pad++;
    size_t out_len = (len / 4) * 3 - pad;
    uint8_t *out = (uint8_t *)n00b_alloc_array_with_opts(
        char,
        out_len,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    size_t op = 0;
    for (size_t i = 0; i < len; i += 4) {
        int a = b64_dec_char(src[i]);
        int b = b64_dec_char(src[i + 1]);
        int c = src[i + 2] == '=' ? 0 : b64_dec_char(src[i + 2]);
        int d = src[i + 3] == '=' ? 0 : b64_dec_char(src[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return nullptr;
        }
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12)
                   | ((uint32_t)c << 6)  | (uint32_t)d;
        if (op < out_len) out[op++] = (uint8_t)((v >> 16) & 0xff);
        if (op < out_len) out[op++] = (uint8_t)((v >> 8) & 0xff);
        if (op < out_len) out[op++] = (uint8_t)(v & 0xff);
    }
    return n00b_buffer_from_bytes((char *)out,
                                  (int64_t)out_len,
                                  .allocator = allocator);
}

// ---------------------------------------------------------------------------
// Decimal-ASCII helpers (PAE length fields).
// ---------------------------------------------------------------------------

static size_t
dec_ascii_len(size_t v)
{
    size_t n = 1;
    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

static void
dec_ascii_write(char *dst, size_t v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        dst[n - 1 - i] = (char)('0' + (v % 10));
        v /= 10;
    }
}

// ---------------------------------------------------------------------------
// Public surface.
// ---------------------------------------------------------------------------

n00b_attest_envelope_t *
n00b_attest_envelope_new() _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_attest_envelope_t *env = n00b_alloc_with_opts(
        n00b_attest_envelope_t,
        &(n00b_alloc_opts_t){
            .allocator = allocator,
        });
    env->payload   = nullptr;
    env->allocator = allocator;
    return env;
}

// D-030 / W-3: BORROW semantics. The body stores the pointer and
// allocates nothing — no allocator parameter (the declaration has no
// _kargs block). The caller's contract per the doxygen: keep
// `payload`'s bytes valid until _serialize runs.
n00b_result_t(bool)
n00b_attest_envelope_set_payload(n00b_attest_envelope_t *env,
                                 n00b_buffer_t          *payload)
{
    if (env == nullptr || payload == nullptr || payload->byte_len == 0) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    env->payload = payload;
    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_serialize(n00b_attest_envelope_t *env) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (env == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (env->payload == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_NO_PAYLOAD);
    }

    n00b_allocator_t *alloc_for_call = allocator ? allocator : env->allocator;

    n00b_string_t *b64 = base64_encode((const uint8_t *)env->payload->data,
                                       env->payload->byte_len,
                                       alloc_for_call);

    // NUL-terminate the base64 string for n00b_json_string_new.
    char *b64_cstr = n00b_alloc_array_with_opts(
        char,
        b64->u8_bytes + 1,
        &(n00b_alloc_opts_t){
            .allocator = alloc_for_call,
        });
    memcpy(b64_cstr, b64->data, b64->u8_bytes);
    b64_cstr[b64->u8_bytes] = '\0';

    n00b_json_node_t *root = n00b_json_object_new();
    n00b_json_object_put(root,
                         "payloadType",
                         n00b_json_string_new(k_dsse_payload_type));
    n00b_json_object_put(root,
                         "payload",
                         n00b_json_string_new(b64_cstr));
    // WP-001: signatures is always emitted as []. The per-entry shape
    // (keyid + sig opaque strings, D-016) lands in WP-002.
    n00b_json_object_put(root, "signatures", n00b_json_array_new());

    char *encoded = n00b_json_encode(root, .pretty = false);
    if (encoded == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_JSON);
    }
    n00b_buffer_t *out = n00b_buffer_from_bytes(encoded,
                                                (int64_t)strlen(encoded),
                                                .allocator = alloc_for_call);
    return n00b_result_ok(n00b_buffer_t *, out);
}

// JSON-object key lookup lives in the shared internal helper:
// `n00b_attest_json_obj_lookup` (declared in
// `include/internal/attest/json_util.h`, defined in
// `src/attest/json_util.c`). Used here and by `statement.c`.

n00b_result_t(n00b_attest_envelope_t *)
n00b_attest_envelope_parse(n00b_buffer_t *bytes) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (bytes == nullptr || bytes->byte_len == 0) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }

    const char       *err  = nullptr;
    n00b_json_node_t *root = n00b_json_parse(bytes->data,
                                              bytes->byte_len,
                                              &err);
    if (root == nullptr || root->type != N00B_JSON_OBJECT) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_JSON);
    }

    n00b_json_node_t *pt_node      = n00b_attest_json_obj_lookup(root, r"payloadType");
    n00b_json_node_t *payload_node = n00b_attest_json_obj_lookup(root, r"payload");
    if (pt_node == nullptr || pt_node->type != N00B_JSON_STRING
        || payload_node == nullptr || payload_node->type != N00B_JSON_STRING) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_JSON);
    }
    if (strcmp(pt_node->string, k_dsse_payload_type) != 0) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_WRONG_TYPE);
    }

    n00b_attest_envelope_t *env = n00b_attest_envelope_new(
        .allocator = allocator);

    n00b_buffer_t *decoded = base64_decode(payload_node->string,
                                           strlen(payload_node->string),
                                           env->allocator);
    if (decoded == nullptr) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_BASE64);
    }

    // The decoded payload is owned by `allocator` (per the @kw on
    // _envelope_parse). Storing it as the envelope's borrowed payload
    // is correct — `decoded` lives in the same allocator scope as the
    // envelope.
    env->payload = decoded;
    return n00b_result_ok(n00b_attest_envelope_t *, env);
}

n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_get_payload(n00b_attest_envelope_t *env)
{
    if (env == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (env->payload == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_NO_PAYLOAD);
    }
    return n00b_result_ok(n00b_buffer_t *, env->payload);
}

n00b_result_t(n00b_buffer_t *)
n00b_attest_envelope_pae_bytes(n00b_attest_envelope_t *env) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (env == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (env->payload == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_NO_PAYLOAD);
    }

    n00b_allocator_t *alloc_for_call = allocator ? allocator : env->allocator;

    // PAE = "DSSEv1" SP <len(type)> SP <type> SP <len(payload)> SP <payload>
    size_t prefix_len  = sizeof(k_dsse_pae_prefix) - 1;       // 6
    size_t type_len    = sizeof(k_dsse_payload_type) - 1;     // 28
    size_t type_lenlen = dec_ascii_len(type_len);
    size_t payload_len = env->payload->byte_len;
    size_t payload_lenlen = dec_ascii_len(payload_len);

    // 4 separator spaces between the 5 fields.
    size_t total = prefix_len + 1 + type_lenlen + 1 + type_len + 1
                 + payload_lenlen + 1 + payload_len;

    char *buf = n00b_alloc_array_with_opts(
        char,
        total,
        &(n00b_alloc_opts_t){
            .allocator = alloc_for_call,
        });
    size_t op = 0;

    memcpy(buf + op, k_dsse_pae_prefix, prefix_len);
    op += prefix_len;
    buf[op++] = ' ';
    dec_ascii_write(buf + op, type_len, type_lenlen);
    op += type_lenlen;
    buf[op++] = ' ';
    memcpy(buf + op, k_dsse_payload_type, type_len);
    op += type_len;
    buf[op++] = ' ';
    dec_ascii_write(buf + op, payload_len, payload_lenlen);
    op += payload_lenlen;
    buf[op++] = ' ';
    if (payload_len > 0) {
        memcpy(buf + op, env->payload->data, payload_len);
        op += payload_len;
    }
    (void)op;

    n00b_buffer_t *out = n00b_buffer_from_bytes(buf,
                                                (int64_t)total,
                                                .allocator = alloc_for_call);
    return n00b_result_ok(n00b_buffer_t *, out);
}
