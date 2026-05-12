/**
 * @file greet.h
 * @brief Demo service definition for the quic_rpc_demo example.
 *
 * Two RPCs:
 *   - `greet.v1.Greeter/Hello` — unary; takes a name, returns a greeting.
 *   - `greet.v1.Greeter/Stream` — server-stream; takes a count, emits N items.
 *
 * NOTE on `@rpc` headers: rpc.md describes the "header decl + .c defn"
 * shape, but ncc's emitted client stub for a forward declaration is a
 * function *definition* (with body referencing per-type CBOR hooks).
 * Including such a header in multiple TUs produces duplicate-symbol
 * link errors.  For this demo we keep the `@rpc` annotation in
 * `greet.c` only and expose **handwritten extern declarations** of the
 * generated client stubs here so other TUs can call them without
 * re-emitting the body.
 *
 * The mangling rule (rpc.md § Generated symbols) is:
 *   `n00b_rpc_call_<svc>__<method>` where dots in the package+service
 *   become underscores and the slash before the method becomes a
 *   double underscore (`__`).
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "net/quic/rpc.h"
#include "net/quic/rpc_ctx.h"

/* ---------------------------------------------------------------------------
 * Request / response types.
 *
 * Tiny structs — one field each.  The CBOR encoding lives in greet.c.
 * --------------------------------------------------------------------------- */

typedef struct {
    n00b_string_t *name;
} GreetRequest;

typedef struct {
    n00b_string_t *message;
} GreetReply;

typedef struct {
    int64_t count;
} StreamRequest;

typedef struct {
    int64_t        i;
    n00b_string_t *text;
} StreamItem;

/* `Upload` (client-stream): the client pushes N `ChunkRequest` items and
 * FINs; the server replies with one `UploadReply` summary. */
typedef struct {
    n00b_string_t *data;
} ChunkRequest;

typedef struct {
    int64_t bytes_total;
    int64_t chunks;
} UploadReply;

/* `Chat` (bidi): client and server each stream `ChatMessage{text, seq}`
 * items independently.  The demo server echoes each inbound message
 * with `seq + 1`; the server FINs after the client FINs. */
typedef struct {
    n00b_string_t *text;
    int64_t        seq;
} ChatMessage;

/* ---------------------------------------------------------------------------
 * Client-stub declarations.
 *
 * The matching definitions are emitted by ncc at the `@rpc(...)`
 * annotations in greet.c.  We re-declare them here verbatim so any TU
 * including this header can call them as ordinary C functions.
 * --------------------------------------------------------------------------- */

extern n00b_result_t(GreetReply *)
n00b_rpc_call_greet_v1_Greeter__Hello(n00b_rpc_ctx_t     *ctx,
                                       n00b_rpc_channel_t *chan,
                                       GreetRequest       *req);

extern n00b_result_t(n00b_rpc_stream_t(StreamItem) *)
n00b_rpc_call_greet_v1_Greeter__Stream(n00b_rpc_ctx_t     *ctx,
                                        n00b_rpc_channel_t *chan,
                                        StreamRequest      *req);

extern n00b_result_t(UploadReply *)
n00b_rpc_call_greet_v1_Greeter__Upload(n00b_rpc_ctx_t                  *ctx,
                                        n00b_rpc_channel_t              *chan,
                                        n00b_rpc_stream_t(ChunkRequest) *in);

extern n00b_result_t(n00b_rpc_stream_t(ChatMessage) *)
n00b_rpc_call_greet_v1_Greeter__Chat(n00b_rpc_ctx_t                 *ctx,
                                      n00b_rpc_channel_t             *chan,
                                      n00b_rpc_stream_t(ChatMessage) *in);
