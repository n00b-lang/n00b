/*
 * services.c — RPC handlers + per-type CBOR hooks for the
 * Phase 5 multi-tenant demo.
 *
 * Five RPCs (matching services.h):
 *   - phase5.v1.Greeter/Hello    — unary, audience-gated
 *   - phase5.v1.Greeter/Stream   — server-stream, audience-gated
 *   - phase5.v1.Vault/Read       — unary, DPoP-required
 *   - phase5.v1.Vault/Write      — unary, DPoP + role=admin
 *   - phase5.v1.MTls/Echo        — unary, mTLS-bound token required
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "net/quic/cbor.h"
#include "net/quic/rpc.h"
#include "net/quic/rpc_ctx.h"
#include "net/quic/rpc_status.h"

#include "services.h"

/* ============================================================================
 * CBOR helpers (factored out of greet.c).
 * ============================================================================ */

static n00b_string_t *
cstr_n(const char *s, size_t n)
{
    char *buf = malloc(n + 1);
    if (!buf) return nullptr;
    memcpy(buf, s, n);
    buf[n] = '\0';
    n00b_string_t *r = n00b_string_from_cstr(buf);
    free(buf);
    return r;
}

static n00b_cbor_value_t *
map_get(n00b_cbor_value_t *map, const char *key)
{
    if (!map || map->kind != N00B_CBOR_VT_MAP) return nullptr;
    size_t klen = strlen(key);
    for (size_t i = 0; i < map->u.map.count; i++) {
        n00b_cbor_value_t *k = map->u.map.pairs[i].key;
        if (!k || k->kind != N00B_CBOR_VT_STRING) continue;
        n00b_string_t *ks = k->u.string;
        if (!ks) continue;
        if ((size_t)ks->u8_bytes != klen) continue;
        if (memcmp(ks->data, key, klen) == 0) {
            return map->u.map.pairs[i].val;
        }
    }
    return nullptr;
}

static int64_t
map_get_int(n00b_cbor_value_t *map, const char *key, int64_t fallback)
{
    n00b_cbor_value_t *v = map_get(map, key);
    if (!v) return fallback;
    if (v->kind == N00B_CBOR_VT_UINT)   return (int64_t)v->u.uint;
    if (v->kind == N00B_CBOR_VT_INT64)  return v->u.int64;
    if (v->kind == N00B_CBOR_VT_NEGINT) return -(int64_t)(v->u.uint + 1);
    return fallback;
}

static n00b_string_t *
map_get_string(n00b_cbor_value_t *map, const char *key)
{
    n00b_cbor_value_t *v = map_get(map, key);
    if (!v || v->kind != N00B_CBOR_VT_STRING || !v->u.string) return nullptr;
    return cstr_n((const char *)v->u.string->data,
                  (size_t)v->u.string->u8_bytes);
}

static void
write_text_field(n00b_buffer_t *dst, const char *key, n00b_string_t *val)
{
    n00b_cbor_write_text(dst, key, strlen(key));
    if (val && val->data) {
        n00b_cbor_write_text(dst, (const char *)val->data, val->u8_bytes);
    } else {
        n00b_cbor_write_text(dst, "", 0);
    }
}

/* Generic 1-string-field encode; saves repetition across the small types. */
static n00b_buffer_t *
encode_one_string(const char *field, n00b_string_t *val)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    write_text_field(b, field, val);
    return b;
}

/* ============================================================================
 * CBOR hooks per type.
 * ============================================================================ */

n00b_buffer_t *
typeid("cbor_encode", HelloRequest *)(HelloRequest *req)
{
    return encode_one_string("name", req ? req->name : nullptr);
}

n00b_result_t(HelloRequest *)
typeid("cbor_decode", HelloRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(HelloRequest *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    HelloRequest *req = n00b_alloc(HelloRequest);
    req->name = map_get_string(root, "name");
    if (!req->name) req->name = n00b_string_from_cstr("");
    return n00b_result_ok(HelloRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", HelloReply *)(HelloReply *rep)
{
    return encode_one_string("message", rep ? rep->message : nullptr);
}

n00b_result_t(HelloReply *)
typeid("cbor_decode", HelloReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(HelloReply *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    HelloReply *r = n00b_alloc(HelloReply);
    r->message = map_get_string(root, "message");
    if (!r->message) r->message = n00b_string_from_cstr("");
    return n00b_result_ok(HelloReply *, r);
}

n00b_buffer_t *
typeid("cbor_encode", StreamRequest *)(StreamRequest *req)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    n00b_cbor_write_text(b, "count", 5);
    n00b_cbor_write_int(b, req ? req->count : 0);
    return b;
}

n00b_result_t(StreamRequest *)
typeid("cbor_decode", StreamRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(StreamRequest *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    StreamRequest *req = n00b_alloc(StreamRequest);
    req->count = map_get_int(root, "count", 0);
    return n00b_result_ok(StreamRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", StreamItem *)(StreamItem *it)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 2);
    n00b_cbor_write_text(b, "i", 1);
    n00b_cbor_write_int(b, it ? it->i : 0);
    write_text_field(b, "text", it ? it->text : nullptr);
    return b;
}

n00b_result_t(StreamItem *)
typeid("cbor_decode", StreamItem *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(StreamItem *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    StreamItem *it = n00b_alloc(StreamItem);
    it->i    = map_get_int(root, "i", 0);
    it->text = map_get_string(root, "text");
    if (!it->text) it->text = n00b_string_from_cstr("");
    return n00b_result_ok(StreamItem *, it);
}

n00b_buffer_t *
typeid("cbor_encode", VaultReadRequest *)(VaultReadRequest *req)
{
    return encode_one_string("key", req ? req->key : nullptr);
}

n00b_result_t(VaultReadRequest *)
typeid("cbor_decode", VaultReadRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(VaultReadRequest *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    VaultReadRequest *req = n00b_alloc(VaultReadRequest);
    req->key = map_get_string(root, "key");
    if (!req->key) req->key = n00b_string_from_cstr("");
    return n00b_result_ok(VaultReadRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", VaultReadReply *)(VaultReadReply *rep)
{
    return encode_one_string("value", rep ? rep->value : nullptr);
}

n00b_result_t(VaultReadReply *)
typeid("cbor_decode", VaultReadReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(VaultReadReply *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    VaultReadReply *rep = n00b_alloc(VaultReadReply);
    rep->value = map_get_string(root, "value");
    if (!rep->value) rep->value = n00b_string_from_cstr("");
    return n00b_result_ok(VaultReadReply *, rep);
}

n00b_buffer_t *
typeid("cbor_encode", VaultWriteRequest *)(VaultWriteRequest *req)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 2);
    write_text_field(b, "key",   req ? req->key   : nullptr);
    write_text_field(b, "value", req ? req->value : nullptr);
    return b;
}

n00b_result_t(VaultWriteRequest *)
typeid("cbor_decode", VaultWriteRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(VaultWriteRequest *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    VaultWriteRequest *req = n00b_alloc(VaultWriteRequest);
    req->key   = map_get_string(root, "key");
    req->value = map_get_string(root, "value");
    if (!req->key)   req->key   = n00b_string_from_cstr("");
    if (!req->value) req->value = n00b_string_from_cstr("");
    return n00b_result_ok(VaultWriteRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", VaultWriteReply *)(VaultWriteReply *rep)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    n00b_cbor_write_text(b, "bytes", 5);
    n00b_cbor_write_int(b, rep ? rep->bytes : 0);
    return b;
}

n00b_result_t(VaultWriteReply *)
typeid("cbor_decode", VaultWriteReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(VaultWriteReply *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    VaultWriteReply *rep = n00b_alloc(VaultWriteReply);
    rep->bytes = map_get_int(root, "bytes", 0);
    return n00b_result_ok(VaultWriteReply *, rep);
}

n00b_buffer_t *
typeid("cbor_encode", EchoRequest *)(EchoRequest *req)
{
    return encode_one_string("payload", req ? req->payload : nullptr);
}

n00b_result_t(EchoRequest *)
typeid("cbor_decode", EchoRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(EchoRequest *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    EchoRequest *req = n00b_alloc(EchoRequest);
    req->payload = map_get_string(root, "payload");
    if (!req->payload) req->payload = n00b_string_from_cstr("");
    return n00b_result_ok(EchoRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", EchoReply *)(EchoReply *rep)
{
    return encode_one_string("payload", rep ? rep->payload : nullptr);
}

n00b_result_t(EchoReply *)
typeid("cbor_decode", EchoReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(EchoReply *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    EchoReply *rep = n00b_alloc(EchoReply);
    rep->payload = map_get_string(root, "payload");
    if (!rep->payload) rep->payload = n00b_string_from_cstr("");
    return n00b_result_ok(EchoReply *, rep);
}

/* Stream-wrapper hooks (typed ↔ wire).  We cast pass-through both
 * directions because our typed stream is structurally a buffer
 * stream — handlers push pre-encoded buffers and clients pop +
 * decode per-item.  Mirrors the greet.c StreamItem pattern. */
n00b_rpc_stream_t(n00b_buffer_t *) *
typeid("rpc_stream_encode", StreamItem)(n00b_rpc_stream_t(StreamItem) *typed)
{
    return (n00b_rpc_stream_t(n00b_buffer_t *) *)typed;
}

n00b_rpc_stream_t(StreamItem) *
typeid("rpc_stream_decode", StreamItem)(n00b_rpc_stream_t(n00b_buffer_t *) *wire)
{
    return (n00b_rpc_stream_t(StreamItem) *)wire;
}

/* ============================================================================
 * Handlers.
 * ============================================================================ */

n00b_result_t(HelloReply *)
phase5_greeter_hello(HelloRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("phase5.v1.Greeter/Hello")
{
    (void)ctx;
    HelloReply *rep = n00b_alloc(HelloReply);
    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!",
             (req && req->name && req->name->data) ? (const char *)req->name->data
                                                   : "world");
    rep->message = n00b_string_from_cstr(buf);
    return n00b_result_ok(HelloReply *, rep);
}

n00b_result_t(n00b_rpc_stream_t(StreamItem) *)
phase5_greeter_stream(StreamRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("phase5.v1.Greeter/Stream")
{
    (void)ctx;
    n00b_rpc_stream_t(n00b_buffer_t *) *out = n00b_rpc_buffer_stream_new();
    int64_t n = (req && req->count > 0) ? req->count : 0;
    if (n > 32) n = 32;
    extern n00b_buffer_t *
        typeid("cbor_encode", StreamItem *)(StreamItem *);
    for (int64_t i = 1; i <= n; i++) {
        StreamItem it;
        char       buf[64];
        snprintf(buf, sizeof(buf), "tick %lld", (long long)i);
        it.i    = i;
        it.text = n00b_string_from_cstr(buf);
        n00b_buffer_t *enc = typeid("cbor_encode", StreamItem *)(&it);
        (void)n00b_rpc_buffer_stream_send(out, enc);
    }
    n00b_rpc_buffer_stream_close(out);
    return n00b_result_ok(n00b_rpc_stream_t(StreamItem) *,
                          (n00b_rpc_stream_t(StreamItem) *)out);
}

n00b_result_t(VaultReadReply *)
phase5_vault_read(VaultReadRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("phase5.v1.Vault/Read")
{
    (void)ctx;
    VaultReadReply *rep = n00b_alloc(VaultReadReply);
    /* Demo "vault": for any key, return "secret-for:<key>".
     * Real deployments would resolve from a Phase 1 secret broker. */
    char buf[256];
    snprintf(buf, sizeof(buf), "secret-for:%s",
             (req && req->key && req->key->data) ? (const char *)req->key->data
                                                 : "(empty)");
    rep->value = n00b_string_from_cstr(buf);
    return n00b_result_ok(VaultReadReply *, rep);
}

n00b_result_t(VaultWriteReply *)
phase5_vault_write(VaultWriteRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("phase5.v1.Vault/Write")
{
    (void)ctx;
    VaultWriteReply *rep = n00b_alloc(VaultWriteReply);
    /* Demo "vault": just count bytes received.  Auth gate (DPoP +
     * role=admin claim) is enforced at the policy layer before this
     * handler runs; reaching the handler means the caller is
     * authorized. */
    int64_t bytes = 0;
    if (req && req->key   && req->key->data)   bytes += req->key->u8_bytes;
    if (req && req->value && req->value->data) bytes += req->value->u8_bytes;
    rep->bytes = bytes;
    return n00b_result_ok(VaultWriteReply *, rep);
}

n00b_result_t(EchoReply *)
phase5_mtls_echo(EchoRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("phase5.v1.MTls/Echo")
{
    (void)ctx;
    EchoReply *rep = n00b_alloc(EchoReply);
    rep->payload = (req && req->payload) ? req->payload
                                         : n00b_string_from_cstr("");
    return n00b_result_ok(EchoReply *, rep);
}
