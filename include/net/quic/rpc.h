/**
 * @file rpc.h
 * @brief n00b RPC runtime — service registry, call dispatch, streaming.
 *
 * Phase 4 § 4.5–4.7.  This is the runtime side of the `@rpc("svc/method")`
 * ncc annotation.  Ships the full registry + client/server dispatch
 * machinery for all four RPC patterns: unary, server-stream,
 * client-stream, and bidi.  Cancellation + deadline propagation
 * runs through `n00b_rpc_ctx_t`.
 *
 * ### Generated symbol naming (from `@rpc(...)`)
 *
 * The ncc transform emits three symbols per annotated function:
 *
 * | Symbol                                                | Visibility       |
 * |-------------------------------------------------------|------------------|
 * | `_n00b_rpc_dispatch__<svc>__<method>`                 | static           |
 * | `_n00b_rpc_register__<svc>__<method>`                 | static + ctor    |
 * | `n00b_rpc_call_<svc>__<method>`                       | extern (public)  |
 *
 * `<svc>` = the dotted package + service joined with underscores
 * (`checkout.v1.Checkout` → `checkout_v1_Checkout`); `<method>` = the
 * verbatim method name from the annotation string (everything after
 * the slash).  The slash itself becomes a double underscore (`__`).
 *
 * The constructor (registered via `__attribute__((constructor))`) calls
 * `n00b_rpc_register()` at process start to install the dispatcher in
 * the runtime registry.  The client stub is what user code calls.
 *
 * @see ~/dd/quic_4.md § 7
 * @see ~/dd/quic_4_ncc_rpc_annotation.md
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "n00b.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "conduit/conduit.h"
#include "net/quic/rpc_ctx.h"
#include "net/quic/rpc_status.h"
#include "net/quic/metrics.h"
#include "net/quic/h3.h"
#include "net/quic/manifest.h"

/* Forward declarations to avoid pulling in h3.h / auth_policy.h /
 * jwt.h transitively. */
typedef struct n00b_h3_client n00b_h3_client_t;
typedef struct n00b_h3_server n00b_h3_server_t;
typedef struct n00b_quic_auth_policy n00b_quic_auth_policy_t;
typedef struct n00b_jwt_verifier n00b_jwt_verifier_t;
typedef struct n00b_rpc_auth_credentials n00b_rpc_auth_credentials_t;

typedef struct {
    n00b_h3_client_t *h3;
    const char       *scheme;
    const char       *authority;
} n00b_rpc_channel_spec_t;

/**
 * @brief Opaque handle to an RPC channel (a QUIC channel running H3 + RPC).
 *
 * Concretely: a small struct holding the underlying `n00b_h3_client_t *`
 * plus the `:scheme` and `:authority` strings used on every outbound
 * request.  Allocate via `n00b_rpc_channel_new` after constructing
 * the H3 client.
 */
typedef struct n00b_rpc_channel n00b_rpc_channel_t;

/**
 * @brief Opaque handle to a server-side RPC dispatcher.
 *
 * Returned by `n00b_rpc_attach_server`; subscribes a worker to the
 * H3 server's request topic and dispatches each inbound request to
 * the registered handler.
 */
typedef struct n00b_rpc_server n00b_rpc_server_t;

/**
 * @brief Build a new RPC channel over an existing H3 client.
 *
 * The channel borrows @p h3 (does NOT take ownership).  The
 * `:scheme` and `:authority` strings are duplicated into the runtime's
 * conduit_pool so the caller may free their own copies.
 *
 * @param spec        Connected H3 client plus authority. `scheme`
 *                    defaults to "https".
 *
 * @return Allocated channel; nullptr on @p h3 / @p scheme / @p authority
 *         being null.
 */
extern n00b_rpc_channel_t *
n00b_rpc_channel_new(n00b_rpc_channel_spec_t spec);

extern n00b_rpc_channel_t *
n00b_rpc_channel_new_raw(n00b_h3_client_t *h3,
                         const char       *scheme,
                         const char       *authority);

/** @brief Borrowed pointer to the underlying H3 client (diagnostic). */
extern n00b_h3_client_t *
n00b_rpc_channel_h3(n00b_rpc_channel_t *chan);

/**
 * @brief A typed stream of values flowing across an RPC stream-shaped call.
 *
 * `T` is the per-message type.  At the wire layer `T` is always
 * `n00b_buffer_t *` (one CBOR-encoded item per push); ncc-generated
 * stubs adapt the user-facing type to and from this form.
 *
 * @details
 *
 * Concretely, `n00b_rpc_stream_t(T)` is a tiny wrapper struct whose
 * `_opaque` field points at a runtime-allocated FIFO + close/error
 * state.  Operations:
 *
 *   - `n00b_rpc_stream_send(stream, item)`   — push.  ok / err.
 *   - `n00b_rpc_stream_recv(stream)`         — pop one (option).
 *   - `n00b_rpc_stream_close(stream)`        — clean end-of-stream.
 *   - `n00b_rpc_stream_close_err(stream, st)`— err close.
 *   - `n00b_rpc_stream_is_closed(stream)`    — bool query.
 *
 * Allocation lives in `runtime->conduit_pool`.
 */
#define n00b_rpc_stream_t(T) _generic_struct typeid("rpc_stream", T) { \
    void *_opaque; \
}

/**
 * @brief Allocate a fresh runtime-internal RPC stream of CBOR buffers.
 *
 * The returned pointer is `n00b_rpc_stream_t(n00b_buffer_t *) *`; the
 * macro form is the type-checked surface, the typedef alias below
 * lets non-ncc code use it directly.
 */
typedef struct n00b_rpc_buffer_stream n00b_rpc_buffer_stream_t;

extern n00b_rpc_stream_t(n00b_buffer_t *) *
n00b_rpc_buffer_stream_new(void);

/** @brief Push one item onto the stream.  err on closed stream. */
extern n00b_result_t(bool)
n00b_rpc_buffer_stream_send(n00b_rpc_stream_t(n00b_buffer_t *) *stream,
                            n00b_buffer_t                       *item);

/** @brief End-of-stream marker (clean close).  Idempotent. */
extern void
n00b_rpc_buffer_stream_close(n00b_rpc_stream_t(n00b_buffer_t *) *stream);

/** @brief Error close.  @p status is mapped to the wire error. */
extern void
n00b_rpc_buffer_stream_close_err(n00b_rpc_stream_t(n00b_buffer_t *) *stream,
                                 n00b_rpc_status_t                   status);

/**
 * @brief Pop one item.  Returns ok with the item, or err on remote
 *        abort, or err with `N00B_QUIC_ERR_NEED_MORE_DATA` when the
 *        stream is empty + still open, or err with the `N00B_RPC_OK`
 *        sentinel when the stream is empty and closed cleanly (use
 *        `n00b_rpc_stream_is_closed` to disambiguate).
 *
 * Blocks up to a small budget waiting for data; callers should treat
 * the call as polling.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_rpc_buffer_stream_recv(n00b_rpc_stream_t(n00b_buffer_t *) *stream);

/** @brief True iff the producer side has been closed (clean or err). */
extern bool
n00b_rpc_buffer_stream_is_closed(n00b_rpc_stream_t(n00b_buffer_t *) *stream);

/** @brief Closed-with-error status code (or `N00B_RPC_OK` if clean). */
extern n00b_rpc_status_t
n00b_rpc_buffer_stream_status(n00b_rpc_stream_t(n00b_buffer_t *) *stream);

/* `n00b_rpc_stream_*` macros — dispatch on the wire-form item type.
 * Today only `n00b_buffer_t *` is supported at the runtime layer; ncc-
 * generated wrappers do the per-type CBOR adapt around these. */
#define n00b_rpc_stream_new(T) n00b_rpc_buffer_stream_new()
#define n00b_rpc_stream_send(stream, item)                                     \
    n00b_rpc_buffer_stream_send((stream), (item))
#define n00b_rpc_stream_close(stream)                                          \
    n00b_rpc_buffer_stream_close((stream))
#define n00b_rpc_stream_close_err(stream, st)                                  \
    n00b_rpc_buffer_stream_close_err((stream), (st))
#define n00b_rpc_stream_recv(stream)                                           \
    n00b_rpc_buffer_stream_recv((stream))
#define n00b_rpc_stream_is_closed(stream)                                      \
    n00b_rpc_buffer_stream_is_closed((stream))
#define n00b_rpc_stream_status(stream)                                         \
    n00b_rpc_buffer_stream_status((stream))

/**
 * @brief Dispatcher signature.  Decodes a CBOR-encoded request, invokes
 *        the user handler, encodes the reply.  ncc generates one of
 *        these per `@rpc(...)`-annotated function.
 */
typedef n00b_result_t(n00b_buffer_t *) (*n00b_rpc_dispatch_fn_t)(
    n00b_buffer_t   *req_cbor,
    n00b_rpc_ctx_t  *ctx);

/**
 * @brief Register a unary dispatcher under a method string.
 *
 * Called from each `@rpc(...)`-generated `__attribute__((constructor))`
 * at process start.  Adds an entry to a process-wide string-keyed
 * registry; lookup happens on the server when an inbound H3 request
 * lands.  Lazy-init (atomic-flag-guarded) and mutex-protected for safe
 * concurrent registrations.
 *
 * @param full_method  "package.service/method" (matches the annotation).
 * @param fn           Dispatcher emitted by ncc.
 */
extern void n00b_rpc_register(const char            *full_method,
                              n00b_rpc_dispatch_fn_t fn);

/**
 * @brief Issue a unary call from the client side.
 *
 * Builds an H3 POST to `/<full_method>` against the channel's
 * scheme/authority, ships @p req_cbor as the DATA body, awaits the
 * response, and returns the response body buffer (CBOR-encoded; the
 * ncc-generated stub decodes it).
 *
 * @param ctx          Cancellation/deadline context (may be nullptr;
 *                     when present, the deadline is propagated as the
 *                     `n00b-rpc-deadline-ms` header and a watcher
 *                     thread translates ctx-cancel into a stream
 *                     reset).
 * @param chan         RPC channel.
 * @param full_method  "package.service/method".
 * @param req_cbor     Pre-encoded CBOR request body (may be nullptr
 *                     for a void-payload call).
 *
 * @kw creds_override   When non-NULL, replaces the channel-level
 *                      credentials for THIS call only (sub-phase
 *                      4.9, per-call override; see § 7.3 of
 *                      `docs/quic/rpc_design.md`).  The bearer
 *                      token + DPoP proof are sent as
 *                      `authorization: Bearer <jwt>` and
 *                      `dpop: <proof>` for this request only;
 *                      channel-level credentials are NOT mutated
 *                      and apply unchanged to subsequent calls.
 *                      The pointed-to strings must outlive the
 *                      call.
 * @kw policy_override  When non-NULL, replaces the channel-level
 *                      `n00b-rpc-policy` header for THIS call only.
 *                      The server applies its at-least-as-strict
 *                      check (§ 7.3) against the
 *                      service-pinned default; downgrades return
 *                      `N00B_RPC_PERMISSION_DENIED`.
 *
 * @return Result wrapping the response CBOR buffer (ok), or an error
 *         status code (one of the `N00B_RPC_*` enum values, or a
 *         negative `n00b_quic_err_t` for transport-level failures).
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_rpc_call_unary(n00b_rpc_ctx_t     *ctx,
                    n00b_rpc_channel_t *chan,
                    const char         *full_method,
                    n00b_buffer_t      *req_cbor)
    _kargs {
        const n00b_rpc_auth_credentials_t *creds_override  = nullptr;
        const char                        *policy_override = nullptr;
    };

/**
 * @brief Issue a server-stream call (single request, N responses).
 *
 * Builds an H3 POST + FIN; the runtime pumps incoming DATA frames
 * through a CBOR-item decoder and pushes each item onto the
 * returned stream.  Caller iterates with `n00b_rpc_stream_recv`
 * until ok(nullptr) (clean end-of-stream) or err.
 *
 * @kw creds_override   Per-call credential override; see
 *                      `n00b_rpc_call_unary`.
 * @kw policy_override  Per-call `n00b-rpc-policy` override; see
 *                      `n00b_rpc_call_unary`.
 */
extern n00b_result_t(n00b_rpc_stream_t(n00b_buffer_t *) *)
n00b_rpc_call_server_stream(n00b_rpc_ctx_t     *ctx,
                            n00b_rpc_channel_t *chan,
                            const char         *full_method,
                            n00b_buffer_t      *req_cbor)
    _kargs {
        const n00b_rpc_auth_credentials_t *creds_override  = nullptr;
        const char                        *policy_override = nullptr;
    };

/**
 * @brief Issue a client-stream call (N requests, single response).
 *
 * Drains @p in onto the wire as a sequence of DATA frames; FINs
 * when @p in is closed.  Awaits the single CBOR-encoded response
 * (HEADERS + DATA + FIN).
 *
 * @kw creds_override   Per-call credential override; see
 *                      `n00b_rpc_call_unary`.
 * @kw policy_override  Per-call `n00b-rpc-policy` override; see
 *                      `n00b_rpc_call_unary`.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_rpc_call_client_stream(n00b_rpc_ctx_t                       *ctx,
                            n00b_rpc_channel_t                   *chan,
                            const char                           *full_method,
                            n00b_rpc_stream_t(n00b_buffer_t *)   *in)
    _kargs {
        const n00b_rpc_auth_credentials_t *creds_override  = nullptr;
        const char                        *policy_override = nullptr;
    };

/**
 * @brief Issue a bidi call (N requests, N responses).
 *
 * Drains @p in onto the wire concurrently with reading inbound
 * DATA frames into the returned stream.  Either side may FIN
 * independently.
 *
 * @kw creds_override   Per-call credential override; see
 *                      `n00b_rpc_call_unary`.
 * @kw policy_override  Per-call `n00b-rpc-policy` override; see
 *                      `n00b_rpc_call_unary`.
 */
extern n00b_result_t(n00b_rpc_stream_t(n00b_buffer_t *) *)
n00b_rpc_call_bidi(n00b_rpc_ctx_t                       *ctx,
                   n00b_rpc_channel_t                   *chan,
                   const char                           *full_method,
                   n00b_rpc_stream_t(n00b_buffer_t *)   *in)
    _kargs {
        const n00b_rpc_auth_credentials_t *creds_override  = nullptr;
        const char                        *policy_override = nullptr;
    };

/* ---------------------------------------------------------------------------
 * Streaming dispatcher signatures (sub-phase 4.7)
 *
 * The shapes match what ncc's `@rpc(...)` templates emit so a service
 * author's annotated handler can be wrapped without an extra adapter
 * layer.  The runtime owns the wire-side encode/decode (CBOR per item),
 * the handler owns the stream lifecycle (create + send-side close).
 * --------------------------------------------------------------------------- */

/**
 * @brief Server-stream dispatcher: client sent one request, handler
 *        creates an outbound stream and returns it.  Runtime drains
 *        the stream onto the wire as DATA frames; FINs once the
 *        handler closes the stream.
 */
typedef n00b_result_t(n00b_rpc_stream_t(n00b_buffer_t *) *)
    (*n00b_rpc_server_stream_dispatch_fn_t)(n00b_buffer_t  *req_cbor,
                                            n00b_rpc_ctx_t *ctx);

/**
 * @brief Client-stream dispatcher: runtime has built an in-stream from
 *        the inbound DATA frames; handler drains it and returns a
 *        single CBOR-encoded response.
 */
typedef n00b_result_t(n00b_buffer_t *)
    (*n00b_rpc_client_stream_dispatch_fn_t)(
        n00b_rpc_stream_t(n00b_buffer_t *) *in_stream,
        n00b_rpc_ctx_t                     *ctx);

/**
 * @brief Bidi dispatcher: runtime hands over the inbound stream;
 *        handler returns its outbound stream.  Both halves are driven
 *        independently by the runtime once the dispatcher returns.
 */
typedef n00b_result_t(n00b_rpc_stream_t(n00b_buffer_t *) *)
    (*n00b_rpc_bidi_dispatch_fn_t)(
        n00b_rpc_stream_t(n00b_buffer_t *) *in_stream,
        n00b_rpc_ctx_t                     *ctx);

/** @brief Register a server-stream dispatcher (sub-phase 4.7). */
extern void
n00b_rpc_register_server_stream(const char                           *full_method,
                                n00b_rpc_server_stream_dispatch_fn_t  fn);

/** @brief Register a client-stream dispatcher (sub-phase 4.7). */
extern void
n00b_rpc_register_client_stream(const char                           *full_method,
                                n00b_rpc_client_stream_dispatch_fn_t  fn);

/** @brief Register a bidi dispatcher (sub-phase 4.7). */
extern void
n00b_rpc_register_bidi(const char                  *full_method,
                       n00b_rpc_bidi_dispatch_fn_t  fn);

/* ===========================================================================
 * Server side (sub-phase 4.6)
 * =========================================================================== */

/**
 * @brief Attach an RPC dispatcher to a running H3 server.
 *
 * Subscribes a worker thread to the H3 server's request topic.  Each
 * inbound request is looked up in the global registry by
 * `:path[1:]`; matching unary methods invoke their dispatcher and
 * emit a CBOR response.  Misses produce a 404 + UNIMPLEMENTED.
 *
 * The `n00b-rpc-deadline-ms` request header (absolute unix-epoch ms;
 * see `docs/quic/rpc_design.md` § 6.2) is honored: if the deadline
 * has already elapsed when the request reaches the server, the
 * dispatcher emits DEADLINE_EXCEEDED without invoking the handler.
 *
 * @param h3       H3 server (already constructed via
 *                 `n00b_h3_server_new`).
 * @param conduit  Conduit hosting the request inbox.
 *
 * @return Allocated server handle; nullptr on null inputs.  Pass to
 *         `n00b_rpc_server_close` to tear down the worker.
 */
extern n00b_rpc_server_t *
n00b_rpc_attach_server(n00b_h3_server_t *h3, n00b_conduit_t *conduit);

/**
 * @brief Stop the RPC dispatcher worker.  Idempotent.
 *
 * The H3 server itself is NOT closed (the caller owns it); only the
 * RPC dispatcher's subscriber + worker thread are released.
 */
extern void n00b_rpc_server_close(n00b_rpc_server_t *s);

/**
 * @brief Test-only: highest observed handler-thread concurrency.
 *
 * Returns the peak value of the dispatcher's `in_flight` counter
 * since the server was attached.  Each spawn of a dispatch thread
 * increments `in_flight`; each handler exit decrements it.  The peak
 * is updated atomically on every spawn.  After a multi-call test
 * run, callers can assert `peak >= 2` to confirm the dispatcher
 * actually parallelized handlers.
 *
 * @param s  Server handle.
 * @return   Peak observed in-flight handler count, or 0 if @p s is NULL.
 */
extern uint32_t
n00b_rpc_server_peak_in_flight(n00b_rpc_server_t *s);

/* ===========================================================================
 * Auth wiring (sub-phase 4.9)
 *
 * Two-sided plumbing for authenticating RPCs:
 *
 *   - **Client**: `n00b_rpc_channel_set_auth` attaches per-channel
 *     credentials (bearer JWT + optional DPoP proof) and a policy id
 *     to the outbound request.  The runtime stamps every call with
 *     `authorization: Bearer <jwt>`, `dpop: <proof>`, and
 *     `n00b-rpc-policy: <id>`.
 *
 *   - **Server**: `n00b_rpc_server_attach_auth` plugs a manifest
 *     (`auth.policies[]` + `rpc.services[]`) and an audit-sink
 *     (Phase 3) into the dispatcher.  Each inbound RPC is gated by
 *     the manifest-pinned policy for the matching service.  A
 *     per-call `n00b-rpc-policy` header picks an alternate policy
 *     if and only if it is **at least as strict** as the pinned
 *     default; downgrades return `403 PERMISSION_DENIED`.
 *
 * If `n00b_rpc_server_attach_auth` is never called the dispatcher
 * runs unauthenticated (channel-level Phase-3 enforcement still
 * applies if the application set it on the channel).
 * =========================================================================== */

/**
 * @brief Credentials presented when authenticating an RPC channel
 *        or a single call.
 *
 * mTLS proof rides at the QUIC layer (peer cert), not here.  The
 * bearer + dpop fields are optional; either may be nullptr if the
 * matching policy doesn't require them.
 */
struct n00b_rpc_auth_credentials {
    const char          *bearer_token;   /**< Compact JWS (RFC 7519). */
    const char          *dpop_proof;     /**< DPoP proof (RFC 9449). */
    n00b_jwt_verifier_t *jwt_verifier;   /**< Server-supplied; may be
                                          *   nullptr if the runtime
                                          *   resolves verifiers via
                                          *   manifest IdPs. */
};

/**
 * @brief Attach client-side auth credentials + a default policy id
 *        to an RPC channel.
 *
 * Once attached, every outbound call from this channel carries:
 *   - `authorization: Bearer <bearer_token>` (if non-null);
 *   - `dpop: <dpop_proof>` (if non-null);
 *   - `n00b-rpc-policy: <policy_id>` (if non-null).
 *
 * Pass nullptr `creds` and `policy_id` to clear.  The strings
 * referenced by @p creds are duplicated into the channel's
 * conduit_pool storage; callers may free their copies after this
 * returns.
 */
extern void
n00b_rpc_channel_set_auth(n00b_rpc_channel_t                *chan,
                          const n00b_rpc_auth_credentials_t *creds,
                          const char                        *policy_id);

/**
 * @brief Attach the manifest + audit sink to a server-side RPC
 *        dispatcher.
 *
 * After this call, every inbound RPC is gated by the pinned policy
 * named in `mf->rpc_services[<service>].auth_policy` (if any).
 * Per-call `n00b-rpc-policy` headers are honored when at least as
 * strict as the pinned default (`require_dpop`, `require_mtls`, and
 * the full `required_claims[]` set from the pinned policy must all
 * appear in the override; the override may add but never drop).
 *
 * Audit emission: every dispatch (allow OR deny) emits one event
 * via the Phase 3 audit subscriber registry.  Subscribe via
 * `n00b_quic_audit_subscribe` (see `quic/audit.h`).  When @p
 * stderr_fallback is true and no audit subscriber is registered,
 * decisions are also logged to stderr.
 *
 * @param s                Server handle (from
 *                         `n00b_rpc_attach_server`).  Borrowed.
 * @param mf               Parsed manifest (`auth.idps[]` +
 *                         `auth.policies[]` + `rpc.services[]`).
 *                         Borrowed; must outlive @p s.
 * @param stderr_fallback  When true, emit a stderr line in
 *                         addition to the audit topic (handy
 *                         during bring-up).
 *
 * @return ok(true) on success; err(NULL_ARG) on null inputs;
 *         err(PROTOCOL) when the manifest references undefined
 *         policies.
 */
extern n00b_result_t(bool)
n00b_rpc_server_attach_auth(n00b_rpc_server_t    *s,
                            n00b_quic_manifest_t *mf,
                            bool                  stderr_fallback);

/**
 * @brief Test/inspection: install a custom JWT verifier resolver.
 *
 * The default resolver is "no resolver, server runs unauthenticated".
 * Tests + applications using a synthetic IdP (or any out-of-band
 * verifier source) install a resolver that maps an IdP id (the
 * `idp` field on `n00b_quic_manifest_policy_t`) to a
 * pre-configured verifier.  Borrowed; lives at process scope.
 */
/* Phase 5 § 5.3 — the resolver receives the inbound request headers
 * so multi-tenant deployments can route on a tenant header
 * (e.g., `X-Tenant: alpha`) without requiring per-tenant policies in
 * the manifest.  Single-tenant deployments may ignore @p hdrs / @p n_hdrs.
 *
 * @p idp_id is the manifest-resolved IdP id from the applied policy
 * (post per-call override).  When the resolver returns nullptr, the
 * runtime treats the call as `UNAUTHENTICATED`. */
typedef n00b_jwt_verifier_t *(*n00b_rpc_verifier_resolver_fn)(
    const char             *idp_id,
    const n00b_h3_header_t *hdrs,
    size_t                  n_hdrs,
    void                   *user_ctx);

extern void
n00b_rpc_server_set_verifier_resolver(n00b_rpc_server_t              *s,
                                      n00b_rpc_verifier_resolver_fn   fn,
                                      void                           *user_ctx);

/**
 * @brief Phase 5 § 5.1 — attach a Prometheus registry.
 *
 * Registers `n00b_quic_rpc_calls_total{service,method,status}` and
 * `n00b_quic_rpc_call_duration_us` (histogram) on @p r.  Subsequent
 * dispatches increment / observe respectively.
 *
 * Idempotent against a second call with the same registry; calling
 * with a different registry overwrites the handles (last writer
 * wins).  Pass nullptr to skip.
 */
extern void
n00b_rpc_server_attach_metrics(n00b_rpc_server_t           *s,
                               n00b_quic_metric_registry_t *r);

/**
 * @brief Compare two manifest-defined policies for "at least as
 *        strict as" semantics.
 *
 * @param base       The pinned default (service-level).  All its
 *                   constraints must be present in @p candidate.
 * @param candidate  The proposed override.
 *
 * @return true iff @p candidate is at least as strict as @p base
 *         (both nullptr or @p base nullptr → true; @p candidate
 *         nullptr but @p base not → false).
 *
 * @details A policy is at-least-as-strict if:
 *   - `require_dpop` (if set on base) is also set on candidate;
 *   - `require_mtls` (if set on base) is also set on candidate;
 *   - every entry in base.required_claims[] is matched by an entry
 *     in candidate.required_claims[] with the same name+op+value;
 *   - the audience requirement (if set on base) matches the
 *     candidate's audience exactly.
 */
extern bool
n00b_rpc_policy_at_least_as_strict(const n00b_quic_manifest_policy_t *base,
                                   const n00b_quic_manifest_policy_t *candidate);
