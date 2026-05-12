/**
 * @file services.h
 * @brief Service definitions for the Phase 5 multi-tenant demo.
 *
 * Five RPCs across three services:
 *   - `phase5.v1.Greeter/Hello`  — unary, audience-gated
 *   - `phase5.v1.Greeter/Stream` — server-stream, audience-gated
 *   - `phase5.v1.Vault/Read`     — unary, DPoP-required
 *   - `phase5.v1.Vault/Write`    — unary, DPoP + role=admin claim
 *   - `phase5.v1.MTls/Echo`      — unary, mTLS-bound token required
 *
 * Each tenant is identified at the request layer by the `X-Tenant`
 * header.  The demo's verifier resolver maps tenant → IdP id (per
 * the manifest's `auth.idps[]`) → JWT verifier; the policy attached
 * to each RPC is per-service per Phase 4 § 4.11 + Phase 3 § 10.
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
 * --------------------------------------------------------------------------- */

typedef struct {
    n00b_string_t *name;
} HelloRequest;

typedef struct {
    n00b_string_t *message;
} HelloReply;

typedef struct {
    int64_t count;
} StreamRequest;

typedef struct {
    int64_t        i;
    n00b_string_t *text;
} StreamItem;

typedef struct {
    n00b_string_t *key;
} VaultReadRequest;

typedef struct {
    n00b_string_t *value;
} VaultReadReply;

typedef struct {
    n00b_string_t *key;
    n00b_string_t *value;
} VaultWriteRequest;

typedef struct {
    int64_t bytes;
} VaultWriteReply;

typedef struct {
    n00b_string_t *payload;
} EchoRequest;

typedef struct {
    n00b_string_t *payload;
} EchoReply;

/* ---------------------------------------------------------------------------
 * Client-stub declarations.  Definitions are emitted at the `@rpc(...)`
 * annotations in services.c.
 * --------------------------------------------------------------------------- */

extern n00b_result_t(HelloReply *)
n00b_rpc_call_phase5_v1_Greeter__Hello(n00b_rpc_ctx_t     *ctx,
                                        n00b_rpc_channel_t *chan,
                                        HelloRequest       *req);

extern n00b_result_t(n00b_rpc_stream_t(StreamItem) *)
n00b_rpc_call_phase5_v1_Greeter__Stream(n00b_rpc_ctx_t     *ctx,
                                         n00b_rpc_channel_t *chan,
                                         StreamRequest      *req);

extern n00b_result_t(VaultReadReply *)
n00b_rpc_call_phase5_v1_Vault__Read(n00b_rpc_ctx_t     *ctx,
                                     n00b_rpc_channel_t *chan,
                                     VaultReadRequest   *req);

extern n00b_result_t(VaultWriteReply *)
n00b_rpc_call_phase5_v1_Vault__Write(n00b_rpc_ctx_t     *ctx,
                                      n00b_rpc_channel_t *chan,
                                      VaultWriteRequest  *req);

extern n00b_result_t(EchoReply *)
n00b_rpc_call_phase5_v1_MTls__Echo(n00b_rpc_ctx_t     *ctx,
                                    n00b_rpc_channel_t *chan,
                                    EchoRequest        *req);
