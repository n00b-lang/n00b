# Deployment manifest + `--preflight`

The deployment manifest is the single declaration of how a n00b/QUIC
server's external surface is configured: which ports it binds, what
cert it presents, where the cert comes from, and how shared
symmetric secrets (stateless-reset, address-validation token) are
sourced.  An operator writes one JSON file; the application loads
it once at startup and uses it to construct endpoints. Before
binding any port, `--preflight` validates every declared dependency
and reports issues with remediation hints.

The structured-finding shape is deliberate: an ops loop driven by
an LLM agent reads the JSON findings, applies fixes, re-runs.  The
playbooks shipped at the back of Phase 2 are essentially "how to
read findings from `--preflight` and act on them."

---

## Schema (Phase 2 v1)

```json
{
  "version": 1,
  "service_name": "checkout-api",
  "endpoints": [
    {
      "id": "main",
      "bind_host": "0.0.0.0",
      "bind_port": 443,
      "alpn": ["h3", "n00b-rpc/1"],
      "cert": {
        "kind": "static" | "external" | "acme",
        ...
      }
    }
  ]
}
```

### `endpoints[].cert.kind = "static"`

```json
"cert": {
  "kind": "static",
  "chain_pem_path": "/etc/letsencrypt/live/api.example.com/fullchain.pem",
  "key_secret_uri": "file:/etc/letsencrypt/live/api.example.com/privkey.pem"
}
```

The simplest path. The PEM chain on disk is the source of truth;
`key_secret_uri` is opened via the secret-provider registry
(`include/net/quic/secret.h`).

### `endpoints[].cert.kind = "external"`

```json
"cert": {
  "kind": "external",
  "argv": ["step", "ca", "certificate",
           "api.example.com",
           "/var/n00b/cert.pem", "/var/n00b/key.pem"],
  "chain_pem_path": "/var/n00b/cert.pem",
  "key_secret_uri": "file:/var/n00b/key.pem"
}
```

The provisioner runs `argv` via `fork+execvp` (no shell — config
substitutions can't smuggle metacharacters), waits for it to exit
0, then loads the resulting files.  Suitable for dev workflows
where `step-cli` (or similar) is already part of the operator's
toolchain.

### `endpoints[].cert.kind = "acme"`  *(shape-only in v1)*

```json
"cert": {
  "kind": "acme",
  "directory_url": "https://acme-v02.api.letsencrypt.org/directory",
  "subject_names": ["api.example.com"],
  "challenge": "http-01",
  "account_key_uri": "keychain:com.example.acme.account-key",
  "contact_email": "ops@example.com"
}
```

The shape is recognized; full ACME-driven provisioning waits on
the cloud DNS providers (route53/gcp_dns/cloudflare) shipped at
the back of Phase 2 and on the deployment-playbook's HTTP-01
solver.

### `rpc.services[]` *(Phase 4 § 4.11)*

```json
"rpc": {
  "services": [
    { "id": "checkout.v1.Checkout",
      "auth_policy": "rpc-readwrite" }
  ]
}
```

Each entry binds a service id (the gRPC-style dotted name used in
the H3 `:path` pseudo-header) to an entry in `auth.policies[]` by
id.  Services that aren't listed here fall back to the
channel-level default policy.  Preflight rejects manifests where
an `auth_policy` reference doesn't resolve.

A service registers itself at process startup by being linked
into the binary (the ncc-emitted dispatcher uses a constructor
attribute).  This `rpc.services[]` block is the **operator
view** — it does not affect *whether* a service is available, only
how its inbound calls are gated.

### Deferred sections (Phase 2 follow-ups)

The design doc § 9.1 also calls out `stateless_reset`,
`address_validation`, `lb`, and `dns_expectations` blocks.  Those
land additively when the cloud-secret providers (vault:, kms:)
ship.  v1 ignores unknown top-level keys and surfaces them via an
`INFO` finding from preflight rather than rejecting the manifest —
forward-compat for additive growth.

---

## Loading the manifest

```c
#include "net/quic/manifest.h"

n00b_buffer_t *body = …;
auto r = n00b_quic_manifest_load_json(body);
if (n00b_result_is_err(r)) { /* schema violation */ }
n00b_quic_manifest_t *m = n00b_result_get(r);

/* Or: n00b_quic_manifest_load_path("/etc/n00b/manifest.json") */
```

---

## Running preflight

```c
auto pr = n00b_quic_preflight(m);
n00b_quic_preflight_report_t *rep = n00b_result_get(pr);
n00b_quic_preflight_report_print(rep, stderr);
if (!rep->ok) {
    /* refuse to start; rep->findings carries the details */
}
```

### Checks Phase 2 v1 ships

| Check id                          | Severity on fail | What it does                                                                  |
|-----------------------------------|------------------|-------------------------------------------------------------------------------|
| `port-bind:<host>:<port>`         | ERROR            | Try `bind()` to the declared host:port (UDP), close immediately               |
| `cert-static:<endpoint>`          | ERROR            | Open `chain_pem_path`, parse first cert block, check Validity period          |
| `cert-static-key:<endpoint>`      | ERROR            | Open `key_secret_uri` via the secret-provider registry                        |
| `cert-external:<endpoint>`        | ERROR            | Confirm `argv[0]` exists in PATH (or as an absolute/relative path)            |
| `cert-external-key:<endpoint>`    | ERROR            | Same as static                                                                |
| `cert-acme-directory:<endpoint>`  | ERROR / WARN     | `directory_url` must be `https://`; host should resolve via DNS               |
| `cert-acme-key:<endpoint>`        | ERROR            | Account-key URI opens cleanly                                                 |
| `rpc-service:<service-id>`        | ERROR            | Each `rpc.services[].auth_policy` must resolve to an `auth.policies[].id`     |

`bind_port` *< 1024* on a non-elevated process surfaces as an
ERROR with `EACCES` and a remediation pointing at
`setcap cap_net_bind_service=+ep` on Linux.

### Severities

- **INFO** — the check ran and the system is healthy.
- **WARN** — the check ran and surfaced something the operator
  should know about, but doesn't block deployment (e.g., cert
  expires in less than 30 days; ACME directory host doesn't
  resolve right now but the deployment may still come up).
- **ERROR** — the check failed and deployment will not work as
  declared.  `report->ok` is `false` if any finding is ERROR.

### What `--preflight` does *not* do

- It does NOT contact ACME or actually provision a cert.  That
  happens lazily at first acquisition, after the server has
  bound.
- It does NOT verify the live cert against any external trust
  store; only Validity dates of the leaf are checked.
- It does NOT verify the LB-CID encryption key shape; that
  belongs to a future check once the `lb` section ships.

---

## Reading findings programmatically

Each finding carries:

```c
typedef struct {
    char                          *check;       // "port-bind:0.0.0.0:443"
    n00b_quic_preflight_severity_t severity;
    char                          *detail;      // human-readable
    char                          *remediation; // how to fix
} n00b_quic_preflight_finding_t;
```

Tooling (LLM agents, CI, GitOps reconcilers) iterates the
`findings` array and decides what to do.  The `check` field is a
stable identifier — use it for filtering or matching against
expected fail-states.

---

## See also

- `examples/quic_server_managed/main.c` — the reference example
  that loads a manifest, runs preflight, and brings up endpoints.
- `docs/net/quic/cert_lifecycle.md` — how the cert provisioner
  abstraction (static / external / acme) plugs into the manifest.
- `~/dd/quic_2.md` § 9 — the design rationale.
