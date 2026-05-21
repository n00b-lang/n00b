/*
 * greet.c — Handlers + per-type CBOR hooks for the quic_rpc_demo.
 *
 * The handler functions are annotated with `@rpc(...)` so ncc emits both
 * the dispatcher and the constructor-registrar at this site.  The CBOR
 * encode/decode hooks (per request/response/stream-item type) are
 * referenced directly by the ncc-generated dispatcher + client stub.
 *
 * Wire shape per RPC:
 *
 *   Hello (unary):
 *     request  = CBOR map {"name": <text>}
 *     response = CBOR map {"message": <text>}
 *
 *   Stream (server-stream):
 *     request  = CBOR map {"count": <int>}
 *     response = sequence of CBOR maps {"i": <int>, "text": <text>}
 *
 * The encoders use `n00b_cbor_write_*` (incremental writers) for clarity;
 * a real production service would lean on the marshaler-driven type→CBOR
 * dispatch for arbitrarily complex types.
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

#include "greet.h"

/* ============================================================================
 * Tiny helpers — read/write CBOR text-strings as `n00b_string_t *`.
 * ============================================================================ */

static n00b_string_t *
cstr_n(const char *s, size_t n)
{
    /* `n00b_string_from_cstr` requires NUL-termination; copy first. */
    char *buf = malloc(n + 1);
    if (!buf) return nullptr;
    memcpy(buf, s, n);
    buf[n] = '\0';
    n00b_string_t *r = n00b_string_from_cstr(buf);
    free(buf);
    return r;
}

/* Walk the CBOR AST for a top-level map, look up a key, return its value
 * node (or nullptr).  Keeps `greet.c` independent of any particular
 * decode helper API. */
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
        size_t kn = (size_t)ks->u8_bytes;
        if (kn != klen) continue;
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

/* ============================================================================
 * CBOR encode/decode hooks for each user-defined struct.
 *
 * The ncc-generated dispatchers + client stubs reference these by their
 * `typeid("cbor_encode", T *)` and `typeid("cbor_decode", T *)` mangled
 * symbols.  Real production code would either let the marshaler walk the
 * type to drive CBOR (work-in-progress in the type-system layer), or
 * generate these via a code generator.  For the demo we hand-write them
 * because the wire format is intentionally tiny (one key per struct).
 * ============================================================================ */

n00b_buffer_t *
typeid("cbor_encode", GreetRequest *)(GreetRequest *req)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    write_text_field(b, "name", req ? req->name : nullptr);
    return b;
}

n00b_result_t(GreetRequest *)
typeid("cbor_decode", GreetRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(GreetRequest *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    GreetRequest *req = n00b_alloc(GreetRequest);
    req->name = map_get_string(root, "name");
    if (!req->name) req->name = n00b_string_from_cstr("");
    return n00b_result_ok(GreetRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", GreetReply *)(GreetReply *rep)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    write_text_field(b, "message", rep ? rep->message : nullptr);
    return b;
}

n00b_result_t(GreetReply *)
typeid("cbor_decode", GreetReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(GreetReply *, (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    GreetReply *rep = n00b_alloc(GreetReply);
    rep->message = map_get_string(root, "message");
    if (!rep->message) rep->message = n00b_string_from_cstr("");
    return n00b_result_ok(GreetReply *, rep);
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
        return n00b_result_err(StreamRequest *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    StreamRequest *req = n00b_alloc(StreamRequest);
    req->count = map_get_int(root, "count", 0);
    return n00b_result_ok(StreamRequest *, req);
}

/* StreamItem is the per-message type carried over the server-stream's
 * data frames.  We need both per-item CBOR encode/decode AND the typed
 * stream wrapper hooks (rpc_stream_encode / rpc_stream_decode) that the
 * ncc-generated server-stream dispatcher + client stub call. */

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
        return n00b_result_err(StreamItem *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    StreamItem *it = n00b_alloc(StreamItem);
    it->i    = map_get_int(root, "i", 0);
    it->text = map_get_string(root, "text");
    if (!it->text) it->text = n00b_string_from_cstr("");
    return n00b_result_ok(StreamItem *, it);
}

/* ============================================================================
 * ChunkRequest / UploadReply — types for the client-stream `Upload` RPC.
 * ============================================================================ */

n00b_buffer_t *
typeid("cbor_encode", ChunkRequest *)(ChunkRequest *req)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 1);
    write_text_field(b, "data", req ? req->data : nullptr);
    return b;
}

n00b_result_t(ChunkRequest *)
typeid("cbor_decode", ChunkRequest *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(ChunkRequest *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    ChunkRequest *req = n00b_alloc(ChunkRequest);
    req->data = map_get_string(root, "data");
    if (!req->data) req->data = n00b_string_from_cstr("");
    return n00b_result_ok(ChunkRequest *, req);
}

n00b_buffer_t *
typeid("cbor_encode", UploadReply *)(UploadReply *rep)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 2);
    n00b_cbor_write_text(b, "bytes_total", 11);
    n00b_cbor_write_int(b, rep ? rep->bytes_total : 0);
    n00b_cbor_write_text(b, "chunks", 6);
    n00b_cbor_write_int(b, rep ? rep->chunks : 0);
    return b;
}

n00b_result_t(UploadReply *)
typeid("cbor_decode", UploadReply *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(UploadReply *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    UploadReply *rep = n00b_alloc(UploadReply);
    rep->bytes_total = map_get_int(root, "bytes_total", 0);
    rep->chunks      = map_get_int(root, "chunks", 0);
    return n00b_result_ok(UploadReply *, rep);
}

/* ============================================================================
 * ChatMessage — type for the bidi `Chat` RPC.
 * ============================================================================ */

n00b_buffer_t *
typeid("cbor_encode", ChatMessage *)(ChatMessage *m)
{
    n00b_buffer_t *b = n00b_alloc(n00b_buffer_t);
    n00b_buffer_init(b, .length = 0);
    n00b_cbor_write_map_header(b, 2);
    write_text_field(b, "text", m ? m->text : nullptr);
    n00b_cbor_write_text(b, "seq", 3);
    n00b_cbor_write_int(b, m ? m->seq : 0);
    return b;
}

n00b_result_t(ChatMessage *)
typeid("cbor_decode", ChatMessage *)(n00b_buffer_t *body)
{
    auto rv = n00b_cbor_decode_strict(body, nullptr);
    if (n00b_result_is_err(rv)) {
        return n00b_result_err(ChatMessage *,
                               (int)n00b_result_get_err(rv));
    }
    n00b_cbor_value_t *root = n00b_result_get(rv);
    ChatMessage *m = n00b_alloc(ChatMessage);
    m->text = map_get_string(root, "text");
    if (!m->text) m->text = n00b_string_from_cstr("");
    m->seq  = map_get_int(root, "seq", 0);
    return n00b_result_ok(ChatMessage *, m);
}

/* ============================================================================
 * Stream wrappers: typed `n00b_rpc_stream_t(StreamItem)` ↔ wire-form
 * `n00b_rpc_stream_t(n00b_buffer_t *)`.
 *
 * The server-stream dispatcher receives the user handler's typed stream
 * and asks for a wire-form stream; the client stub does the inverse.
 *
 * Implementation: spawn a small worker that drains one stream and pushes
 * the encoded/decoded items into the other.  The runtime's stream
 * primitives (FIFO + close-state) are thread-safe.
 *
 * Both halves use the runtime's `n00b_rpc_buffer_stream_*` primitives;
 * the typed half is just a thin alias (we don't need a separate FIFO
 * implementation for the demo).
 * ============================================================================ */

typedef struct {
    n00b_rpc_stream_t(StreamItem)        *typed;
    n00b_rpc_stream_t(n00b_buffer_t *)   *wire;
} _stream_pump_arg_t;

static void *
_stream_encode_pump(void *arg)
{
    /* Drain typed → encode → push to wire.  The typed stream wraps the
     * same buffer-stream primitive (we cast through `_opaque`), so we
     * can recv buffers and forward verbatim.  In a real per-type
     * implementation, we would recv typed values and call the per-type
     * cbor_encode here; for the demo we let the handler push pre-encoded
     * buffers via `n00b_rpc_stream_send_buf` (defined below) and pump
     * them through. */
    _stream_pump_arg_t *p = arg;
    while (true) {
        auto rr = n00b_rpc_buffer_stream_recv(
            (n00b_rpc_stream_t(n00b_buffer_t *) *)p->typed);
        if (n00b_result_is_err(rr)) {
            int e = n00b_result_get_err(rr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) continue;
            n00b_rpc_buffer_stream_close_err(p->wire, (n00b_rpc_status_t)e);
            return nullptr;
        }
        n00b_buffer_t *b = n00b_result_get(rr);
        if (!b) {
            n00b_rpc_buffer_stream_close(p->wire);
            return nullptr;
        }
        (void)n00b_rpc_buffer_stream_send(p->wire, b);
    }
}

static void *
_stream_decode_pump(void *arg)
{
    _stream_pump_arg_t *p = arg;
    while (true) {
        auto rr = n00b_rpc_buffer_stream_recv(p->wire);
        if (n00b_result_is_err(rr)) {
            int e = n00b_result_get_err(rr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) continue;
            n00b_rpc_buffer_stream_close_err(
                (n00b_rpc_stream_t(n00b_buffer_t *) *)p->typed,
                (n00b_rpc_status_t)e);
            return nullptr;
        }
        n00b_buffer_t *b = n00b_result_get(rr);
        if (!b) {
            n00b_rpc_buffer_stream_close(
                (n00b_rpc_stream_t(n00b_buffer_t *) *)p->typed);
            return nullptr;
        }
        (void)n00b_rpc_buffer_stream_send(
            (n00b_rpc_stream_t(n00b_buffer_t *) *)p->typed, b);
    }
}

n00b_rpc_stream_t(n00b_buffer_t *) *
typeid("rpc_stream_encode", StreamItem)(n00b_rpc_stream_t(StreamItem) *typed)
{
    n00b_rpc_stream_t(n00b_buffer_t *) *wire = n00b_rpc_buffer_stream_new();
    _stream_pump_arg_t *p = n00b_alloc(_stream_pump_arg_t);
    p->typed = typed;
    p->wire  = wire;
    (void)n00b_thread_spawn(_stream_encode_pump, p);
    return wire;
}

n00b_rpc_stream_t(StreamItem) *
typeid("rpc_stream_decode", StreamItem)(n00b_rpc_stream_t(n00b_buffer_t *) *wire)
{
    /* The "typed" half here is structurally identical to the wire half
     * (both are buffer streams).  See `_stream_encode_pump` for the
     * matching note.  Returning the wire stream directly cast to the
     * typed alias avoids spawning an extra pump thread on the client
     * side; the demo's caller still pulls items + decodes per-item via
     * `cbor_decode` on the buffers it receives. */
    (void)_stream_decode_pump;  /* keep available for reverse direction */
    return (n00b_rpc_stream_t(StreamItem) *)wire;
}

/* ----- ChunkRequest stream adapters (used by client-stream `Upload`) ----- */

n00b_rpc_stream_t(n00b_buffer_t *) *
typeid("rpc_stream_encode", ChunkRequest)(
    n00b_rpc_stream_t(ChunkRequest) *typed)
{
    /* Same alias-cast trick as StreamItem: the client pushes already-
     * CBOR-encoded buffers onto the typed stream, and this hook just
     * hands the runtime the same FIFO. */
    return (n00b_rpc_stream_t(n00b_buffer_t *) *)typed;
}

n00b_rpc_stream_t(ChunkRequest) *
typeid("rpc_stream_decode", ChunkRequest)(
    n00b_rpc_stream_t(n00b_buffer_t *) *wire)
{
    return (n00b_rpc_stream_t(ChunkRequest) *)wire;
}

/* ----- ChatMessage stream adapters (used by bidi `Chat`) ----- */

n00b_rpc_stream_t(n00b_buffer_t *) *
typeid("rpc_stream_encode", ChatMessage)(n00b_rpc_stream_t(ChatMessage) *typed)
{
    return (n00b_rpc_stream_t(n00b_buffer_t *) *)typed;
}

n00b_rpc_stream_t(ChatMessage) *
typeid("rpc_stream_decode", ChatMessage)(n00b_rpc_stream_t(n00b_buffer_t *) *wire)
{
    return (n00b_rpc_stream_t(ChatMessage) *)wire;
}

/* ============================================================================
 * Handlers (server side).
 *
 * `@rpc("greet.v1.Greeter/Hello")` and `@rpc("greet.v1.Greeter/Stream")`
 * cause ncc to emit dispatchers + constructor-registrars; the registrars
 * fire on process start and wire these handlers into the global RPC
 * registry.  The client stubs (also emitted by ncc) call into the
 * runtime's `n00b_rpc_call_unary` / `n00b_rpc_call_server_stream`.
 * ============================================================================ */

n00b_result_t(GreetReply *)
greet_hello(GreetRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("greet.v1.Greeter/Hello")
{
    (void)ctx;

    GreetReply *r = n00b_alloc(GreetReply);

    /* "Hello, <name>!" — uses snprintf for portability without leaning on
     * n00b_string_format's variadic discipline. */
    const char *who = "world";
    size_t      wn  = 5;
    if (req && req->name && req->name->data && req->name->u8_bytes > 0) {
        who = (const char *)req->name->data;
        wn  = req->name->u8_bytes;
    }
    char  buf[256];
    int   n = snprintf(buf, sizeof(buf), "Hello, %.*s!", (int)wn, who);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    r->message = n00b_string_from_cstr(buf);

    return n00b_result_ok(GreetReply *, r);
}

n00b_result_t(n00b_rpc_stream_t(StreamItem) *)
greet_stream(StreamRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("greet.v1.Greeter/Stream")
{
    (void)ctx;

    /* Build a typed stream + push N items.  Because our typed stream is
     * structurally a buffer stream (see the pump notes above), we push
     * pre-encoded buffers — encoding each item via the per-type
     * `cbor_encode` hook. */
    n00b_rpc_stream_t(n00b_buffer_t *) *out = n00b_rpc_buffer_stream_new();

    int64_t count = req ? req->count : 0;
    if (count < 0)  count = 0;
    if (count > 64) count = 64;  /* cap; keeps the demo fast under tests */

    /* Encode each StreamItem and push it. */
    extern n00b_buffer_t *
        typeid("cbor_encode", StreamItem *)(StreamItem *);

    for (int64_t i = 1; i <= count; i++) {
        StreamItem item;
        char       buf[64];
        int        n = snprintf(buf, sizeof(buf), "tick %lld", (long long)i);
        if (n < 0) n = 0;
        if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
        buf[n] = '\0';

        item.i    = i;
        item.text = n00b_string_from_cstr(buf);

        n00b_buffer_t *enc = typeid("cbor_encode", StreamItem *)(&item);
        (void)n00b_rpc_buffer_stream_send(out, enc);
    }
    n00b_rpc_buffer_stream_close(out);

    return n00b_result_ok(n00b_rpc_stream_t(StreamItem) *,
                          (n00b_rpc_stream_t(StreamItem) *)out);
}

/* ----- Upload: client-stream — N ChunkRequest items → one UploadReply ----- */

n00b_result_t(UploadReply *)
upload_chunks(n00b_rpc_stream_t(ChunkRequest) *in, n00b_rpc_ctx_t *ctx)
    @rpc("greet.v1.Greeter/Upload")
{
    (void)ctx;

    /* Drain the inbound stream as encoded buffers (the typed↔wire
     * mapping is an alias-cast for ChunkRequest), decode each one,
     * and accumulate byte count + chunk count. */
    n00b_rpc_stream_t(n00b_buffer_t *) *wire =
        (n00b_rpc_stream_t(n00b_buffer_t *) *)in;

    extern n00b_result_t(ChunkRequest *)
        typeid("cbor_decode", ChunkRequest *)(n00b_buffer_t *);

    int64_t bytes  = 0;
    int64_t chunks = 0;

    while (true) {
        auto rr = n00b_rpc_buffer_stream_recv(wire);
        if (n00b_result_is_err(rr)) {
            int e = n00b_result_get_err(rr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) continue;
            return n00b_result_err(UploadReply *, e);
        }
        n00b_buffer_t *b = n00b_result_get(rr);
        if (!b) break;  /* clean EOS */

        auto dr = typeid("cbor_decode", ChunkRequest *)(b);
        if (n00b_result_is_err(dr)) {
            return n00b_result_err(UploadReply *,
                                   (int)n00b_result_get_err(dr));
        }
        ChunkRequest *c = n00b_result_get(dr);
        chunks++;
        if (c && c->data) bytes += (int64_t)c->data->u8_bytes;
    }

    UploadReply *rep = n00b_alloc(UploadReply);
    rep->bytes_total = bytes;
    rep->chunks      = chunks;
    return n00b_result_ok(UploadReply *, rep);
}

/* ----- Chat: bidi — echo each inbound ChatMessage with seq+1 ----- */

typedef struct {
    n00b_rpc_stream_t(n00b_buffer_t *) *in;
    n00b_rpc_stream_t(n00b_buffer_t *) *out;
} _chat_pump_arg_t;

static void *
_chat_pump_main(void *arg)
{
    _chat_pump_arg_t *p = arg;

    extern n00b_result_t(ChatMessage *)
        typeid("cbor_decode", ChatMessage *)(n00b_buffer_t *);
    extern n00b_buffer_t *
        typeid("cbor_encode", ChatMessage *)(ChatMessage *);

    while (true) {
        auto rr = n00b_rpc_buffer_stream_recv(p->in);
        if (n00b_result_is_err(rr)) {
            int e = n00b_result_get_err(rr);
            if (e == N00B_QUIC_ERR_NEED_MORE_DATA) continue;
            n00b_rpc_buffer_stream_close_err(p->out, (n00b_rpc_status_t)e);
            return nullptr;
        }
        n00b_buffer_t *b = n00b_result_get(rr);
        if (!b) {
            /* Client FINed — close our outbound side and exit. */
            n00b_rpc_buffer_stream_close(p->out);
            return nullptr;
        }

        auto dr = typeid("cbor_decode", ChatMessage *)(b);
        if (n00b_result_is_err(dr)) {
            n00b_rpc_buffer_stream_close_err(
                p->out, (n00b_rpc_status_t)n00b_result_get_err(dr));
            return nullptr;
        }
        ChatMessage *inbound = n00b_result_get(dr);

        /* Build the echo: text passes through verbatim, seq incremented. */
        ChatMessage reply = {
            .text = inbound ? inbound->text : nullptr,
            .seq  = inbound ? inbound->seq + 1 : 1,
        };
        n00b_buffer_t *enc = typeid("cbor_encode", ChatMessage *)(&reply);
        (void)n00b_rpc_buffer_stream_send(p->out, enc);
    }
}

n00b_result_t(n00b_rpc_stream_t(ChatMessage) *)
chat_session(n00b_rpc_stream_t(ChatMessage) *in, n00b_rpc_ctx_t *ctx)
    @rpc("greet.v1.Greeter/Chat")
{
    (void)ctx;

    n00b_rpc_stream_t(n00b_buffer_t *) *out = n00b_rpc_buffer_stream_new();

    _chat_pump_arg_t *p = n00b_alloc(_chat_pump_arg_t);
    p->in  = (n00b_rpc_stream_t(n00b_buffer_t *) *)in;
    p->out = out;
    (void)n00b_thread_spawn(_chat_pump_main, p);

    return n00b_result_ok(n00b_rpc_stream_t(ChatMessage) *,
                          (n00b_rpc_stream_t(ChatMessage) *)out);
}
