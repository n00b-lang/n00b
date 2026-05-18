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
 * Base64 (RFC 4648 with '=' padding) is delegated to the
 * libn00b util `n00b_base64_*` (include/util/base64.h, lifted in
 * WP-002 Phase 1; thin wrapper around picotls's `ptls_base64_*`
 * primitives). The dsse.c-local inline encoder/decoder removed in
 * that same lift — libchalk's `src/chalk/macos_wrap.c` now consumes
 * the same util.
 */

#include <attest/n00b_attest.h>
#include <util/base64.h>

#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "parsers/json.h"
#include "adt/result.h"

#include "internal/attest/json_util.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Module-domain error codes are now declared in
// `include/attest/n00b_attest_error.h` (V-2 fix during WP-002 Phase 3 — see
// D-031 A-1 closure). The umbrella header pulls them in.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DSSE constants.
// ---------------------------------------------------------------------------

static const char k_dsse_payload_type[] = "application/vnd.in-toto+json";
static const char k_dsse_pae_prefix[]   = "DSSEv1";

// ---------------------------------------------------------------------------
// Private type.
//
// The envelope holds a borrowed pointer to the payload buffer per D-030
// W-3 (no copy, no allocator threading on set_payload), plus a parallel
// pair of lists carrying the (keyid, sig) signature entries appended via
// `n00b_attest_envelope_add_signature` / `n00b_attest_envelope_sign`. The
// per-entry shape (D-016) is `{ "keyid": "<hex>", "sig": "<base64>" }` —
// algorithm-agnostic; the envelope code does not interpret either field.
//
// We use parallel lists rather than a struct array because the existing
// `subject_entry_t` precedent in `statement.c` uses the same shape and the
// number of signatures per envelope is small (typically 1 — the multi-
// signer flow FR-11 is deferred). No header break.
// ---------------------------------------------------------------------------

struct n00b_attest_envelope {
    n00b_buffer_t    *payload;     // borrowed; lifetime managed by caller
    n00b_allocator_t *allocator;   // for everything the envelope itself produces

    // signature_keyids[i] paired with signature_sigs[i]; both lists grow
    // together via `n00b_attest_envelope_add_signature`. Stored as private
    // copies — the caller may free the inputs immediately after the call.
    n00b_list_t(n00b_string_t *) signature_keyids;
    n00b_list_t(n00b_buffer_t *) signature_sigs;
    bool signatures_initialized;
};

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

// Lazy-initialize the signature lists on first append. We don't
// build them in `_envelope_new` because every envelope produced
// pre-Phase-3 went without — the lazy-init keeps the construct-
// then-discard fast path (the existing roundtrip test pattern)
// unchanged while making the lists available the moment a
// signature is appended.
static void
ensure_signatures_initialized(n00b_attest_envelope_t *env,
                              n00b_allocator_t       *alloc_for_call)
{
    if (env->signatures_initialized) {
        return;
    }
    env->signature_keyids = n00b_list_new(n00b_string_t *,
                                          .allocator = alloc_for_call);
    env->signature_sigs   = n00b_list_new(n00b_buffer_t *,
                                          .allocator = alloc_for_call);
    env->signatures_initialized = true;
}

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
    env->payload                = nullptr;
    env->allocator              = allocator;
    env->signatures_initialized = false;
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

    auto enc_r = n00b_base64_encode(env->payload,
                                    .allocator = alloc_for_call);
    if (n00b_result_is_err(enc_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    n00b_string_t *b64 = n00b_result_get(enc_r);

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

    // WP-002 Phase 3: emit each appended `{keyid, sig}` pair into the
    // `signatures[]` array. The per-entry shape (D-016) is
    // `{ "keyid": "<hex>", "sig": "<base64>" }` — algorithm-agnostic.
    // If no signatures were appended the array is emitted empty
    // (matching the WP-001 behavior for envelopes that go through
    // serialize without signing — e.g., the existing roundtrip test).
    n00b_json_node_t *sigs_arr = n00b_json_array_new();
    if (env->signatures_initialized) {
        size_t nsigs = env->signature_keyids.len;
        for (size_t i = 0; i < nsigs; i++) {
            n00b_string_t *kid = env->signature_keyids.data[i];
            n00b_buffer_t *sig = env->signature_sigs.data[i];

            // Base64-encode the sig bytes per DSSE wire shape.
            auto sig_enc_r = n00b_base64_encode(sig,
                                                .allocator = alloc_for_call);
            if (n00b_result_is_err(sig_enc_r)) {
                return n00b_result_err(n00b_buffer_t *,
                                       N00B_ATTEST_ERR_DSSE_BAD_INPUT);
            }
            n00b_string_t *sig_b64 = n00b_result_get(sig_enc_r);

            // NUL-terminate keyid + sig_b64 so the JSON helpers can
            // ingest them (`n00b_json_string_new` takes `const char *`).
            char *kid_cstr = n00b_alloc_array_with_opts(
                char,
                kid->u8_bytes + 1,
                &(n00b_alloc_opts_t){.allocator = alloc_for_call});
            memcpy(kid_cstr, kid->data, kid->u8_bytes);
            kid_cstr[kid->u8_bytes] = '\0';

            char *sig_b64_cstr = n00b_alloc_array_with_opts(
                char,
                sig_b64->u8_bytes + 1,
                &(n00b_alloc_opts_t){.allocator = alloc_for_call});
            memcpy(sig_b64_cstr, sig_b64->data, sig_b64->u8_bytes);
            sig_b64_cstr[sig_b64->u8_bytes] = '\0';

            n00b_json_node_t *entry = n00b_json_object_new();
            n00b_json_object_put(entry,
                                 "keyid",
                                 n00b_json_string_new(kid_cstr));
            n00b_json_object_put(entry,
                                 "sig",
                                 n00b_json_string_new(sig_b64_cstr));
            n00b_json_array_push(sigs_arr, entry);
        }
    }
    n00b_json_object_put(root, "signatures", sigs_arr);

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

    // `payload_node->string` is a NUL-terminated `char *` from the
    // JSON parser. Wrap it as an n00b_string_t for the base64 util.
    n00b_string_t *b64_in = n00b_string_from_cstr(payload_node->string,
                                                  .allocator = env->allocator);
    auto dec_r = n00b_base64_decode(b64_in,
                                    .allocator = env->allocator);
    if (n00b_result_is_err(dec_r)) {
        return n00b_result_err(n00b_attest_envelope_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_BASE64);
    }
    n00b_buffer_t *decoded = n00b_result_get(dec_r);

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

// ---------------------------------------------------------------------------
// Envelope signature population (Phase 3).
//
// `_add_signature` is the low-level entry point: appends a pre-computed
// `{keyid, sig}` pair to the envelope. Stores PRIVATE COPIES of both
// inputs — the caller may free `keyid` and `sig` immediately after the
// call. `_sign` is the high-level composition: it computes the PAE
// bytes, runs the signer over them, fetches the cached keyid from the
// signer, and dispatches the pair through `_add_signature`.
//
// Errors are surfaced through the same `N00B_ATTEST_ERR_*` namespace as
// the rest of the module. The signer's `N00B_ATTEST_ERR_SIGN_FAILED`
// propagates verbatim when sign fails.
// ---------------------------------------------------------------------------

n00b_result_t(bool)
n00b_attest_envelope_add_signature(n00b_attest_envelope_t *env,
                                   n00b_string_t          *keyid,
                                   n00b_buffer_t          *sig) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (env == nullptr || keyid == nullptr || sig == nullptr) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (keyid->u8_bytes == 0 || sig->byte_len == 0) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }

    n00b_allocator_t *alloc_for_call = allocator ? allocator : env->allocator;

    ensure_signatures_initialized(env, alloc_for_call);

    // Store private copies — the caller may free `keyid` / `sig`
    // immediately after this call (per the header's doxygen). The
    // copies are owned by the envelope's effective allocator.
    n00b_string_t *kid_copy = n00b_string_from_raw(keyid->data,
                                                   (int64_t)keyid->u8_bytes,
                                                   .allocator = alloc_for_call);
    n00b_buffer_t *sig_copy = n00b_buffer_from_bytes(sig->data,
                                                     (int64_t)sig->byte_len,
                                                     .allocator = alloc_for_call);

    n00b_list_push(env->signature_keyids, kid_copy);
    n00b_list_push(env->signature_sigs, sig_copy);
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_attest_envelope_sign(n00b_attest_envelope_t *env,
                          n00b_attest_signer_t   *signer) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (env == nullptr || signer == nullptr) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (env->payload == nullptr) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_NO_PAYLOAD);
    }

    n00b_allocator_t *alloc_for_call = allocator ? allocator : env->allocator;

    // PAE the envelope. Failures propagate via the envelope-domain
    // error namespace (e.g., NO_PAYLOAD if the caller forgot
    // _set_payload — but we already checked above).
    auto pae_r = n00b_attest_envelope_pae_bytes(env,
                                                .allocator = alloc_for_call);
    if (n00b_result_is_err(pae_r)) {
        return n00b_result_err(bool, n00b_result_get_err(pae_r));
    }
    n00b_buffer_t *pae = n00b_result_get(pae_r);

    // Sign the PAE bytes. Failures propagate through the signer-domain
    // error namespace (typically N00B_ATTEST_ERR_SIGN_FAILED).
    auto sig_r = n00b_attest_signer_sign(signer,
                                         pae,
                                         .allocator = alloc_for_call);
    if (n00b_result_is_err(sig_r)) {
        return n00b_result_err(bool, n00b_result_get_err(sig_r));
    }
    n00b_buffer_t *sig = n00b_result_get(sig_r);

    // Fetch the cached keyid (SHA-256 of SPKI DER, hex-encoded per
    // D-039 — the file backend computes + caches at load time, so
    // this is a pointer lookup, NOT a recomputation).
    n00b_string_t *keyid = n00b_attest_signer_keyid(signer);
    if (keyid == nullptr) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }

    // Dispatch through the low-level entry so multi-signer flows
    // (FR-11, deferred) can use the same path against pre-computed
    // signatures.
    return n00b_attest_envelope_add_signature(env,
                                              keyid,
                                              sig,
                                              .allocator = alloc_for_call);
}
