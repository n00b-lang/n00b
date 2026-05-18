# RPC over QUIC — User Guide (Phase 4)

n00b's RPC layer rides H3-over-QUIC with CBOR-encoded payloads.
Service authors write **plain C functions** with an `@rpc("...")`
annotation; ncc generates client stubs + server dispatchers.

This guide is the user-facing reference.  See
`docs/net/quic/rpc_design.md` for internals + wire format, and
`~/dd/quic_4.md` for the design rationale.

## TL;DR

```c
/* server: declare + implement an annotated function. */
extern n00b_result_t(GreetReply *)
greet_hello(GreetRequest *req, n00b_rpc_ctx_t *ctx)
    @rpc("greet.v1.Greeter/Hello");

n00b_result_t(GreetReply *)
greet_hello(GreetRequest *req, n00b_rpc_ctx_t *ctx) {
    GreetReply *r = n00b_alloc(GreetReply);
    r->message = n00b_string_format("Hello, %s!", req->name);
    return n00b_result_ok(GreetReply *, r);
}
```

```c
/* client: just call the generated stub. */
n00b_rpc_ctx_t *ctx = n00b_rpc_ctx_with_deadline(nullptr, 5 * 1000 * 1000 * 1000LL);
GreetRequest req = { .name = n00b_cstr("alice") };
auto resp = n00b_rpc_call_greet_v1_Greeter_Hello(ctx, chan, &req);
if (n00b_result_is_ok(resp)) { /* ... */ }
```

## Annotation grammar

```c
extern <return-type>
<function-name>(<args...>, n00b_rpc_ctx_t *ctx)
    @rpc("<package>.<service>/<method>");
```

The annotation's quoted string is the on-the-wire identifier.
It's independent of the C function name (which can follow project
conventions).

The last argument MUST be `n00b_rpc_ctx_t *ctx`.  Server handlers
receive it from the runtime; clients pass their own.

## RPC patterns

### Unary (request/response)

```c
extern n00b_result_t(Reply *)
foo_call(Request *req, n00b_rpc_ctx_t *ctx)
    @rpc("foo.v1.Foo/Call");
```

One request → one response.  Most common shape.

### Server-streaming

```c
extern n00b_result_t(n00b_rpc_stream_t(Notification) *)
admin_subscribe(SubscribeReq *req, n00b_rpc_ctx_t *ctx)
    @rpc("admin.v1.Admin/Subscribe");

/* Server handler: build a stream, push items, close when done. */
n00b_result_t(n00b_rpc_stream_t(Notification) *)
admin_subscribe(SubscribeReq *req, n00b_rpc_ctx_t *ctx) {
    auto out = n00b_rpc_stream_new(Notification);
    for (int i = 0; i < req->count; i++) {
        Notification *n = build_notification(i);
        n00b_rpc_stream_send(out, n);
    }
    n00b_rpc_stream_close(out);
    return n00b_result_ok(n00b_rpc_stream_t(Notification) *, out);
}

/* Client: iterate the response stream until end-of-stream. */
auto r = n00b_rpc_call_admin_v1_Admin__Subscribe(ctx, chan, &req);
auto stream = n00b_result_get(r);
while (true) {
    auto item = n00b_rpc_stream_recv(stream);
    if (n00b_result_is_err(item)) {
        if (n00b_result_get_err(item) == N00B_QUIC_ERR_NEED_MORE_DATA)
            continue;          /* poll again */
        break;                 /* peer-side abort; check status */
    }
    Notification *n = n00b_result_get(item);
    if (!n) break;             /* clean end-of-stream */
    handle_notification(n);
}
```

One request → N responses streamed back over the same QUIC stream.
`n00b_rpc_stream_t(T)` is a typed FIFO + close-state holder; on the
server side, emit values via `n00b_rpc_stream_send(stream, val)`;
on the client side, drain via `n00b_rpc_stream_recv(stream)`.

### Client-streaming

```c
extern n00b_result_t(UploadResponse *)
upload_chunks(n00b_rpc_stream_t(Chunk) *chunks, n00b_rpc_ctx_t *ctx)
    @rpc("storage.v1.Storage/Upload");

/* Server: drain `chunks` until end-of-stream, return the summary. */
n00b_result_t(UploadResponse *)
upload_chunks(n00b_rpc_stream_t(Chunk) *chunks, n00b_rpc_ctx_t *ctx) {
    UploadResponse *r = n00b_alloc(UploadResponse);
    while (true) {
        auto item = n00b_rpc_stream_recv(chunks);
        if (n00b_result_is_err(item)) {
            if (n00b_result_get_err(item) == N00B_QUIC_ERR_NEED_MORE_DATA)
                continue;
            return n00b_result_err(UploadResponse *, N00B_RPC_INTERNAL);
        }
        Chunk *c = n00b_result_get(item);
        if (!c) break;
        r->bytes_total += c->len;
    }
    return n00b_result_ok(UploadResponse *, r);
}

/* Client: build a stream of chunks, FIN by closing it. */
auto in = n00b_rpc_stream_new(Chunk);
for (size_t i = 0; i < n_chunks; i++) {
    n00b_rpc_stream_send(in, &chunks[i]);
}
n00b_rpc_stream_close(in);
auto resp = n00b_rpc_call_storage_v1_Storage__Upload(ctx, chan, in);
```

N requests → one response.  The server's handler reads `chunks`
until end-of-stream (recv returns ok(nullptr)), then returns the
single summary response.

### Bidirectional

```c
extern n00b_result_t(n00b_rpc_stream_t(ServerMsg) *)
chat_session(n00b_rpc_stream_t(ClientMsg) *in, n00b_rpc_ctx_t *ctx)
    @rpc("chat.v1.Chat/Session");

/* Server handler returns immediately with an outbound stream;
 * a worker drains `in` and pushes responses concurrently. */
n00b_result_t(n00b_rpc_stream_t(ServerMsg) *)
chat_session(n00b_rpc_stream_t(ClientMsg) *in, n00b_rpc_ctx_t *ctx) {
    auto out = n00b_rpc_stream_new(ServerMsg);
    spawn_chat_worker(in, out);   /* drains in → builds out */
    return n00b_result_ok(n00b_rpc_stream_t(ServerMsg) *, out);
}
```

Both directions stream independently.  Either side may FIN its
direction before the other; the runtime honors half-closed states.

### `n00b_rpc_stream_t(T)` API

| Operation                                   | Semantics |
|---------------------------------------------|-----------|
| `n00b_rpc_stream_send(s, item)`             | Push.  Returns `result(bool)` — err if closed. |
| `n00b_rpc_stream_close(s)`                  | Clean end-of-stream.  Idempotent. |
| `n00b_rpc_stream_close_err(s, status)`      | Err close.  `status` is mapped to wire. |
| `n00b_rpc_stream_recv(s)`                   | Pop.  ok(item), ok(nullptr) at EOS, err on remote abort, err(NEED_MORE_DATA) when empty + open. |
| `n00b_rpc_stream_is_closed(s)`              | Predicate. |
| `n00b_rpc_stream_status(s)`                 | Last close status (`N00B_RPC_OK` if clean). |

The wire layer carries CBOR-encoded items: each `n00b_rpc_stream_send`
becomes one (or part of one) H3 DATA frame on the underlying QUIC
stream.  H3 doesn't preserve frame boundaries semantically — the
runtime reassembles bytes, decodes one CBOR item at a time, and
pushes onto the recipient's stream.

## Cancellation + deadlines

`n00b_rpc_ctx_t` carries deadline + cancel state with cascading
propagation.

```c
/* Root context with no deadline. */
n00b_rpc_ctx_t *root = n00b_rpc_ctx_new();

/* Derived ctx with a 5-second deadline (relative). */
int64_t deadline_ns = n00b_ns_timestamp() + 5LL * 1000 * 1000 * 1000;
n00b_rpc_ctx_t *call = n00b_rpc_ctx_with_deadline(root, deadline_ns);

auto r = n00b_rpc_call_foo(call, chan, &req);

/* Cancellable but no deadline (e.g., user clicked stop). */
n00b_rpc_ctx_t *cancellable = n00b_rpc_ctx_with_cancel(root);
/* ... start a background call ... */
n00b_rpc_ctx_cancel(cancellable);  /* aborts in-flight call */
```

### Cascading

A child context inherits cancellation:

```c
/* Server handler: */
n00b_result_t(Reply *)
foo_call(Request *req, n00b_rpc_ctx_t *ctx) {
    /* Cascade: outbound call inherits remaining time + cancellation. */
    auto child_resp = n00b_rpc_call_bar(ctx, downstream_chan, &child_req);
    /* If client cancels OR ctx's deadline expires, child_resp returns
     * N00B_RPC_CANCELLED or N00B_RPC_DEADLINE_EXCEEDED. */
}
```

Cancelling a parent cancels all descendants.  Deadline propagation
is conservative: a child's effective deadline is `min(parent's
remaining, child's requested)`.

### Server-side bail-out

A long-running handler can check the ctx and bail early:

```c
n00b_result_t(Reply *)
slow_handler(Request *req, n00b_rpc_ctx_t *ctx) {
    for (int i = 0; i < req->iterations; i++) {
        if (n00b_rpc_ctx_is_cancelled(ctx)) {
            return n00b_result_err(Reply *, N00B_RPC_CANCELLED);
        }
        do_work(i);
    }
    /* ... */
}
```

`n00b_rpc_ctx_remaining_ns(ctx)` gives the time-budget remaining
(or -1 if no deadline).

## Auth

Auth wiring is two-sided: the **client** stamps every outbound
call with bearer + DPoP + policy headers; the **server**
consults a manifest (`auth.policies[]` + `rpc.services[]`) to
decide whether to dispatch.

### Server side

After attaching the dispatcher with `n00b_rpc_attach_server`,
plug in the manifest + an IdP-to-verifier resolver:

```c
n00b_quic_manifest_t *mf = n00b_quic_manifest_load_path("manifest.json");
n00b_rpc_server_t    *s  = n00b_rpc_attach_server(h3_server, conduit);

(void)n00b_rpc_server_attach_auth(s, mf, /*stderr_fallback=*/false);
n00b_rpc_server_set_verifier_resolver(s, my_resolver, &my_idp_table);
```

Each inbound RPC is matched against `rpc.services[<service>]`'s
pinned `auth_policy`; that policy's claims/audience/dpop/mtls
constraints gate the call.  On allow, the registered handler
runs; on deny, the runtime responds `:status=4xx` +
`n00b-rpc-status=…` without invoking the handler.

`n00b_rpc_server_attach_auth` also emits the same auth events
to `stderr` (one line per dispatch) when its third argument is
`true` — handy during bring-up.

### Client side (per-channel default)

Stamp the channel with credentials + a default policy id:

```c
n00b_rpc_auth_credentials_t creds = {
    .bearer_token = jwt,             /* compact JWS */
    .dpop_proof   = dpop_header,     /* optional */
};
n00b_rpc_channel_set_auth(chan, &creds, "rpc-write");
```

Every outbound call now carries:

| Header             | Value                                    |
|--------------------|------------------------------------------|
| `authorization`    | `Bearer <jwt>`                           |
| `dpop`             | `<dpop_proof>` (only if non-null)        |
| `n00b-rpc-policy`  | `rpc-write` (the channel default)        |

### Per-call override

Each call function (`n00b_rpc_call_unary`,
`n00b_rpc_call_server_stream`, `n00b_rpc_call_client_stream`,
`n00b_rpc_call_bidi`) accepts two optional kw-args that override
the channel default for THIS call only — channel state is not
mutated.

| kw-arg            | Type                                  | Effect |
|-------------------|---------------------------------------|--------|
| `creds_override`  | `const n00b_rpc_auth_credentials_t *` | Substitutes `bearer_token` + `dpop_proof` for one call |
| `policy_override` | `const char *`                        | Substitutes `n00b-rpc-policy` header value for one call |

```c
/* Per-call: a privileged write call from an otherwise read-only
 * channel.  The override carries a stricter token + a stricter
 * policy id; the channel's default credentials apply unchanged
 * to subsequent calls. */
n00b_rpc_auth_credentials_t admin = { .bearer_token = admin_jwt };
auto r = n00b_rpc_call_unary(ctx, chan, "checkout.v1.Checkout/Confirm",
                             req_cbor,
                             .creds_override  = &admin,
                             .policy_override = "rpc-write");
```

The server still applies the at-least-as-strict check (§ 7.3 of
`rpc_design.md`): any override that's at least as strict (same
`require_dpop` / `require_mtls`, superset of `required_claims[]`,
same audience pin) is accepted; downgrades are rejected with
`PERMISSION_DENIED` before the handler runs.

Pre-flight: a `creds_override` with a NULL/empty `bearer_token`,
or a configured policy id with no bearer available from any
source, returns `N00B_RPC_UNAUTHENTICATED` synchronously without
issuing the request.

### Failure modes

- Token missing / invalid / expired → `:status=401` +
  `n00b-rpc-status=UNAUTHENTICATED`.
- Per-call override weaker than the service-pinned default →
  `:status=403` + `n00b-rpc-status=PERMISSION_DENIED`.
- Audit topic emits one event per dispatch (allow OR deny);
  subscribe with `n00b_quic_audit_subscribe` (see
  `docs/net/quic/auth.md`).

## Status codes

```c
typedef enum : int32_t {
    N00B_RPC_OK                  = 0,
    N00B_RPC_CANCELLED           = 1,
    N00B_RPC_UNKNOWN             = 2,
    N00B_RPC_INVALID_ARGUMENT    = 3,
    N00B_RPC_DEADLINE_EXCEEDED   = 4,
    N00B_RPC_NOT_FOUND           = 5,
    N00B_RPC_ALREADY_EXISTS      = 6,
    N00B_RPC_PERMISSION_DENIED   = 7,
    N00B_RPC_RESOURCE_EXHAUSTED  = 8,
    N00B_RPC_FAILED_PRECONDITION = 9,
    N00B_RPC_ABORTED             = 10,
    N00B_RPC_OUT_OF_RANGE        = 11,
    N00B_RPC_UNIMPLEMENTED       = 12,
    N00B_RPC_INTERNAL            = 13,
    N00B_RPC_UNAVAILABLE         = 14,
    N00B_RPC_DATA_LOSS           = 15,
    N00B_RPC_UNAUTHENTICATED     = 16,
} n00b_rpc_status_t;
```

Numeric values match gRPC's status codes for ease of bridging.

## Type support (CBOR)

Anything the n00b marshaler can walk, the CBOR codec can encode:

| n00b type            | Encodes as                              |
|----------------------|-----------------------------------------|
| `int64_t`            | CBOR unsigned / negative integer        |
| `bool`               | CBOR simple value                       |
| `double`             | CBOR float                              |
| `n00b_string_t *`    | CBOR text string                        |
| `n00b_buffer_t *`    | CBOR byte string                        |
| `n00b_list_t(T) *`   | CBOR array                              |
| `n00b_dict_t<K,V> *` | CBOR map                                |
| `n00b_option_t(T)`   | CBOR null or T                          |
| `n00b_result_t(T)`   | CBOR tagged union                       |
| `n00b_bigint_t *`    | CBOR tagged bignum                      |
| `n00b_time_t`        | CBOR tagged date-time                   |

Custom structs the marshaler handles (most n00b types) round-trip
automatically.

## Manifest registration

Services declare themselves in the deployment manifest so they
inherit auth policies + appear in preflight reports:

```json
{
  "auth": {
    "idps": [{
      "id": "primary",
      "issuer": "https://login.example.com"
    }],
    "policies": [{
      "id": "rpc-readwrite",
      "idp": "primary",
      "audience": "checkout-api",
      "require_dpop": true
    }]
  },
  "rpc": {
    "services": [{
      "id": "checkout.v1.Checkout",
      "auth_policy": "rpc-readwrite"
    }]
  }
}
```

Preflight verifies each service's `auth_policy` reference resolves.

## Audit

Every RPC start emits one audit event (allow or deny).  Subscribe
via `n00b_quic_audit_subscribe` (Phase 3) or use the built-in
JSONL sink:

```c
auto sink = n00b_quic_audit_jsonl_sink_open("/var/log/n00b/audit.jsonl");
```

Event JSON shape (extends the Phase 3 base):

```json
{
  "ts_ms": 1234567890000,
  "decision": "allow",
  "reason": "ok",
  "iss": "https://idp.example",
  "sub": "alice",
  "aud": "checkout-api",
  "policy_id": "rpc-readwrite",
  "rpc_method": "checkout.v1.Checkout/Confirm",
  "rpc_deadline_ms": 5000
}
```

## Generated symbols

Each `@rpc("<package>.<service>/<method>")` annotation expands into
**three** symbols emitted by ncc into the user's TU.  Knowing the
naming rule is useful for:

- Calling the client stub from another C file (its name is fixed).
- Reading backtraces / linker errors that mention the dispatcher or
  the constructor.
- Auditing which methods a TU has registered (grep for
  `_n00b_rpc_register__`).

| Generated symbol                                   | Visibility       | Emitted at  |
|----------------------------------------------------|------------------|-------------|
| `_n00b_rpc_dispatch__<svc>__<method>`              | static           | definition  |
| `_n00b_rpc_register__<svc>__<method>` (constructor)| static + ctor    | definition  |
| `n00b_rpc_call_<svc>__<method>`                    | extern (public)  | declaration |

### Mangling rule

`<svc>` is the dotted package + service name with **dots replaced
by single underscores**.  `<method>` is the verbatim method name
from the annotation (everything after the slash).  The slash
separator becomes a **double underscore** (`__`).

| @rpc string                              | Generated mangled tail               |
|------------------------------------------|--------------------------------------|
| `"checkout.v1.Checkout/Confirm"`         | `checkout_v1_Checkout__Confirm`      |
| `"greet.v1.Hello/Greet"`                 | `greet_v1_Hello__Greet`              |
| `"admin.v1.Admin/Subscribe"`             | `admin_v1_Admin__Subscribe`          |
| `"a.b/X"`                                | `a_b__X`                             |

### Header vs. definition asymmetry

A pure forward declaration with `@rpc(...)` (the header path) emits
**only an extern prototype** for the client stub — no body.  The
definition with `@rpc(...)` (the .c path) emits the **dispatcher +
constructor + the client stub body**.

This means: putting `@rpc(...)` on the user-handler declaration in a
header file is the recommended pattern.  Many translation units may
include the same header — each sees only the prototype, so there are
no duplicate-symbol errors at link.  The body lives in the
implementing TU (along with the dispatcher and `__attribute__((constructor))`
registration), so the symbol is defined exactly once per program.

### Restrictions in v0

- The annotated function must take a `n00b_rpc_ctx_t *` parameter
  (typically the last).
- The return type must be `n00b_result_t(...)`.
- `static` functions cannot be `@rpc`-annotated — the client stub
  must be externally linkable.
- `_kargs` and `@rpc` cannot coexist on the same function (parse-
  time error).
- Method-string format is `<package>.<service>/<method>` with **one
  slash**; package, service, and method must be valid C identifier
  components (`[A-Za-z_][A-Za-z0-9_]*`); the package may be dotted
  (`pkg.sub.deeper`).

## Examples

`examples/quic_rpc_demo/main.c` is the canonical "hello world":
spawns a server with two annotated functions, opens a client, calls
them, exits.  Cross-references both per-channel and per-call auth.

### Worked example: `quic_rpc_demo`

The [`examples/quic_rpc_demo/`](../../../examples/quic_rpc_demo/)
directory contains a self-contained demo exercising the full Phase 4
stack:

- `greet.h` — request/response types + extern declarations of the
  ncc-generated client stubs.  The `@rpc(...)` annotations live in
  `greet.c` (the implementation TU); per `rpc.md` § Generated symbols,
  we re-declare the stubs by their mangled names (`n00b_rpc_call_<svc>__<method>`)
  here so multiple TUs can call them without re-emitting the body.
- `greet.c` — the two annotated handlers (`@rpc("greet.v1.Greeter/Hello")`
  unary, `@rpc("greet.v1.Greeter/Stream")` server-stream), plus the
  per-type CBOR encode/decode hooks the dispatcher + client stubs
  reference.  Constructor-time registration parks the dispatchers in a
  pending queue (since `n00b_init` hasn't run yet); the queue replays
  on first registry access from inside `main`.
- `main.c` — `--server`, `--client`, and `--loopback` modes.  Wires a
  manifest-driven `n00b_rpc_server_attach_auth` policy + the
  in-process synthetic IdP fixture so the demo boots without any
  external OIDC infrastructure.  Trust uses `n00b_quic_trust_pinned`
  with the SHA-256 fingerprint of the test PKI cert — no `--insecure`
  mode.

Run it:

```sh
bash build.sh
./build_debug/quic_rpc_demo --loopback
```

Expected output (audit lines on stderr; data lines on stdout):

```
loopback: server up on 127.0.0.1:<port>
[audit] ALLOW greet.v1.Greeter/Hello policy=rpc-default sub=alice ...
Hello reply: Hello, alice!
Stream item 1: tick 1
Stream item 2: tick 2
Stream item 3: tick 3
Stream item 4: tick 4
Stream item 5: tick 5
```

The `examples/quic_rpc_demo/README.md` covers the production swap
(real OIDC + system trust) and the standalone server/client modes.

## What's NOT shipping in v1

- gRPC wire compatibility (we use CBOR, not protobuf).
- 0-RTT.
- Service reflection / dynamic schema.
- HTTP/2 fallback.
- WebTransport.
- Push streams from the RPC layer (H3 has them; RPC doesn't expose).

These are documented as deferrals in `~/dd/quic_4.md` § 2.
