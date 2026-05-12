# quic_phase5_demo

Multi-tenant Phase 5 reference demo.  Exercises every Phase 1–5
piece end-to-end in a single process via `--loopback`:

| Layer | Piece exercised |
|---|---|
| Phase 1 | UDP conduit + endpoint + chan + qlog |
| Phase 2 | manifest loader + preflight (`observability.metrics` + the existing endpoint sections) |
| Phase 3 | OIDC verifier (synthetic-IdP) + audit fan-out + DPoP / role-claim policies |
| Phase 4 | H3 + RPC (unary + server-stream) + `@rpc(...)` ncc annotation + auth wiring |
| Phase 5 | Prometheus `/metrics` listener (conduit-driven) + multi-tenant resolver + per-call metrics |

## Services

Five RPCs across three services:

| Method | Shape | Policy |
|---|---|---|
| `phase5.v1.Greeter/Hello`  | unary         | audience-only      |
| `phase5.v1.Greeter/Stream` | server-stream | audience-only      |
| `phase5.v1.Vault/Read`     | unary         | audience           |
| `phase5.v1.Vault/Write`    | unary         | audience + role=admin (override) |
| `phase5.v1.MTls/Echo`      | unary         | beta-tenant audience |

The demo's loopback driver exercises all five.

## Multi-tenancy

Two synthetic IdPs (`alpha` + `beta`) feed two `n00b_jwt_verifier_t`s.
The demo's resolver (`tenant_resolver.c`) routes inbound requests by
the `X-Tenant: <id>` header (when present) or by the manifest-resolved
`idp_id` argument (fallback).  Each tenant pins its own audience;
tokens minted by tenant A's IdP only validate for tenant A's services.

```
inbound request
  → applied policy (manifest or per-call override)
  → resolver(idp_id, hdrs, n_hdrs, ctx)
  → tenant table lookup
  → JWT verifier
  → claim checks (audience / role / scope)
  → ALLOW / DENY + audit event
```

## Observability

`/metrics` listener bound on an ephemeral port; scrape with curl:

```
$ curl -s localhost:<metrics-port>/metrics
n00b_quic_chan_opens_total{kind="framed"} N
n00b_quic_chan_active{kind="framed"}      M
n00b_quic_audit_events_total{decision="allow"} ...
n00b_quic_audit_events_total{decision="deny"}  ...
n00b_quic_rpc_calls_total{service="phase5.v1.Greeter",method="Hello",status="0"} ...
n00b_quic_rpc_call_duration_us_bucket{le="100"} ...
...
```

The loopback driver scrapes the listener and asserts the expected
counters fire.

## Build + run

```
$ N00B_TEST=1 bash build.sh
$ ./build_debug/quic_phase5_demo --loopback
loopback: server up on 127.0.0.1:<eph>, /metrics on :<eph>
Hello reply: Hello, phase5!
Stream item: i=1 text=tick 1
Stream item: i=2 text=tick 2
Stream item: i=3 text=tick 3
Vault.Read reply: secret-for:api_key
Vault.Write reply: bytes=14
MTls.Echo reply: hello-mtls
loopback: metrics surface OK
```

## Production swap-in

Phase 5 production deployments replace the synthetic IdPs with real
Keycloak (K8s playbook) or ZITADEL (bare-VM playbook); the manifest's
`auth.idps[].issuer` URLs change but the resolver / services / metrics
machinery is unchanged.  See `docs/quic/playbook_k8s.md` and
`docs/quic/playbook_baremetal.md` for the operator-facing flows.

## What's NOT exercised in the loopback

Documented gaps that real production deployments cover but the
loopback skips for fixture simplicity:

- **Real mTLS-bound tokens** — `phase5.v1.MTls/Echo`'s policy in the
  loopback stops at audience.  Production sets `require_mtls=true`
  and the policy enforces `cnf.x5t#S256` against the QUIC client
  cert.  The Phase 3 `test_quic_mtls_token.c` covers the token
  validation path against a real cert thumbprint.
- **DPoP htm/htu enforcement at server side** — the loopback omits
  `require_dpop=true` from the vault policies because the loopback
  reconstructs htu differently than the client builds it.  Phase 3
  `test_quic_dpop.c` exercises full DPoP verify end-to-end.
- **Real cert-expiry parsing** — `n00b_quic_cert_expiry_seconds`
  is registered but stays at zero in the loopback (the demo doesn't
  parse the test cert's notAfter).  Production deployments set it
  via `n00b_quic_metric_gauge_set` after their cert load.
