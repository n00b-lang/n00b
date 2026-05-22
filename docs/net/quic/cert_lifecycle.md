# Cert lifecycle

How a cert flows through n00b/QUIC from "operator declares it" to
"server presents it on the wire," and how renewal happens without
dropping in-flight connections.

The cross-cutting view from `~/dd/quic_2.md` § 10:

```
                    [manifest.cert]
                          |
                +---------+---------+
                |         |         |
            kind=acme  kind=static  kind=external
                |         |         |
        n00b_quic_acme_  load.     spawn argv,
        acquire()        from       wait for
                |        secret_t   files, then
                |         |         load
                +---------+---------+
                          |
              n00b_quic_cert_provisioner_t (acquire/should_renew)
                          |
                  n00b_quic_endpoint_new
                          |
                  endpoint serves cert
                          |
                  ── renewal cycle ──
                          |
                  cert about to expire
                          |
                  re-acquire
                          |
                  n00b_quic_endpoint_reload_cert
                          |
                  in-flight connections (already-handshaked) unaffected;
                  new handshakes use new cert
```

---

## The three layers

### 1. Manifest entry → cert provisioner

`include/internal/net/quic/cert_provisioner.h` defines the
`n00b_quic_cert_provisioner_t` trait: `acquire()`, `should_renew()`,
`close()`.  Three concrete back-ends ship in Phase 2:

| Kind       | Constructor                                          | Renewal                                                |
|------------|------------------------------------------------------|--------------------------------------------------------|
| `static`   | `n00b_quic_cert_provisioner_static(path, key)`       | Never. Operator manages the file on disk.              |
| `acme`     | `n00b_quic_cert_provisioner_acme(directory, ...)`    | Auto, when `now > not_after - renew_margin_ms`.        |
| `external` | `n00b_quic_cert_provisioner_external(argv, ...)`     | Operator-driven via `force_refresh` flag.              |

The manifest's `cert.kind` field selects which constructor to use;
the surrounding fields (chain_pem_path, key_secret_uri, argv,
directory_url, etc.) are the constructor's arguments.

### 2. Provisioner → endpoint

The endpoint constructor accepts raw cert DER + a key file path
(Phase 2 v1 limitation; the proper picotls sign-callback bridge
lands in Phase 3).  Provisioner output (`n00b_quic_cert_t`) carries
a PEM chain + a secret key handle; the bring-up code converts the
PEM leaf to DER and routes the key through.

### 3. Renewal → hot-reload

When `should_renew(current)` returns `true`, the supervisor calls
`acquire()` again, then:

```c
n00b_quic_endpoint_reload_cert(
    ep,
    (n00b_quic_cert_reload_t){
        .cert_der_bytes = …new cert leaf DER…,
        .cert_der_len   = …,
        .key_pem_path   = …new key path…,
    });
```

Already-handshaked connections continue with their negotiated
session keys; new handshakes after the swap see the new cert.
There's a microsecond-scale race window during the actual swap
where a handshake-in-flight could observe a torn cert/key pair —
the swap isn't fully atomic per-cnx.  Renewals fire on the order
of weeks; clients retry transient handshake failures.  The
per-cnx atomic version of the swap (via picotls's
`on_client_hello` hook) is a Phase 2 follow-up and uses the
already-shipped `n00b_quic_cert_store_t` data structure.

---

## Renewal cadence

The default for ACME provisioners is `now > not_after - 30 days`.
Configurable per provisioner via the `renew_margin_ms` kwarg on the
constructor.  With Let's Encrypt's 90-day certs, this means a
60-day operating window with 30 days of slack.

A supervisor (the deployment playbook's job) runs:

```c
n00b_quic_cert_t *current = …last acquired cert…;
while (running) {
    if (provisioner->should_renew(provisioner, current)) {
        auto next = provisioner->acquire(provisioner);
        if (n00b_result_is_ok(next)) {
            current = n00b_result_get(next);
            n00b_quic_endpoint_reload_cert(
                ep,
                (n00b_quic_cert_reload_t){
                    .cert_der_bytes = …,
                    .cert_der_len   = …,
                    .key_pem_path   = …,
                });
        } else {
            /* log + back off; old cert is still valid until expiry */
        }
    }
    sleep_minutes(60);
}
```

The check cadence (1 hour above) is conservative; renewals are
rare so missing one slot by a few hours is fine.

---

## Failure modes

| Failure                              | Detected by             | Behavior                                                                 |
|--------------------------------------|-------------------------|--------------------------------------------------------------------------|
| `chain_pem_path` missing             | `--preflight`           | ERROR; `report.ok = false`; supervisor refuses to start.                 |
| `chain_pem_path` is a non-PEM file   | `--preflight`           | ERROR with the openssl-suggestion remediation.                           |
| Cert in chain is expired             | `--preflight`           | ERROR; the static provisioner won't return an expired cert at acquire.   |
| Cert expires in < 30 days            | `--preflight`           | WARN; deployment proceeds; supervisor's renewal loop should kick in.     |
| ACME network failure during renewal  | provisioner.acquire     | err returned; supervisor backs off and retries; old cert still valid.    |
| `external` command fails (non-0)     | provisioner.acquire     | err returned; same back-off behaviour.                                   |
| Renewal succeeds but reload fails    | endpoint.reload_cert    | err returned; old cert still on the wire; supervisor logs + retries.    |

---

## See also

- `include/net/quic/manifest.h` + `docs/net/quic/manifest.md` — manifest
  schema and preflight checks.
- `include/internal/net/quic/cert_provisioner.h` — provisioner trait.
- `include/internal/net/quic/cert_store.h` — the SNI-keyed cert store
  data structure (foundation for the per-cnx atomic SNI routing
  Phase 2 follow-up).
- `~/dd/quic_2.md` § 6 + § 10 — design rationale for hot-reload
  and the cert provisioning state machine.
