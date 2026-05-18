# RPC over QUIC — Design + Wire Format Reference

Internals reference for `libn00b_rpc`.  Companion to
[`rpc.md`](rpc.md) (user guide) and `~/dd/quic_4.md`
(design rationale).  Read this when you're modifying the RPC
layer, debugging a wire trace, or bridging to another stack.

---

## 1. Layering

```
+---------------------------------------------+
|  Application: handler returning             |
|     n00b_result_t(Reply *)                  |
+---------------------------------------------+
|  ncc-generated stubs (client + dispatcher)  |  Phase 4 § 4.5–4.7
+---------------------------------------------+
|  RPC runtime: ctx, status, dispatch table,  |  Phase 4 § 4.6–4.10
|  auth wiring, audit                         |
+---------------------------------------------+
|  CBOR codec (RFC 8949)                      |  Phase 4 § 4.1
+---------------------------------------------+
|  HTTP/3 (RFC 9114) + QPACK (RFC 9204)       |  Phase 4 § 4.3, 4.2
+---------------------------------------------+
|  QUIC transport (picoquic)                  |  Phase 1
+---------------------------------------------+
```

Every RPC is one HTTP/3 request on one bidi QUIC stream.  Headers
are QPACK-encoded.  Bodies are CBOR maps.  Streaming variants
re-use the same stream — the body is a sequence of CBOR items
rather than a single map.

---

## 2. Stream lifecycle

### 2.1 Unary

```
client                                                    server
  |                                                          |
  |-- HEADERS  (QPACK: pseudo + n00b-rpc-* + auth) --------->|
  |-- DATA     (CBOR-encoded request, FIN) ---------------->|
  |                                                          |
  |<-- HEADERS (status + n00b-rpc-status[-detail]) ----------|
  |<-- DATA    (CBOR-encoded response, FIN) ----------------|
```

`FIN` on either side closes that direction.  Status headers ride
on the response HEADERS frame (gRPC-style trailers are not used —
H3 doesn't require a separate trailers frame for this and bundling
keeps clients simpler).

### 2.2 Server-streaming

Client sends one request (HEADERS + DATA + FIN).  Server emits
HEADERS once, then a sequence of DATA frames each carrying one
CBOR item, then FIN.

```c
auto r = n00b_rpc_call_server_stream(ctx, chan, "svc/Method", req);
auto stream = n00b_result_get(r);
while (true) {
    auto item = n00b_rpc_stream_recv(stream);
    if (n00b_result_is_err(item)) break;        // remote abort or NEED_MORE_DATA
    n00b_buffer_t *cbor = n00b_result_get(item);
    if (!cbor) break;                           // clean end-of-stream
    /* decode + handle */
}
```

### 2.3 Client-streaming

Client emits HEADERS once, then a sequence of DATA frames each
carrying one CBOR item, then FIN.  Server replies with HEADERS +
DATA + FIN once the stream is half-closed.

```c
auto in = n00b_rpc_buffer_stream_new();
for (size_t i = 0; i < n; i++) {
    n00b_rpc_stream_send(in, build_item(i));
}
n00b_rpc_stream_close(in);                      // FINs the request stream
auto r = n00b_rpc_call_client_stream(ctx, chan, "svc/Method", in);
n00b_buffer_t *resp_cbor = n00b_result_get(r);
```

### 2.4 Bidirectional

Both sides send DATA frames freely after their HEADERS.  Either
side may FIN its direction independently.  Headers (status etc.)
are emitted by the **server** at the start; trailing status, if
any, would require a second HEADERS frame and is not currently
used.

```c
auto in  = n00b_rpc_buffer_stream_new();
auto r   = n00b_rpc_call_bidi(ctx, chan, "svc/Method", in);
auto out = n00b_result_get(r);
/* Push to `in` and drain from `out` concurrently — each direction
 * has its own pump thread inside the runtime. */
```

---

## 3. Header conventions

### 3.1 Pseudo-headers (HTTP/3)

| Header     | Direction | Value                                |
|------------|-----------|--------------------------------------|
| `:method`  | request   | always `POST`                        |
| `:scheme`  | request   | always `https`                       |
| `:authority` | request | server hostname (SNI)                |
| `:path`    | request   | `/<package>.<service>/<method>`      |
| `:status`  | response  | coarse HTTP status; see § 5          |

The path encodes the on-wire RPC identifier, e.g.
`/checkout.v1.Checkout/Charge`.  This matches gRPC's convention so
existing tooling (Wireshark dissectors, traffic shapers) reads it.

### 3.2 Content negotiation

| Header               | Direction | Value             |
|----------------------|-----------|-------------------|
| `content-type`       | both      | `application/cbor` |
| `n00b-rpc-encoding`  | both      | `cbor` (informational; reserved for marshaler-backed codecs in Phase 6) |

Content-type is `application/cbor` rather than gRPC's
`application/grpc+cbor` because the n00b RPC layer is **not**
gRPC framing.  Bridge proxies should rewrite if needed.

### 3.3 RPC-specific headers

| Header                    | Direction | Required | Purpose |
|---------------------------|-----------|----------|---------|
| `n00b-rpc-status`         | response  | yes      | numeric status code (see § 5) |
| `n00b-rpc-status-detail`  | response  | optional | free-form text; never trusted as machine-readable |
| `n00b-rpc-deadline-ms`    | request   | optional | absolute deadline in ms since unix epoch (UTC) |
| `n00b-rpc-cancel`         | client→server signal | optional | sent in a follow-up HEADERS when client wants the server to abort; alternative to RESET_STREAM |
| `n00b-rpc-trace-id`       | both      | optional | opaque tracing correlator |
| `n00b-rpc-idempotency-key` | request | optional | client-chosen UUID for safe retries on idempotent methods |

Status fields are `:status` (HTTP coarse) + `n00b-rpc-status`
(numeric, gRPC-aligned).  Both are always present on a normal
response; transport-layer aborts use RESET_STREAM with a QUIC
error code instead.

### 3.4 Auth headers

| Header                    | Direction | Purpose |
|---------------------------|-----------|---------|
| `authorization`           | request   | `Bearer <jwt>` (Phase 3) |
| `dpop`                    | request   | DPoP proof header (RFC 9449) |
| `n00b-rpc-policy`         | request   | policy id (per-call override; otherwise inherits per-channel default) |

Auth wiring is described in `~/dd/quic_3.md` § 7 and 8.  Phase 4
adds the **per-call policy override**: a service's manifest entry
pins a default `auth_policy`, and a caller may request a tighter
policy for a single call (downgrades are rejected).

---

## 4. CBOR body conventions

Bodies are always CBOR maps with **string keys**.  Field names match
the C struct field names.  Type mapping is the table in
`docs/net/quic/cbor.md` § 3.

Four hardening rules apply at decode time, enforced by
`n00b_cbor_decode_strict` (sub-phase 4.7):

1. **No CBOR tags except those listed** (datetime tag 0/1, bignum
   tag 2/3, n00b result tag 27/28).  Unknown tag → `INVALID_ARGUMENT`.
2. **No indefinite-length items.**  The base decoder rejects them at
   any nesting depth (via the `ai == 31` check in `read_head`).
3. **No duplicate map keys.**  RFC 8949 § 5.6 — strict decoders treat
   duplicates as protocol errors rather than last-write-wins.
4. **Maximum nesting depth: 32.**  Beyond → `INVALID_ARGUMENT`.

The runtime calls `n00b_cbor_decode_strict_bytes` for every inbound
CBOR item on streaming RPCs (and for the outer body of unary calls);
the default tag allowlist is the small RPC set above.  Pass an
explicit `n00b_cbor_strict_opts_t` with a custom `tag_allowlist` to
extend (or shrink) the accepted tag set per-method.

```c
const uint64_t my_tags[] = { 0, 1, 27, 28 };  // no bignums
n00b_cbor_strict_opts_t opts = {
    .tag_allowlist     = my_tags,
    .tag_allowlist_len = sizeof(my_tags) / sizeof(my_tags[0]),
};
auto r = n00b_cbor_decode_strict(body, &opts);
```

Empty bodies (zero-length DATA frames before FIN) are valid and
decode to "no fields set" — convenient for void-returning RPCs.

---

## 5. Status codes

The gRPC-numeric enum lives in `include/net/quic/rpc_status.h`.  The
HTTP coarse mapping (used for `:status`) is implemented in
`n00b_rpc_status_http_class()` and follows the gRPC-over-HTTP/2
convention:

| n00b-rpc-status     | numeric | :status |
|---------------------|--------:|--------:|
| OK                  | 0       | 200     |
| INVALID_ARGUMENT    | 3       | 400     |
| FAILED_PRECONDITION | 9       | 400     |
| OUT_OF_RANGE        | 11      | 400     |
| UNAUTHENTICATED     | 16      | 401     |
| PERMISSION_DENIED   | 7       | 403     |
| NOT_FOUND           | 5       | 404     |
| ALREADY_EXISTS      | 6       | 409     |
| ABORTED             | 10      | 409     |
| RESOURCE_EXHAUSTED  | 8       | 429     |
| CANCELLED           | 1       | 499     |
| UNIMPLEMENTED       | 12      | 501     |
| UNAVAILABLE         | 14      | 503     |
| DEADLINE_EXCEEDED   | 4       | 504     |
| UNKNOWN, INTERNAL, DATA_LOSS | 2/13/15 | 500 |

The `:status` is informational only; clients **must** read
`n00b-rpc-status` for the precise outcome (`:status` is what
intermediaries see).  Browsers, ad-hoc curl/wget, and grpc-web
proxies see something sensible without n00b-aware parsing.

### 5.1 Application errors vs transport errors

- **Application error** (handler returned a non-OK
  `n00b_result_t`): a normal response; `n00b-rpc-status` set to a
  non-zero value, `:status` set to the matching coarse HTTP code,
  optional `n00b-rpc-status-detail` text, normally with an empty
  body.
- **Transport error** (truncated body, codec refused, oversized
  frame, invalid auth): `RESET_STREAM` with the matching QUIC
  error code (see `n00b_rpc_status_from_quic_err()` for the
  reverse mapping a client should apply when synthesising a
  status from the QUIC code).

The split exists so middleboxes (proxies, load balancers) cannot
silently *upgrade* a legitimate "permission denied" to a
"connection failure" by mishandling status, and so clients can
distinguish "your request was rejected" from "your call did not
arrive".

---

## 6. Cancellation + deadlines

The runtime context lives in `include/net/quic/rpc_ctx.h`
(`n00b_rpc_ctx_t`).  Fan-in and propagation rules:

### 6.1 Client-initiated cancellation

```
n00b_rpc_ctx_cancel(ctx)
       │
       ▼
ctx state → CANCELLED  ─── n00b_futex_wake_all() ─── observers wake
       │
       ▼
runtime sends RESET_STREAM (preferred) OR HEADERS frame with
   n00b-rpc-cancel: 1 (when reset is impractical)
```

Both children of `ctx` (child contexts spawned for downstream calls
via `n00b_rpc_ctx_with_parent`) are cascaded synchronously
depth-first while their per-node locks are held.  See
`test/unit/test_quic_rpc_ctx.c` for the cascade ordering tests.

### 6.2 Deadline propagation

```
client: ctx with absolute deadline T
       │
       ▼ encoded into HEADERS as
          n00b-rpc-deadline-ms: T
       │
       ▼
server: dispatcher reads the header, adopts T as its handler ctx's
        deadline.  If T is in the past → reply DEADLINE_EXCEEDED
        without invoking the handler.
       │
       ▼
handler: any sub-RPC inherits the same T (clamped if it requests
         a tighter deadline; never extended).
```

Deadlines are **absolute** (ms since unix epoch UTC) so clock
skew between hops can't extend a deadline.  The runtime warns at
startup if NTP isn't running on the host.

### 6.3 Server-initiated abort

A server handler that calls `n00b_rpc_ctx_cancel(ctx)` causes the
runtime to send `RESET_STREAM` with `N00B_QUIC_ERR_PEER_RESET`
once the handler returns (or immediately if the handler exits
without writing any response data).  Pending response writes are
discarded.

---

## 7. Auth integration (Phase 3 → Phase 4 wiring) — **shipped**

Phase 4 sub-phase 4.9 wired Phase-3's policy + audit machinery into
the RPC dispatch path.  Two surfaces are added on top of `n00b_rpc.h`:

```c
n00b_rpc_channel_set_auth(chan, &creds, "policy-id");
n00b_rpc_server_attach_auth(server, manifest, /*stderr_fallback=*/false);
n00b_rpc_server_set_verifier_resolver(server, my_resolver, &ctx);
```

The first is client-side (stamps every outbound call with
`authorization: Bearer …`, `dpop: …`, and `n00b-rpc-policy: …`).
The second is server-side (consults the manifest's `auth.policies[]`
+ `rpc.services[]` to gate every inbound RPC).  The third installs
the IdP → JWT-verifier mapping (the runtime defers verifier
construction to the application; in production this is wired through
the OIDC discovery/JWKS-cache stack).

### 7.1 Per-channel default policy

Each channel may have a default policy id stamped on it via
`n00b_rpc_channel_set_auth`.  When set, every outbound call on
that channel carries `n00b-rpc-policy: <id>` — the server uses
that as the **per-call request**, evaluated against the
service-pinned policy as described in § 7.3.

### 7.2 Per-service policy

`rpc.services[]` (sub-phase 4.11) binds a service id (e.g.
`checkout.v1.Checkout`) to an `auth_policy` from
`auth.policies[]`.  This is the **pinned default**; if a request
carries no `n00b-rpc-policy:` header, the service-pinned policy
applies.  Preflight + `n00b_rpc_server_attach_auth` both verify
that every service's `auth_policy` resolves to a defined policy
id.

### 7.3 Per-call override (at-least-as-strict)

A request may carry `authorization: Bearer <jwt>`,
`dpop: <proof>`, and `n00b-rpc-policy: <id>` to override the
channel default for that one call.  The runtime accepts the
policy override only when it is **at least as strict** as the
service-pinned default; downgrades return `:status=403` +
`n00b-rpc-status=PERMISSION_DENIED` without invoking the
handler.

**Client surface (shipped).**  Each of the four call functions
(`n00b_rpc_call_unary`, `n00b_rpc_call_server_stream`,
`n00b_rpc_call_client_stream`, `n00b_rpc_call_bidi`) accepts:

| kw-arg            | Type                                  | Effect |
|-------------------|---------------------------------------|--------|
| `creds_override`  | `const n00b_rpc_auth_credentials_t *` | Substitutes bearer + DPoP for one call |
| `policy_override` | `const char *`                        | Substitutes `n00b-rpc-policy` for one call |

When non-NULL, each kw-arg replaces the matching channel-level
field for THIS request only; channel state is **not** mutated and
applies unchanged to subsequent calls.  The bearer header value
is heap-allocated from the rpc allocator (no truncation).

A pre-flight check returns `N00B_RPC_UNAUTHENTICATED`
synchronously when `creds_override` is non-NULL with an empty
bearer, or when a policy id is in play (per-call OR
channel-level) but no bearer is available from any source.

**Server-side strict-vs-loose comparison**
(`n00b_rpc_policy_at_least_as_strict`):

| Constraint                | "At least as strict" rule |
|---------------------------|---------------------------|
| `require_dpop`            | candidate must require if base requires |
| `require_mtls`            | candidate must require if base requires |
| `audience` (pinned on base) | candidate must pin the same audience |
| `issuer_override`         | candidate must pin the same issuer (if base did) |
| `required_claims[]`       | every base claim must be matched (name + op + value) in candidate |

The candidate may add MORE claims and/or require additional
factors; it just can't drop anything the base required.

### 7.4 Audit

Every dispatch emits exactly one audit event via the Phase-3
audit topic (`n00b_quic_audit_emit`) — allow OR deny.  Fields:
`timestamp_ms`, `policy_id` (effective policy after override
evaluation), `decision`, `reason_code` (= rpc-status on deny),
`htm`/`htu` (= "POST"/full method id).  An audit subscriber
(`n00b_quic_audit_subscribe`) is the interception point for
JSONL sinks, OTLP shippers, etc.

Phase-3's `n00b_quic_auth_policy_eval` itself also emits an
audit event for the inner policy decision; subscribers see one
event from the eval and a second from the dispatch.  Tests assert
on the dispatch event (the one with `policy_id` populated to the
applied policy).

When `n00b_rpc_server_attach_auth` is **not** called, the
dispatcher runs unauthenticated and the audit event is still
emitted (decision = ALLOW, policy_id = null).

---

## 8. Service registration (server side)

ncc-generated dispatcher tables live in a per-service C file
(`<service>_dispatch.c`) emitted next to the source.  At startup,
each service module calls `n00b_rpc_register_service(svc)` to
attach its dispatch table to the runtime's global service map.

```c
/* generated */
static const n00b_rpc_method_t methods[] = {
    { .id = "Charge",  .handler = checkout_v1_charge_dispatch,  .pattern = N00B_RPC_PATTERN_UNARY },
    { .id = "Stream",  .handler = checkout_v1_stream_dispatch,  .pattern = N00B_RPC_PATTERN_SERVER_STREAM },
    /* ... */
};
static const n00b_rpc_service_t svc = {
    .id      = "checkout.v1.Checkout",
    .methods = methods,
    .count   = sizeof(methods) / sizeof(methods[0]),
};
__attribute__((constructor))
static void register_(void) { n00b_rpc_register_service(&svc); }
```

The constructor pattern means a service registers itself merely
by being linked into the binary — no manifest cross-reference at
the C level.  The manifest's `rpc.services[]` is the **operator
view** and only exists to bind services to auth policies; service
*existence* is determined by what's been linked in.

Lookup is O(1) via a hash on the service id; methods within a
service are O(log n) via binary search on a sorted array (small
enough that a hash isn't worth it).

---

## 9. Backpressure surface

Flow control is per-stream (one stream per call).  The runtime
exposes:

- `n00b_rpc_ctx_writable_bytes(ctx)` — bytes the runtime can
  buffer before blocking.  In handler-on-server contexts, this is
  the **outbound** window of the response stream.
- A `writable_topic` published when peer credit opens up.
  Clients can subscribe to throttle their producers without
  polling.

Streaming RPCs honour backpressure naturally: writes block (or
return `N00B_QUIC_ERR_FLOW_BLOCKED`) when the window is closed.
Unary RPCs typically fit in one stream-window; bursts past
`max_field_section_size` require coordinating with the operator
(QPACK dynamic table sizing, see `docs/net/quic/qpack.md` § 6).

---

## 10. Idempotency + retries

Two retry-relevant headers:

- `n00b-rpc-idempotency-key`: client-chosen UUID.  Identical
  retries within an idempotency window (default 60s,
  service-configurable) MUST yield the same response — the runtime
  caches the result in a small per-service LRU.
- `n00b-rpc-trace-id`: opaque correlator; not interpreted by the
  runtime, surfaced in audit + qlog.

The server runtime never automatically retries inbound calls.
Clients are responsible for retry/backoff; the recommended
pattern is jittered exponential with `n00b-rpc-idempotency-key`
held constant across attempts.

---

## 11. Protocol constants reference

| Constant                            | Value          |
|-------------------------------------|----------------|
| ALPN                                | `n00b-rpc/1`   |
| Default max-frame-size              | 16 MiB         |
| Default header-list-size            | 64 KiB         |
| QPACK dynamic table (default)       | 4 KiB          |
| Idempotency cache window (default)  | 60 s           |
| Deadline header                     | unix-epoch ms  |
| Status header                       | gRPC-numeric   |

---

## 12. Test surface

| File                                        | Subject |
|---------------------------------------------|---------|
| `test/unit/test_quic_cbor.c`                | RFC 8949 vectors + n00b round-trip |
| `test/unit/test_quic_qpack.c`               | RFC 9204 examples + dynamic table |
| `test/unit/test_quic_rpc_ctx.c`             | cancel/deadline ctx primitives |
| `test/unit/test_quic_rpc_status.c`          | status name + http class + quic-err bridge |
| `test/unit/test_quic_rpc_unary.c`           | end-to-end unary round-trip; UNIMPLEMENTED, deadline, handler-error, client-cancel |
| `test/unit/test_quic_rpc_streaming.c`       | end-to-end server-stream / client-stream / bidi loopback; early-cancel; handler-error; half-close; strict-decode rejection |
| `test/unit/test_quic_manifest.c`            | rpc.services parse + preflight |
| `test/fuzz/fuzz_quic_cbor.c`                | strict-decode fuzz |
| `test/fuzz/fuzz_quic_qpack.c`               | qpack decoder fuzz |
| `examples/quic_rpc_demo/`                   | end-to-end smoke (Phase 4 § 4.12) |

---

## 13. Cross-references

- `~/dd/quic_4.md` — design rationale; this doc is the
  implementation-bound reference.
- `docs/net/quic/cbor.md` — CBOR codec design + type mapping.
- `docs/net/quic/qpack.md` — QPACK encoder/decoder + dynamic table.
- `docs/net/quic/rpc.md` — user-facing service-author guide.
- `docs/net/quic/auth.md` — auth policy model (Phase 3).
- `docs/net/quic/manifest.md` — manifest + preflight (Phase 2 + 4.11).
- gRPC status codes — <https://grpc.io/docs/guides/status-codes/>
- RFC 9114 (HTTP/3), RFC 9204 (QPACK), RFC 8949 (CBOR), RFC 9449 (DPoP).
