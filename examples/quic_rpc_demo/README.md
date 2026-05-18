# quic_rpc_demo — End-to-end H3+CBOR+@rpc demo

The "hello world" of n00b's Phase 4 RPC stack.  Defines four annotated
RPCs — one per RPC shape — builds a server that registers them, and a
client that calls each with auth + deadline + cancellation context
plumbed through.

## What it shows

- `@rpc("greet.v1.Greeter/Hello")` — **unary**.  `GreetRequest{name}`
  → `GreetReply{message}`.
- `@rpc("greet.v1.Greeter/Stream")` — **server-stream**.
  `StreamRequest{count}` → N × `StreamItem{i, text}`.
- `@rpc("greet.v1.Greeter/Upload")` — **client-stream**.  Client
  pushes N × `ChunkRequest{data}` and FINs; server replies with one
  `UploadReply{bytes_total, chunks}`.
- `@rpc("greet.v1.Greeter/Chat")` — **bidi**.  Client streams
  `ChatMessage{text, seq}` items; server echoes each with `seq + 1`
  on its outbound stream and FINs after the client FINs.
- The ncc-emitted constructor wires all four handlers into the
  global RPC registry on process start.
- A real (test-only) cert chain + pinned-fingerprint trust store —
  the demo never disables verification.
- Auth wiring: a manifest (`auth.policies[]` + `rpc.services[]`)
  pinned via `n00b_rpc_server_attach_auth`, JWT verification via the
  in-process **synthetic IdP fixture** (`test/fixtures/synthetic_idp.c`).
- Deadline propagation: the client's `n00b_rpc_ctx_with_deadline`
  carries through to a `n00b-rpc-deadline-ms` request header.
- Audit subscription: every dispatch emits one stderr line via
  `n00b_quic_audit_subscribe`.

## Build

```bash
bash build.sh
ls build_debug/quic_rpc_demo
```

## Run — single-process loopback (recommended)

The simplest way to see all the pieces working together:

```bash
./build_debug/quic_rpc_demo --loopback
```

Expected output (paths/ports vary; audit lines go to stderr):

```
loopback: server up on 127.0.0.1:62421
[audit] ALLOW greet.v1.Greeter/Hello policy=rpc-default sub=alice aud=n00b-rpc-demo
Hello reply: Hello, alice!
[audit] ALLOW greet.v1.Greeter/Stream policy=rpc-default sub=alice aud=n00b-rpc-demo
Stream item 1: tick 1
Stream item 2: tick 2
Stream item 3: tick 3
Stream item 4: tick 4
Stream item 5: tick 5
[audit] ALLOW greet.v1.Greeter/Upload policy=rpc-default sub=alice aud=n00b-rpc-demo
Upload: chunks=4 bytes=40
[audit] ALLOW greet.v1.Greeter/Chat policy=rpc-default sub=alice aud=n00b-rpc-demo
Chat reply: ping-1 seq=2
Chat reply: ping-2 seq=3
Chat reply: ping-3 seq=4
```

## Run — separate server + client processes

In one shell:

```bash
./build_debug/quic_rpc_demo --server --bind 127.0.0.1:4433
# prints "listening on 127.0.0.1:<port> (alpn=h3)"; runs until Ctrl-C
```

In another shell (note: standalone `--client` mode does **not** mint
an auth token because it has no shared IdP with the server — the
canonical end-to-end flow is `--loopback` for self-contained smokes,
or the production swap below for real deployments):

```bash
./build_debug/quic_rpc_demo --client --target 127.0.0.1:4433
```

## Production swap — real OIDC + real PKI

Two pieces would change for a real deployment:

1. **Trust store.**  Replace `build_pinned_trust_for_test_cert()` in
   `main.c` with `n00b_quic_trust_system()` (default OS-trust, picks
   up Let's Encrypt + corporate CAs out of the box) or a custom
   `n00b_quic_trust_with_extra(...)` chain.

2. **Verifier resolver.**  Replace the synthetic IdP with a real
   `n00b_oidc_open(issuer)` per IdP entry in the manifest.  The
   resolver callback (`idp_resolver` here) keys by `idp_id` and
   returns the matching `n00b_jwt_verifier_t *`:

   ```c
   /* one-off setup */
   n00b_oidc_t *o = n00b_result_get(n00b_oidc_open("https://login.example"));
   auto vr = n00b_oidc_jwt_verifier(o, "checkout-api");
   n00b_jwt_verifier_t *v = n00b_result_get(vr);

   /* in idp_resolver: look up idp_id → v from your table */
   ```

   Tokens come from the operator's IdP via the user's normal login
   flow — the demo's `n00b_synthetic_idp_mint` becomes the real
   `n00b_oidc_token_endpoint(...)` exchange.

Everything else — the `@rpc`-annotated handlers, the ncc-generated
client stubs, the manifest schema, the audit hooks — stays identical.

## Files

- `greet.h`     — request/response types + `@rpc` forward declarations.
- `greet.c`     — handler bodies, per-type CBOR hooks, stream wrappers.
- `main.c`      — CLI dispatch, server + client lifecycle, auth wiring.

The smoke test (`test_quic_rpc_demo_smoke`) shells out to this binary's
`--loopback` mode in a subprocess and asserts the output.
