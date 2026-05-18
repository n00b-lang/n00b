# Deploying n00b QUIC services — index

This page is the operator-facing entry point for n00b's Phase 5
deployment story.  Two playbooks ship: **Kubernetes** and
**bare-VM**.  Both produce the same deliverable — the n00b QUIC
RPC server, terminating TLS via an ACME-issued cert and exposing
Prometheus metrics on `/metrics` — using ergonomics that fit each
environment's conventions.

| Role       | Playbook   | Cert lifecycle      | LB / fronting  | Service supervision | See      |
|------------|------------|---------------------|----------------|---------------------|----------|
| Server     | Kubernetes | cert-manager (variant A) <br> n00b-direct-ACME (variant B) | Optional Envoy / HAProxy with QUIC LB-CID | Deployment / Job        | [playbook_k8s.md](playbook_k8s.md)    |
| Server     | Bare-VM    | uacme + DNS-01 (variant A) <br> n00b-direct-ACME (variant B) | Optional HAProxy 3.x lb-cid                | systemd unit            | [playbook_vm.md](playbook_vm.md)      |
| Client     | Connect-only | n/a (uses peer's cert) | n/a            | host process            | [playbook_client.md](playbook_client.md) |

The first two playbooks describe how to **run n00b as a QUIC
server**.  The third describes how to **use n00b's HTTP/3 client
surface** to call someone else's HTTP/3 endpoint (a SaaS API, an
ACME directory, an OIDC provider, an internal microservice).
Connect-only deployments don't need a cert lifecycle of their own
— they verify the peer's cert via the OS-native trust store —
and they don't need an L4 LB story; the relevant operational
concerns are *discovery* (does the server speak QUIC?) and
*fallback* (what if it doesn't, or UDP is blocked on this path?).
The connect-only playbook covers Alt-Svc, HTTPS DNS records,
Happy Eyeballs racing, and the trust-store options for
public-CA / private-CA / pinned-fingerprint flavours.

Cross-references:

- [`runbook.md`](runbook.md) — incident-response playbook entries.
  K8s entries are prefixed `K8s.N`; bare-VM entries are `VM.N`.
- [`dashboards/n00b_quic.json`](dashboards/n00b_quic.json) — Grafana
  dashboard wired against the metrics exposed in
  [`overview.md` § Metrics](overview.md#metrics).
- [`interop_matrix.md`](interop_matrix.md) — n00b ↔ third-party
  QUIC stack (ngtcp2 / msquic / quiche / aioquic) compatibility
  matrix.
- [`cert_lifecycle.md`](cert_lifecycle.md) — module-level docs for
  the cert-provisioner / cert-store APIs the playbooks consume.

## Cross-cutting design choices

The two playbooks share a common foundation; the per-playbook
docs only describe the differences.  Anything below applies to
both.

### Identity

n00b's RPC layer authenticates callers via OIDC-issued JWTs (see
[auth.md](auth.md)).  The playbook fixtures install a tiny
in-cluster IdP that mints DPoP-bound and mTLS-bound tokens to
prove the JWT validation path end-to-end.  Production deployments
swap that IdP for Keycloak / ZITADEL / Auth0 / Okta / etc.; the
n00b verifier-resolver is decoupled from the IdP brand by design.

The K8s fixture ships a Bitnami Keycloak Helm install with the
2-tenant realm seed (`tenant-alpha` DPoP-bound, `tenant-beta`
mTLS-bound, 4 users × 3 roles each); the bare-VM fixture documents
the same realm seed but starts a Keycloak container directly.
The realm JSON is portable across both flavours.

### Cert lifecycle

The `cert-manager-fronts` (variant A) and `n00b-direct-ACME`
(variant B) split is deliberate:

- Variant **A** is the K8s-native default: cert-manager owns ACME,
  deposits a `tls.crt` + `tls.key` into a Secret, n00b mounts it
  via a projected volume.  Operators get cert-manager's
  out-of-the-box renewal scheduling and the cert-manager
  controller's view of expiry windows.
- Variant **B** keeps n00b in the cert-acquisition path:
  `n00b_acme_acquire_certificate` (see
  [cert_lifecycle.md](cert_lifecycle.md)) drives the ACME flow
  itself.  Operators who want the cert state inside the n00b
  binary (e.g., for rotate-without-restart, or air-gapped
  environments where cert-manager isn't available) pick this.

The bare-VM playbook substitutes `uacme + DNS-01 + cron` for
variant A (canonical for systemd-managed hosts) and uses
`quic_acme_http01_demo` (or its production-shaped sibling) for
variant B.

### Metrics + observability

Both playbooks expose `/metrics` (Prometheus exposition 0.0.4)
on a separate TCP port from the QUIC UDP port, populated by the
`n00b_quic_metric_registry_t` the demo binary builds at startup.
Default series:

- `n00b_quic_chan_opens_total{kind=...}` — counter, per-channel-kind
- `n00b_quic_chan_active{kind=...}` — gauge
- `n00b_quic_cert_expiry_seconds{endpoint=...}` — gauge
- `n00b_quic_audit_events_total{decision=allow|deny|err}` — counter
- `n00b_quic_rpc_calls_total{service,method,status}` — counter
- `n00b_quic_rpc_call_duration_us{service,method}` — histogram

`dashboards/n00b_quic.json` plots the canonical SLO views (handshake
latency p99, RPC error rate, connection-pool saturation, cert
expiry countdown).  Drop it into Grafana via *Dashboards → Import*;
points at the `prometheus` datasource by default.

### Multi-replica + LB-CID

Both playbooks support multi-replica deployments where every n00b
instance encrypts its server-id into its outgoing CIDs (see
[`lb_cid.h`](../../../include/net/quic/lb_cid.h) /
[`security.md` § QUIC LB-CID](security.md#quic-lb-cid)).  An
LB-CID-aware L4 load balancer (Envoy contrib's QUIC LB filter,
HAProxy 3.x's lb-cid mode) decodes the wire CID and forwards
follow-up packets to the right replica regardless of source-IP
migration.  The shared LB key + per-replica server_id are wired
through `n00b_quic_endpoint_new(.lb_cid_config = ...)`; per-playbook
docs explain how operators feed the key + ID through their
substrate (K8s Secret + downward API; systemd EnvironmentFile +
hostname suffix).

### Smoke-test fixtures

Both playbooks ship a CI fixture (`test/integration/phase5_k8s/`,
`test/integration/phase5_vm/`) that:

1. Builds the demo binary into a container image.
2. Stands up the playbook substrate (kind + Helm; or
   privileged-systemd container).
3. Runs the demo's `--loopback` mode (server + client in one
   process exercising 5 RPCs across 2 tenants with different
   token bindings + the LB-CID encode/decode round-trip + the
   metrics scrape).
4. Asserts canonical success markers (`Hello reply`,
   `MTls.Echo reply`, `metrics surface OK`, `LB-CID OK`).
5. Tears down.

The fixtures are gated on opt-in env vars (`N00B_TEST_KIND=1` and
`N00B_TEST_VM=1`) so they don't slow the default
`bash build.sh` cycle.  Run them by hand, or via meson:

```bash
$ meson test -C build_debug --print-errorlogs --suite integration \
    quic_phase5_k8s_smoke
$ meson test -C build_debug --print-errorlogs --suite integration \
    quic_phase5_vm_smoke
```

## Where the demo / fixture / playbook boundary is

The Phase 5 *demo binary* (`examples/quic_phase5_demo`) and the
Phase 5 *playbook fixtures* are deliberately self-contained:

- Fixtures use **ephemeral self-signed cert + 16-zero-byte LB-CID
  key**.  Production playbooks pin a real LE / private CA cert
  and provision the LB key via `openssl rand 16` into a real
  Secret / EnvironmentFile.
- Fixtures use the **synthetic IdP** that ships in
  `test/fixtures/synthetic_idp.c` (mints offline-validatable
  tokens with the audience the demo expects).  Production
  playbooks point the verifier-resolver at the operator's real
  IdP.
- Fixtures use **Pebble** for ACME.  Production playbooks point
  cert-manager / uacme / n00b-direct-ACME at LE prod or a real
  private CA.

Each playbook MD calls out the exact lines in fixture YAML / the
systemd unit that change in the production-shaped variant.
