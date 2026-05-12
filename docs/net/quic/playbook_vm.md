# Bare-VM deployment playbook

How to run an n00b QUIC RPC service on a bare host or VM with
systemd as the service supervisor.  Companion to the
`test/integration/phase5_vm/` fixture.

This playbook is the operator path of choice when:

- The host's kernel already runs systemd.  No Kubernetes, no
  container runtime to learn.
- The cert lifecycle uses the canonical Linux ACME tooling
  (`uacme` for variant A, n00b-direct-ACME for variant B).
- Observability runs through the host's `journalctl` first,
  Prometheus second.

## Topology

```
┌────────────────────────────────────────────────┐
│  /etc/systemd/system/                          │
│    n00b-phase5.service       (Type=notify)     │
│    n00b-cert-renew.service   (Type=oneshot)    │
│    n00b-cert-renew.timer     (daily, 03:00)    │
└────────────────────────────────────────────────┘
                    │  manages
                    ▼
┌────────────────────────────────────────────────┐
│  /usr/local/bin/quic_phase5_demo  (or similar) │
│  /etc/n00b/manifest.toml                       │
│  /var/lib/n00b/tls/{tls.crt,tls.key,ca.crt}    │
│  /var/lib/n00b/lb-cid.key                      │
└────────────────────────────────────────────────┘
                    │  reads cert from
                    ▼
┌────────────────────────────────────────────────┐
│  /usr/bin/uacme  (variant A)                   │
│      runs daily; writes to /var/lib/n00b/tls/  │
│  OR                                            │
│  n00b's in-process ACME (variant B)            │
│      uses sticky_secret_t to coordinate        │
│      multi-host rotation                       │
└────────────────────────────────────────────────┘
```

## Prerequisites

- A host with systemd ≥ 247 (for `RuntimeDirectory=`,
  `LoadCredential=` if you use that path).
- `cap_net_bind_service` available — granted via the unit's
  `AmbientCapabilities=CAP_NET_BIND_SERVICE` if you bind privileged
  UDP ports (443).
- Outbound network reachability to the IdP (Keycloak / ZITADEL /
  vendor) and the ACME server.
- A real DNS name pointing at the host's public IP (variant A's
  HTTP-01 challenge needs port 80 reachable; DNS-01 doesn't).

## Installation steps

### 1. IdP — Keycloak (or equivalent)

The bare-VM playbook usually runs Keycloak inside a `podman` pod
fronted by the host's `nginx` / `caddy`, but it's straight-line
plumbing — no Kubernetes-specific story.  The realm seed JSON the
K8s fixture ships
(`test/integration/phase5_k8s/helm/keycloak-realm-tenant-*.json`)
is portable verbatim.

ZITADEL is an alternative IdP that ships in 5.11 — see
[`playbook_k8s.md`](playbook_k8s.md) § ZITADEL for the K8s-side
install.  The bare-VM equivalent runs the same `zitadel` binary
under a systemd unit pointed at an external Postgres (the
2-step install pattern the K8s fixture uses for the same chart-
hook reason).  The realm shape is portable verbatim from the
Keycloak fixture's JSON; ZITADEL's data model uses
*organizations* + *applications* in place of *realms* +
*clients*, but the operator-facing concept (one tenant per
isolation boundary, one OIDC client per binding mode) is the
same.

### 2. Cert lifecycle — variant A (uacme + DNS-01)

`uacme` is the canonical Linux ACME client when there's no L7
HTTP server already on the host (you'd use `certbot` if you ran
nginx; n00b's QUIC server doesn't speak HTTP/1.1 to LE).  The
fixture wiring:

```ini
# /etc/systemd/system/n00b-cert-renew.service
[Unit]
Description=Renew TLS cert for n00b
After=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/bin/uacme \
    -h /etc/n00b/dns01-hook.sh \
    -c /var/lib/n00b/uacme \
    issue n00b.example.com
ExecStartPost=/bin/install -m 600 \
    /var/lib/n00b/uacme/n00b.example.com/cert.pem \
    /var/lib/n00b/tls/tls.crt
ExecStartPost=/bin/install -m 600 \
    /var/lib/n00b/uacme/private/n00b.example.com/key.pem \
    /var/lib/n00b/tls/tls.key
ExecStartPost=/bin/systemctl reload n00b-phase5
```

```ini
# /etc/systemd/system/n00b-cert-renew.timer
[Unit]
Description=Daily cert renewal for n00b

[Timer]
OnCalendar=*-*-* 03:00:00
Persistent=true

[Install]
WantedBy=timers.target
```

DNS-01 hook (`/etc/n00b/dns01-hook.sh`): a small shell script that
provisions / deprovisions the `_acme-challenge` TXT record with
your DNS provider's API.  The fixture ships skeletons for
Cloudflare, Route53, GCP CloudDNS — pick whichever matches your
zone.  See [cert_lifecycle.md](cert_lifecycle.md) § DNS providers
for the contract; the same provider classes work in
`n00b_acme_acquire_certificate` if you go variant B.

The `ExecStartPost=systemctl reload n00b-phase5` step relies on
the n00b service unit having `ExecReload=/bin/kill -HUP $MAINPID`
— n00b's runtime reloads the cert from disk on SIGHUP via the
[cert_lifecycle.md § Hot reload](cert_lifecycle.md#hot-reload)
path.  No process restart, no dropped QUIC connections.

### 2'. Cert lifecycle — variant B (n00b-direct-ACME)

`quic_acme_http01_demo` (or its production-shaped sibling) drives
ACME from inside the n00b binary; renewal is a function call, not
a separate process.  Operators who prefer to keep cert-state
inside n00b pick this; trade-off is they lose the systemd-timer
observability that variant A's separate unit gives.

For multi-host deployments, share the ACME *account* key (via a
`sticky_secret_t` blob distributed to all hosts) so renewals
coordinate cleanly under LE rate limits.  The same blob carries
the LB-CID key in step 3.

### 3. Multi-host + LB-CID routing

The bare-VM equivalent of the K8s LB-CID story is HAProxy 3.x in
front of the n00b hosts:

```haproxy
# /etc/haproxy/haproxy.cfg (excerpt)
listen n00b-quic
    bind quic4@:443 ssl crt /var/lib/n00b/tls/edge.pem alpn h3
    mode http
    # HAProxy 3.x QUIC LB-CID mode — needs the same 16-byte key
    # the n00b hosts use.
    quic lb-cid key-file /etc/haproxy/lb-cid.key

    server host-1 10.0.0.11:443 quic ssl
    server host-2 10.0.0.12:443 quic ssl
    server host-3 10.0.0.13:443 quic ssl
```

Each n00b host's systemd unit reads its `LB_CID_SERVER_ID` from
`Environment=` (or via `LoadCredential=`-style provisioning).
Hosts agree on a 16-byte LB key
(`/etc/haproxy/lb-cid.key` and `/var/lib/n00b/lb-cid.key`); the
key rotates out-of-band via the
[sticky_secret.h](../../../include/net/quic/sticky_secret.h) coordination
helpers — see
[runbook.md § VM.6](runbook.md#vm6-haproxy-lb-cid-key-mismatch).

### 4. Observability — journald + node_exporter

`/metrics` on TCP/9100 still applies; `node_exporter` 1.7+ on the
host scrapes both n00b and node metrics in one Prometheus target.
For ad-hoc operator queries, `journalctl -u n00b-phase5 -f`
streams the demo's audit + RPC logs in real time —
`StandardOutput=journal` in the unit (which the fixture's
`n00b-phase5.service` sets) makes this work without extra
configuration.

## Verifying the install

```bash
$ N00B_TEST_VM=1 bash test/integration/phase5_vm/run.sh
```

Boots a privileged container with systemd PID-1, runs the
n00b-phase5 unit through one `--loopback` cycle, asserts the
canonical success markers in `journalctl -u n00b-phase5`.  The
container fakes a real VM well enough to catch any unit-file /
cap-bind / cgroup config that wouldn't survive a real boot.
~2 min on warm Docker cache.

## Common gotchas (cross-references to runbook)

- [VM.1](runbook.md#vm1-uacme-cron-failed): uacme cron failed
- [VM.2](runbook.md#vm2-systemd-unit-restart-loop): systemd unit
  restart loop
- [VM.4](runbook.md#vm4-privileged-port-without-cap_net_bind_service):
  bind 443 → "Permission denied"
- [VM.5](runbook.md#vm5-dns-01-hook-failed-provider-creds):
  DNS-01 hook failed
