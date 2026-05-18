# Kubernetes deployment playbook

How to run a multi-replica n00b QUIC RPC service in Kubernetes.

This is the operator-facing companion to the
`test/integration/phase5_k8s/` fixture: every line below maps to
a fixture YAML you can read; the fixture proves the substrate
works end-to-end before you wire your own service into it.

## Topology

```
       ┌────────────────────────────┐
       │  ingress-nginx              │   variant A only
       │  (HTTP-01 challenge solver) │
       └──────────────┬──────────────┘
                      │ Ingress: <fqdn>
                      │
   ┌──────────────────┴───────────────────┐
   │ cert-manager  (variant A)            │
   │   - ClusterIssuer → ACME directory   │
   │   - Certificate   → Secret tls.crt   │
   └──────────────────┬───────────────────┘
                      │ Secret mount
                      ▼
   ┌─────────────────────────────────────┐
   │  Envoy / HAProxy  (optional)        │ ◄── L4 UDP, QUIC LB-CID-aware
   │   QUIC LB filter / lb-cid mode      │
   └────────────┬─────────┬──────────────┘
                │         │
                ▼         ▼
    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
    │ n00b pod 0   │ │ n00b pod 1   │ │ n00b pod 2   │
    │ server_id=1  │ │ server_id=2  │ │ server_id=3  │
    └──────┬───────┘ └──────┬───────┘ └──────┬───────┘
           │                │                │
           └────────────────┴────────────────┘
                            │
                            ▼
                   ┌────────────────┐
                   │  Keycloak      │  ◄── tokens (DPoP / mTLS)
                   │  realm-imported│
                   └────────────────┘
```

## Prerequisites

- A Kubernetes cluster (any flavour ≥ v1.28).  The fixture runs on
  `kind` + Docker Desktop; production runs on EKS, GKE, AKS, or
  on-prem clusters with no n00b-specific assumptions.
- Helm 3.13+ for cert-manager / Keycloak / ingress-nginx installs
  (the fixture's `run.sh` invokes `helm` directly; production
  operators usually wrap in their preferred templating layer).
- An OIDC IdP and a public DNS name pointing at the Service /
  Ingress that fronts n00b.

## Installation steps

### 1. IdP — Keycloak (or equivalent)

The fixture installs the Bitnami Keycloak chart with a 2-tenant
realm seed (DPoP-bound + mTLS-bound clients × 4 users × 3 roles
each).  Production operators bring their own realm shape; the
parts n00b needs:

- A realm per logical tenant.
- A confidential client per tenant with the cert-bound /
  DPoP-bound attribute set, depending on the binding mode you
  want enforced.
- An audience-mapper that emits the audience n00b's
  verifier-resolver expects (`phase5-alpha`, `phase5-beta` in the
  fixture; rename via the manifest's `[rpc].auth.audience` field).

For Bitnami chart specifics + the bitnamilegacy registry redirect
that's required after Aug 2025, see
[runbook.md § K8s.6](runbook.md#k8s6-bitnami-chart-install-fails-manifest-unknown).

For the user-profile firstName/lastName trap that bites realm
imports without those fields populated, see
[runbook.md § K8s.7](runbook.md#k8s7-keycloak-realm-import-users-cannot-password-grant).

### 2. Cert lifecycle — variant A (cert-manager-fronts)

Standard K8s pattern: cert-manager owns ACME, deposits the cert
into a `tls.crt` + `tls.key` Secret, n00b mounts it.  The fixture
points cert-manager at Pebble; production points at Let's Encrypt.

Skeletons live under `test/integration/phase5_k8s/cert-manager/`:

- `cluster-issuer.yaml` — ACME ClusterIssuer.  For prod, swap
  `acme.server` to LE prod (`https://acme-v02.api.letsencrypt.org/directory`)
  and drop the `acme.caBundle` field (LE's chain is in the system
  trust store).
- `certificate.yaml` — Certificate CR.  For prod, swap the
  `commonName` / `dnsNames` to your real domain and rotate the
  `secretName` accordingly.
- `n00b-svc.yaml` — ExternalName Service that puts the challenge
  hostname in cluster DNS without patching CoreDNS.  Production
  operators with a real Public DNS for the cert hostname don't
  need this — kube-dns + the public DNS record cover both
  cert-manager's self-check and the ACME server's validation
  path.  See
  [runbook.md § K8s.8](runbook.md#k8s8-cert-manager-challenge-stuck-on-self-check-dns-error)
  for why the fixture needs the workaround.

### 2'. Cert lifecycle — variant B (n00b-direct-ACME)

When you want cert state inside the n00b binary (rotate without
process restart, or operate in environments where cert-manager
isn't available), `n00b_acme_acquire_certificate` drives the full
ACME flow from inside n00b's runtime.  The fixture's
`examples/quic_acme_http01_demo` is the operator-facing demo: it
runs an embedded HTTP-01 responder on a non-privileged port, calls
`n00b_acme_acquire_certificate`, writes the resulting PEM chain
to disk.  Operators wrap this in their long-running n00b service
and rotate the cert in-process via `n00b_quic_cert_store_update`
(see [cert_lifecycle.md](cert_lifecycle.md)).

The variant-B Service in the fixture
(`test/integration/phase5_k8s/acme_b/n00b-acme-svc.yaml`) carries
two subtleties worth noting in production:

- `publishNotReadyAddresses: true` so a Pod-without-readinessProbe
  enters the EndpointSlice the moment its container starts.
  Operators who add a real readinessProbe (which they should —
  TLS-listener-up is a meaningful health signal) can drop this.
- Pod listens on a non-privileged port (8080 in fixture); the
  Service maps `:80 → :8080` so the ACME validator's port-80 GET
  still lands.  Operators who grant `CAP_NET_BIND_SERVICE` to the
  container can collapse this back to direct port 80.

### 3. Multi-replica + LB-CID routing

n00b's endpoint accepts a `lb_cid_config` kwarg
([`endpoint.h`](../../../include/net/quic/endpoint.h)) that, when set,
encodes the replica's `server_id` into every server-issued CID
via AES-128 in the
[draft-ietf-quic-load-balancers](https://datatracker.ietf.org/doc/draft-ietf-quic-load-balancers/)
block-cipher mode.  The shared 16-byte key + per-replica server_id
need to be propagated to each Pod:

- **Shared key** — generate once (`openssl rand 16 -hex`), store
  in a K8s Secret, mount as a file or pass via env var.
  Production operators rotate the key out-of-band coordinated
  with all replicas (the
  [`sticky_secret.h`](../../../include/net/quic/sticky_secret.h) API
  handles the rotation envelope; see
  [runbook.md § 13](runbook.md#13-replica-skew-on-sticky-secret-rotation)).
- **Per-pod server_id** — for a StatefulSet, derive from the
  Pod's ordinal (`metadata.name` ends in `-0`, `-1`, ...) via
  the downward API.  For a Deployment, an init container that
  hashes the Pod IP works too; or use a
  `kube-system/configmap-reload`-style sidecar to assign + watch.

  Concrete pattern that works with the Phase 5 demo binary
  (which reads `LB_CID_SERVER_ID` from its environment and
  defaults to `1` if unset):

  ```yaml
  # In the StatefulSet's spec.template.spec.containers[0]:
  env:
    - name: POD_NAME
      valueFrom:
        fieldRef:
          fieldPath: metadata.name
    - name: LB_CID_SERVER_ID
      # POD_NAME ends in `-N`; an init container or a small
      # shell wrapper extracts the suffix and exports
      # LB_CID_SERVER_ID.  In the fixture's loopback mode this
      # is set directly via `Environment=` in the systemd unit
      # (bare-VM) or `env:` in the Pod spec (K8s).
      value: "1"  # replace per replica; use a downward-API
                  # `subPath` if your CD pipeline can
  ```

  ## ZITADEL

  Operators can swap Keycloak for ZITADEL.  The K8s install
  fixture lives at `test/integration/phase5_k8s/zitadel/` —
  it deploys ZITADEL via the upstream Helm chart with
  `postgresql.enabled=false` + a separately-installed Bitnami
  Postgres release (the chart's pre-install hook fires before
  any sub-chart sub-resources deploy, so the bundled
  Postgres path doesn't work without the 2-step install
  pattern).  See `runbook.md` § VM.3 for the upgrade-path
  caveat that applies to both K8s and bare-VM ZITADEL
  deployments.

  After install, populate organizations + applications via
  ZITADEL's gRPC management API or
  [`terraform-provider-zitadel`](https://registry.terraform.io/providers/zitadel/zitadel/latest/docs).
  The realm shape from `helm/keycloak-realm-tenant-*.json` is
  portable conceptually (one ZITADEL org per Keycloak realm,
  one OIDC application per Keycloak client) but the wire
  format is different — there's no realm-import-via-JSON in
  ZITADEL.

The wire CID is decoded by an LB-CID-aware L4 LB (Envoy
contrib's QUIC LB filter / HAProxy 3.x with `lb-cid` mode).
Operators ship the same key to the LB; the LB extracts the
`server_id` from each incoming UDP datagram's destination CID
and forwards to the right backend.  An L4 LB that can't decode
LB-CID rounds-robins like a normal UDP LB — connections still
work, but they're not *sticky*, so connection migration breaks
once a replica restarts.

For more on LB-CID mechanics see
[security.md § QUIC LB-CID](security.md#quic-lb-cid).

### 4. Observability — Prometheus + Grafana

The Phase 5 demo manifest has an `[observability]` section that
adds a `n00b_quic_metric_listener` to the endpoint:

```toml
[observability]
metrics_bind = "0.0.0.0:9100"
```

This exposes `/metrics` on TCP/9100 (separate from the QUIC UDP
port).  Wire it into your Prometheus scrape config via a Service
+ ServiceMonitor or a podMonitor.  Drop
[`dashboards/n00b_quic.json`](dashboards/n00b_quic.json) into
Grafana to get the canonical SLO views.

## Verifying the install

```bash
$ N00B_TEST_KIND=1 bash test/integration/phase5_k8s/run.sh
```

This boots a kind cluster, installs all of the above (Keycloak,
ingress-nginx, Pebble, cert-manager, ClusterIssuer + Certificate,
n00b-direct-ACME variant), runs the demo Job, and asserts the
canonical success markers (Hello / MTls.Echo / LB-CID OK / metrics
surface OK).  ~3 min on warm Docker cache.

## Common gotchas (cross-references to runbook)

- [K8s.1](runbook.md#k8s1-pod-crashloopbackoff): Pod
  CrashLoopBackoff (almost always: cert-renewal failure or
  manifest preflight error)
- [K8s.2](runbook.md#k8s2-cert-manager-order-stuck-variant-a):
  cert-manager Order stuck in `pending`
- [K8s.5](runbook.md#k8s5-pod-to-replica-routing-via-lb-cid):
  multi-replica connections all landing on one pod (LB doesn't
  decode LB-CID)
- [K8s.6](runbook.md#k8s6-bitnami-chart-install-fails-manifest-unknown):
  Bitnami chart install fails with "manifest unknown"
- [K8s.8](runbook.md#k8s8-cert-manager-challenge-stuck-on-self-check-dns-error):
  cert-manager Challenge stuck on self-check DNS error
