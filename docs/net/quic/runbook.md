# n00b QUIC — Operator Runbook

Phase 5 § 5.13.  Incident-response runbook for n00b QUIC + RPC
deployments.  Each entry follows the **symptom → triage →
root causes → remediation → verify** template.

The first half is **shared** across both reference playbooks
(`playbook_k8s.md` and `playbook_baremetal.md`); each playbook
appends its own platform-specific section after the shared
core.

## Conventions

- Commands assume the operator can `kubectl exec` into a pod
  (K8s) or `ssh` to the host running n00b (VM).  Substitute as
  needed.
- "metrics scrape" means `curl -s <metrics-host>:<port>/metrics`.
- "JSONL audit" means tailing the audit-sink file or its
  sidecar log shipper output.

---

## Shared core entries

### 1. Cert renewal failed

**Symptom**: clients begin failing TLS handshake with
`unexpected_message` or `bad_certificate`; alerts fire on
`n00b_quic_cert_expiry_seconds < 86400`.

**Triage (≤ 5 min)**:
1. `n00b_quic_cert_expiry_seconds{endpoint="..."}` value — is
   the cert about to expire, expired, or already replaced?
2. Tail the audit sink for `cert.renewed` events (Phase 2 ACME
   provisioner emits one per successful renewal).
3. Cluster-side: `kubectl describe certificate -A` (variant A)
   or `journalctl -u uacme.timer` (bare-VM).

**Root causes**:
- ACME directory rate-limit (Let's Encrypt: 50 certs/week
  per registered domain — most common).
- DNS-01 challenge propagation delay (DNS provider returned
  cached negative response).
- ACME account key revoked or unrecognised by the directory.
- Network egress blocked from the deployment to the ACME URL.

**Remediation**:
- **Rate-limit hit**: switch to LE staging temporarily; wait
  out the 7-day window; deploy with the staging-issued cert
  while the production limit clears.
- **DNS-01 delay**: increase `dns_propagation_seconds` in the
  manifest's `cert.dns01` block; verify with `dig +trace
  _acme-challenge.<domain>`.
- **Account key revoked**: rotate via
  `n00b_quic_acme_account_register` against the directory and
  update `cert.account_key_uri` in the manifest.
- **Egress blocked**: confirm the directory URL resolves +
  responds from the n00b host; bypass corporate proxy if
  applicable.

**Verify**: `n00b_quic_cert_expiry_seconds` returns a value ≥
14 days; new TLS handshakes complete cleanly; audit sink shows
a `cert.renewed` event with the new validity window.

### 2. ACME directory unreachable

**Symptom**: preflight reports
`cert-acme-directory:<endpoint>: ERROR Cannot resolve ACME
directory host`; cert renewal cron logs show
`acme: directory_url HTTP error`.

**Triage**: from the n00b host, `curl -sv <directory_url>`.
If DNS fails, that's the proximate cause.  If DNS succeeds
but TLS fails, check `n00b_quic_trust_*` config — the OS
trust store may have lost the LE intermediate.

**Root causes**: corporate egress blocking; ISO-2 root CA
bundle out-of-date; ACME provider DDoS / outage (rare).

**Remediation**: open egress to the directory URL; refresh
the OS CA bundle (`update-ca-certificates` on Debian);
swap `cert.directory_url` to a backup ACME provider while
the primary recovers (Phase 2 manifest supports per-endpoint
override).

**Verify**: `curl <directory_url>` returns the expected
JSON directory document.

### 3. IdP unreachable / JWKS stale

**Symptom**: clients receive
`UNAUTHENTICATED (n00b-rpc-status: 16)` for previously-working
tokens; `n00b_quic_audit_events_total{decision="deny"}` rate
spikes.

**Triage**:
1. From the n00b host, `curl <issuer>/.well-known/openid-configuration`.
2. Check JWKS endpoint reachability + response.
3. `kubectl logs <n00b-pod> | grep "jwks fetch"` for fetch errors.

**Root causes**: IdP outage; JWKS rotation while n00b's cache
is stale; clock skew between n00b and IdP > leeway window;
compromised key revoked + new key not yet propagated.

**Remediation**:
- **IdP outage**: failover-domain config; client-side
  short-circuit to a cached "graceful degradation" mode if
  applicable.  Service-to-service: cache last-known-good
  JWKS for a short read-only window.
- **JWKS rotation**: bump `cache_ttl_seconds` to a smaller
  value, or trigger an immediate refresh via the
  configurable `n00b_oidc_refresh()` hook.
- **Clock skew**: enable NTP / chrony.
- **Compromised key**: invalidate cached JWKS, force a fetch.

**Verify**: tokens minted by the IdP validate; deny rate
returns to baseline.

### 4. JWKS rotation skew

**Symptom**: a rolling subset of requests fail with
`AUTH_KEY_NOT_FOUND` or `AUTH_TOKEN_INVALID` while others
succeed; the failures correlate with replicas that haven't
refetched the JWKS yet.

**Triage**: `kubectl exec` into each n00b pod and dump its
in-memory JWKS state (env-flag debug feature).  The pods
that report the OLD set are skewed.

**Root causes**: independent per-replica JWKS caches with
TTLs that drift; one replica fetched at minute 0, the next
at minute 15, IdP rotated at minute 10.

**Remediation**: shorten `cache_ttl_seconds` (Phase 3 default
is 600s = 10 min — too long for this scenario; 60-120s is
better for active deployments).  Consider a shared external
JWKS cache (Redis / memcached) with explicit invalidation —
documented as a Phase 6 enhancement.

**Verify**: all replicas converge on the same JWKS within the
new TTL window; deny rate flat.

### 5. DPoP replay-cache full

**Symptom**: spike in `n00b_quic_audit_events_total{decision="deny",reason="DPOP_FAILED"}`;
inspecting the audit JSONL shows entries with the same `jti`
across short windows.

**Triage**: check the replay-cache size config; confirm
the deployment serves more requests/sec than the cache holds
× DPoP-jti-uniqueness rate.

**Root causes**: cache capacity (default 1024) is undersized
for the deployment's QPS × DPoP-token reuse; bug in the
client minting DPoP proofs with non-unique jti values.

**Remediation**: bump `n00b_dpop_replay_store_new(.capacity = N)`
to ~10× the QPS-second window the deployment needs to cover.
If the issue is client-side jti collisions, the client's DPoP
implementation has a bug — reach out.

**Verify**: false-replay rate returns to ~0; legitimate
replays (actual replay attempts) still get denied.

### 6. Audit sink disk full

**Symptom**: audit JSONL writes fail; the audit subscriber
falls back to stderr (a Phase 3 design choice), so the journal
fills.

**Triage**: `df -h` on the audit-sink path; check log-rotation
config (logrotate, k8s logging shipper).

**Root causes**: log shipper failure (fluentbit / promtail
pod not running); rotation cron disabled; runaway audit rate
during an attack.

**Remediation**: restart the log shipper; rotate the file
manually; if attack-driven, deploy rate-limiting at the
edge.

**Verify**: free disk on the audit path > 20%; new audit
entries land in the JSONL file.

### 7. Audit sink write failures

**Symptom**: audit JSONL has gaps (timestamps don't increase
monotonically by per-call deltas); error counter for the sink
ticks up.

**Triage**: tail the sink with `tail -F` while load-testing —
do new entries appear?  Check the sink's open file descriptor.

**Root causes**: file rotated under the sink (the sink holds
an FD to the original path; rotation moves the file).

**Remediation**: configure logrotate with `copytruncate`
instead of `create`, OR configure n00b to re-open the FD on
SIGHUP.

**Verify**: monotonic timestamps; no gaps under load.

### 8. Handshake failures spiking

**Symptom**: `n00b_quic_chan_opens_total` rate drops; clients
report `connection refused` or `handshake timeout`.

**Triage**: from a known-good client, `n00b-quic ping
<endpoint>` (or equivalent debug tool) and watch the qlog
output.  Compare against `n00b_quic_audit_events_total` —
is this happening at the TLS layer or post-handshake auth?

**Root causes**:
- ALPN mismatch (peer expects `h3`, server advertises
  something else).
- Cert chain verification fails on the client (corporate
  trust store missing the LE intermediate).
- UDP path-MTU drop (clients on a network that drops > 1300-byte
  packets without ICMP).

**Remediation**:
- ALPN: align via the manifest's `endpoints[].alpn` array.
- Trust: distribute the missing intermediate.
- MTU: lower `max_datagram_size` in the picoquic config.

**Verify**: handshake rate recovers; qlog shows a clean
TLS handshake on a sample request.

### 9. GOAWAY storm

**Symptom**: clients see `n00b-rpc-status: UNAVAILABLE` for
new requests; existing in-flight requests complete fine.

**Triage**: `kubectl logs <n00b-pod> | grep GOAWAY` — is
the pod proactively GOAWAYing?  This happens during graceful
shutdown.

**Root causes**: rolling restart in progress; pod-disruption-
budget exceeded; cascading pod evictions due to memory
pressure.

**Remediation**: stagger restarts; raise PDB; check memory
limits on the pod.

**Verify**: GOAWAY rate returns to ~0; new request rate
recovers.

### 10. qlog disk pressure

**Symptom**: pod memory or ephemeral-storage usage approaching
limit; `qlog_dir` growing without bound.

**Triage**: `du -sh <qlog_dir>` on the pod.  Check
qlog-rotation config.

**Root causes**: qlog enabled in production by accident
(should be debug-only); qlog dir on an unbounded volume.

**Remediation**: disable qlog (`qlog_dir = nullptr` in
manifest) for steady-state ops; enable selectively only when
debugging; mount qlog dir on a sized ephemeral volume with
log-shipper-driven rotation.

**Verify**: pod storage usage stable; qlog dir empty (or
small).

### 11. Stream budget exhaustion

**Symptom**: clients receive `STREAM_LIMIT_ERROR`; new RPC
calls fail with the same error code.

**Triage**: `n00b_quic_chan_active{kind="framed"}` per pod;
compare to the configured `MAX_STREAMS` budget.

**Root causes**: client opening streams faster than they
close; long-lived streams (server-stream, bidi) accumulating
without backpressure feedback.

**Remediation**: increase `MAX_STREAMS` in the manifest's
endpoint config; client-side: tear down completed streams.
Persistent: implement backpressure via `n00b-rpc-deadline`
on long calls.

**Verify**: stream budget returns to slack; rate of
`STREAM_LIMIT_ERROR` is zero.

### 12. mTLS client cert mismatch / rejected

**Symptom**: only mTLS-required RPCs fail; bearer-only RPCs
succeed.  Audit shows
`reason="MTLS_MISMATCH"`.

**Triage**: dump the client cert SHA-256 thumbprint client-side;
compare against the JWT's `cnf.x5t#S256` claim.  If they
differ, the IdP issued a token that doesn't match the cert.

**Root causes**: IdP minted the token with the WRONG
thumbprint (configuration error); cert rotated client-side
but token was minted before rotation.

**Remediation**: re-mint the token after cert rotation;
align IdP's mTLS-binding claim source to the actual presented
cert.

**Verify**: mTLS RPCs succeed; thumbprints align.

### 13. Replica skew on sticky-secret rotation

**Symptom**: in a multi-replica deployment behind an LB,
existing connections start dropping mid-flight after a rotation.
LB-CID routing changes.

**Triage**: confirm the sticky-secret Secret was updated;
check each replica's mounted Secret has the new value.

**Root causes**: rotation propagated to one replica but not
yet others; replicas using stale secrets disagree on CID
routing.

**Remediation**: trigger a rolling restart of all replicas
after the Secret update so they pick up the new value
simultaneously (close-enough); for zero-downtime, use a
rotation overlap where both old + new secrets are accepted
for a window.

**Verify**: no mid-flight drops; replicas converge on the
same secret.

### 14. Endpoint won't bind (port conflict, missing capability)

**Symptom**: pod CrashLoopBackoff; logs show
`n00b_quic_endpoint_new: BIND_FAILED`.

**Triage**: `lsof -i :443` (or whatever port) on the host;
`getcap` on the n00b binary if running on bare-metal Linux.

**Root causes**: another process (nginx, haproxy) already
holds the port; binary missing `cap_net_bind_service` for
privileged ports; SELinux/AppArmor blocking the bind.

**Remediation**: free the port; `setcap cap_net_bind_service=+ep`
on the binary; configure SELinux/AppArmor to allow.

**Verify**: pod transitions to Running; `n00b_quic_chan_active`
gauge starts moving.

### 15. Preflight failed at startup

**Symptom**: pod doesn't transition to Running; logs show
`preflight: ERROR <check_id> <detail>`.

**Triage**: read each ERROR finding's detail + remediation
strings.

**Root causes**: any of the above (cert path missing, IdP
unreachable, etc.) detected at startup.

**Remediation**: address each ERROR; ignore INFO + WARN.

**Verify**: preflight exits 0; pod becomes Running.

---

## K8s playbook appendix

### K8s.1: Pod CrashLoopBackoff

**Symptom**: `kubectl get pods` shows CrashLoopBackoff.

**Triage**: `kubectl describe pod` + `kubectl logs --previous`.

**Common causes**: image pull error (kind didn't load the
image, or the image tag drifted); preflight failure (see
shared #15); OOMKilled (resource limits too tight).

**Remediation**: re-load the image (`kind load
docker-image n00b-phase5-demo:latest`); fix preflight; bump
limits.

### K8s.2: cert-manager Order stuck (variant A)

**Symptom**: `kubectl get certificate` shows `Ready=False`;
no Secret materializes; n00b deployment can't pick up the
cert.

**Triage**: `kubectl describe order <order>` + `kubectl
logs cert-manager`.

**Common causes**: DNS-01 propagation hasn't completed;
solver-pod can't reach the ACME directory; ClusterIssuer
referencing wrong account key.

**Remediation**: per-issuer; LE staging will tell you the
DNS-01 hasn't propagated; bump propagation TTL.

### K8s.3: Envoy 503 vs n00b 5xx — distinguishing

**Symptom**: client sees 503 but the n00b pod logs no
matching request.

**Triage**: Envoy access log; compare timestamps.  503 from
Envoy = upstream not reachable.

**Common causes**: `nodePort` config drift; pod readiness
probe failing; Envoy config map references wrong service.

### K8s.4: Secret mount stale after rotation

**Symptom**: after rotating a Secret, the n00b pod still uses
the old value.

**Triage**: K8s mounts Secrets as projected volumes that
auto-update; n00b reads the file at startup, not on every
request.  So the rotation is propagated to disk but n00b
hasn't reread.

**Remediation**: `kubectl rollout restart deployment/n00b-phase5`.

### K8s.5: Pod-to-replica routing via LB-CID

**Symptom**: Envoy-fronted multi-replica deployment isn't
routing connections stickily; new connections always go to
the same pod.

**Triage**: `n00b_quic_chan_opens_total` per pod — is it
spread across replicas?  If 100% to one pod, LB-CID isn't
hashing.

**Common causes**: Envoy's QUIC LB hash config not aligned
with n00b's CID encoding; sticky-secret not synced.

**Remediation**: align Envoy `connection_id_generator` config
with n00b's `lb_cid_encoding_key`.

### K8s.6: Bitnami chart install fails "manifest unknown"

**Symptom**: `helm install ... bitnami/keycloak` (or any other
Bitnami chart) returns
`Failed to pull and unpack image "docker.io/bitnami/<x>:<tag>":
manifest unknown` on every pod.

**Triage**: as of August 2025 Bitnami stopped publishing free
images under `docker.io/bitnami/<name>` and mirrored their
final image set under `docker.io/bitnamilegacy/<name>` — the
charts still reference the old `bitnami/` repo, so installs
fail until you redirect.  Look for the chart's NOTICE banner
("Starting August 28th, 2025, only a limited subset of
images/charts will remain available for free…").

**Remediation**: in the chart's values, override every image
ref to `docker.io/bitnamilegacy/<name>:<original-tag>` *and*
set `global.security.allowInsecureImages: true` to clear the
chart's NOTES.txt tripwire.  Phase 5's K8s fixture
(`test/integration/phase5_k8s/helm/keycloak-values.yaml`) is
the canonical example; mirror that pattern for any future
Bitnami-chart consumer.  Long-term, migrate off the Bitnami
chart altogether (Keycloak Operator, codecentric/keycloakx,
or vendor-direct).

### K8s.7: Keycloak realm-import users cannot password-grant

**Symptom**: realm imported via `--import-realm` looks healthy
(`/.well-known/openid-configuration` returns 200) but every
password grant fails with
`{"error":"invalid_grant","error_description":"Account is not
fully set up"}`.

**Triage**: Keycloak 26.x defaults the realm user-profile to
require `firstName` and `lastName` for the `user` role;
imported users without those fields trigger the
`VERIFY_PROFILE` action at login, which the password grant
rejects.

**Remediation**: add `"firstName": "...", "lastName": "..."`
to every imported user entry in the realm JSON.  Alternatively
relax the realm's user-profile (`required: { roles: [] }`),
but populating the fields is less surprising for downstream
consumers that read the same JSON.

### K8s.8: cert-manager Challenge stuck on "self check" DNS error

**Symptom**: `kubectl describe challenge` shows status
"Waiting for HTTP-01 challenge propagation: failed to perform
self check GET request '...': dial tcp: lookup <hostname> on
10.96.0.10:53: no such host".

**Triage**: cert-manager's controller does its own GET against
the challenge URL *before* telling the ACME server to validate
— this is a fail-fast against badly-configured Ingresses.  When
the cert hostname isn't in cluster DNS (typical for closed test
environments using Pebble or any in-cluster CA), the self-check
hits NXDOMAIN and the Order never progresses.

**Remediation**: get the hostname into cluster DNS via a
standard K8s primitive — *don't* patch CoreDNS or per-pod
hostAliases.  Two clean patterns:

1. **Use an in-cluster Service FQDN as the cert hostname**
   (`<svc>.<ns>.svc.cluster.local`).  kube-dns auto-resolves it;
   no extra config needed.  This is the Phase 5 fixture's choice
   — see `test/integration/phase5_k8s/cert-manager/n00b-svc.yaml`
   for an `ExternalName` Service that aliases the cert hostname
   to ingress-nginx, so HTTP traffic still reaches the
   cert-manager-spawned challenge solver.
2. **Issue the cert directly for ingress-nginx's FQDN**
   (`ingress-nginx-controller.ingress-nginx.svc.cluster.local`)
   if you don't need a stable n00b-shaped name yet.

The CoreDNS-patch and hostAliases workarounds *do* work, but
they leak fixture concerns into shared cluster config and into
every Pod that needs the resolution — not worth it.

---

## Bare-VM playbook appendix

### VM.1: uacme cron failed

**Symptom**: cert expiry approaches without renewal.

**Triage**: `journalctl -u uacme.timer` + `journalctl -u
uacme.service`.

**Common causes**: DNS hook not authenticated (wrong API
token); ACME account locked; uacme binary upgraded with
breaking flags.

### VM.2: systemd unit restart loop

**Symptom**: `systemctl status n00b-quic` shows multiple
recent restarts.

**Triage**: `journalctl -u n00b-quic --since "5 min ago"`.

**Common causes**: same as K8s.1 (preflight, OOM, port).

### VM.3: ZITADEL handshake error after upgrade

**Symptom**: tokens minted for the new ZITADEL realm don't
validate post-upgrade.

**Triage**: dump the new realm's JWKS; compare against n00b's
cached set.

**Common causes**: ZITADEL rotated keys during upgrade and
n00b's TTL hasn't elapsed.

**Remediation**: restart n00b to force a JWKS refresh.

### VM.4: Privileged port without cap_net_bind_service

**Symptom**: same as shared #14.  Bare-VM specific because
podman/systemd handle caps differently than k8s.

**Triage**: `getcap /usr/local/bin/quic_phase5_demo`.

**Remediation**: `sudo setcap cap_net_bind_service=+ep
/usr/local/bin/quic_phase5_demo`; OR run on a non-privileged
port + use sysctl `net.ipv4.ip_unprivileged_port_start`.

### VM.5: DNS-01 hook failed (provider creds)

**Symptom**: cert renewal hook fails with 401/403.

**Triage**: `journalctl -u uacme.service | grep dns01`.

**Common causes**: API token expired; provider rotated
credentials.

**Remediation**: refresh the credential in the secret
provider (vault, KMS) and reload the systemd unit.

### VM.6: HAProxy lb-cid key mismatch

**Symptom**: under multi-host, connection migration breaks
even though all hosts are up — `n00b_quic_chan_opens_total`
spikes on every reconnect, `picoquic_get_local_cnxid` on the
new host doesn't match what the previous host issued.

**Triage**: HAProxy `quic lb-cid key-file` and the n00b hosts'
`/var/lib/n00b/lb-cid.key` must contain *byte-identical*
16-byte keys.  Hex-encoded files are a common foot-gun
(operator stores hex, n00b reads raw, key length looks right
but is actually the hex digits).

**Common causes**: trailing newline in the key file (`16` is
counted including the trailing `\n`); hex vs raw mismatch;
key rotated on the LB but not propagated to the n00b hosts.

**Remediation**: regenerate as raw bytes —
`openssl rand 16 > /var/lib/n00b/lb-cid.key && install -m 600
/var/lib/n00b/lb-cid.key /etc/haproxy/lb-cid.key`.  Restart
both sides.  Operators with multi-host rotation should drive
the rotation through `sticky_secret.h`'s coordination helpers
so the LB sees the new generation only once all hosts have
acknowledged.
