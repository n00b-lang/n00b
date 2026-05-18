# QUIC — Security model + threat model (Phase 3)

This is the running threat model for the auth subsystem shipped
in Phase 3.  It complements `docs/net/quic/auth.md` (the user-facing
overview) and `~/dd/quic_3.md` (the design doc).

## In scope

- Bearer-token authentication for inbound requests on a QUIC
  channel: parse, verify the JWS, validate the JWT claim set.
- Proof-of-possession via DPoP (RFC 9449).
- mTLS-bound tokens via `cnf.x5t#S256` (RFC 8705).
- Per-channel declarative policy with audit events.
- OS-trust-store-backed verification of the QUIC handshake's
  peer certificate (server cert from the client's perspective;
  client cert when client-auth is enabled).

## Explicitly out of scope

- Token revocation.  We rely on short-lived tokens (`exp`
  enforced) and accept that revocation only takes effect on the
  next refresh.
- HSM-backed JWKS cache integrity proofs.  Cross-instance JWKS
  coherence is the operator's problem (typically: front-loaded
  by a CDN/LB).
- SAML, WS-Federation, Kerberos.  Out forever.
- An IdP-side ticket issuer.  We *consume* tokens, we don't
  *issue* them; an authorization server is its own product.

## Trust assumptions

| Component             | Trust                                            |
|-----------------------|--------------------------------------------------|
| picotls               | Vendored at known commit; treat as TCB.          |
| picoquic              | Vendored at known commit; treat as TCB.          |
| OS trust store        | Trusted to identify which CAs are valid.         |
| IdP issuer URL        | Trusted via TLS.  No `--insecure` override.      |
| JWKS bytes            | Authentic only via the TLS-verified channel.     |
| Replay store          | Defender, not authority — bounded local cache.   |

## Known risks + mitigations

### 1. `alg=none` and HS\* downgrade attacks

**Risk**: an attacker substitutes a token with `alg=none` or
`alg=HS256` (using a public-known secret), hoping the verifier
would skip signature checks or accept a guessable secret.

**Mitigation**: hard-coded refusal in `jwt.c`:

```c
if (alg == "none" || alg == "HS256" || alg == "HS384" || alg == "HS512") {
    return AUTH_ALG_REFUSED;
}
```

### 2. JWKS cache poisoning

**Risk**: an attacker substitutes a malicious JWKS at the IdP's
URL during cache refresh.

**Mitigation**: the JWKS fetch goes through the same OS-trust
verification path as ACME (`acme_http.c`).  No `--insecure`.

### 3. Token replay (DPoP)

**Risk**: an attacker replays a DPoP-bound bearer token + DPoP
header to issue duplicate side-effecting requests.

**Mitigation**: the optional `n00b_dpop_replay_store_t` rejects
re-uses of the same `jti`.  Default capacity 1024.  FIFO
eviction means a long-tail attacker who sends 1024 distinct
jtis between a victim's request and its replay COULD succeed.
Operators with high QPS or long replay windows should bump
capacity.

**Cross-instance replay**: the in-process replay store is per-
instance.  Multi-replica deployments need a Redis-backed sidecar
(deferred follow-up).

### 4. Trust-store bypass

**Risk**: an attacker presents a self-signed cert that the QUIC
handshake accepts.

**Mitigation**: there is no `--insecure` mode.  Endpoints with
`.trust = nullptr` default to OS-native trust
(`n00b_quic_trust_system`), which uses
SecTrust on macOS or libssl's `X509_verify_cert` on Linux.
Tests that need to bypass for a self-signed fixture must pass an
explicit `n00b_quic_trust_pinned(fp)`.

### 5. Mid-handshake cert reload tearing in-flight cnxs

**Risk**: a `cert_store_replace` during an in-flight handshake
could swap the cert/key pair mid-handshake, producing an
inconsistent CertificateVerify.

**Mitigation**: the `n00b_quic_cert_store_t` uses RCU-style swap
with an unbounded graveyard (Phase 2).  The picotls SNI router
captures the entry pointer at `on_client_hello`; subsequent
`emit_certificate` and `sign_certificate` calls read from the
captured entry, not the live view.  See
`test_quic_reload_race.c`.

### 6. mTLS-bound thumbprint substitution

**Risk**: an attacker presents a token issued for a different
client's cert and connects with their own cert.

**Mitigation**: when `require_mtls` is set, `auth_policy_eval`
checks `cnf.x5t#S256` against the SHA-256 of the client's
presented cert.  Mismatch → AUTH_MTLS_MISMATCH.

**Gap**: client-auth isn't yet wired through
`n00b_quic_endpoint_t`'s constructor.  Until that ships,
`peer_cert_der` is passed in by the caller.  The verify primitive
itself is correct.

### 7. Audit-log denial-of-service (slow subscriber)

**Risk**: a slow audit subscriber (e.g., remote log shipper)
blocks the policy-eval call site under the global mutex.

**Mitigation**: synchronous emit is documented as the v1 trade-
off.  Operators wanting async should subscribe a daemon that
posts to its own bounded queue.  Async fan-out is a v2.

### 8. Information leakage via audit events

**Risk**: audit events carry `iss`, `sub`, `jti`, `htu`, etc.
A misconfigured JSONL sink at world-readable path leaks identity.

**Mitigation**: operator's responsibility.  Default sink path
(when chosen) is `/tmp/quic_oidc_demo.jsonl` — a demo path, not
a production one.  The recommended deployment location is
under `/var/log/<service>/` with mode 0640 and a dedicated
user/group.

## Cryptographic primitives

| Primitive | Source         | Audit                                    |
|-----------|----------------|------------------------------------------|
| SHA-256   | n00b vendored  | Stable; used in JWS sign+verify, mTLS    |
| ECDSA-P-256 | uECC (picotls deps) | Standard implementation             |
| AES-128   | cifra          | via picotls minicrypto                   |
| RSA-PKCS1-v1_5 | hand-rolled in `rsa_verify.c` | Verify-only; small public-e modexp |

The hand-rolled RSA verifier in `rsa_verify.c` is the v1's
highest-risk piece (it's our own bignum).  Mitigations:

- Verify-only; we never sign with RSA.  No timing-side-channel
  on a private key.
- Constant-time equality on the recovered EM vs. expected EM.
- Pinned against a known-good RFC 7515 Appendix A.2 vector in
  `test_quic_jwt.c`.
- Bignum modexp uses the public exponent only (typically 65537,
  17 mulmod operations), so attacker-controlled timing has no
  signal to recover.

## QUIC LB-CID

(Phase 5 § 5.8.)  When `n00b_quic_endpoint_new(.lb_cid_config = ...)`
is set, every server-issued connection ID is AES-128-encrypted in
the *block-cipher mode* of
[draft-ietf-quic-load-balancers](https://datatracker.ietf.org/doc/draft-ietf-quic-load-balancers/),
embedding a `<server_id>||<nonce>` payload that an LB-CID-aware
load balancer (Envoy contrib's QUIC LB filter, HAProxy 3.x
`lb-cid` mode) can decode without breaking the QUIC handshake's
opacity to other on-path elements.

Threat model:

- **Key disclosure** breaks the routing privacy property — an
  attacker who recovers the 16-byte LB key can decode any
  on-path n00b-issued CID and learn which replica handles
  which connection.  Disclosure does *not* break the QUIC
  handshake's confidentiality (which is anchored in TLS, not
  the CID), and does *not* let the attacker forge or hijack
  connections — the CID is opaque to QUIC's authentication
  envelope.  Treat the LB key like any middlebox-shared
  secret: rotate on a schedule via `sticky_secret_t`'s
  coordination helpers; provision via the operator's secret
  manager, not Git.
- **Replica-affinity inference** — anyone observing the wire
  with the LB key can correlate post-migration packet flights
  to the same replica.  This is the *intended* property; the
  attacker holding the key is already inside the LB's trust
  boundary.
- **Nonce collisions** — the nonce space (128 − 8·`server_id_len`
  bits, ≥ 120 bits with the 1-byte server-id default) is
  large enough that birthday collisions are not a practical
  concern.

The block-cipher mode is the most secure of the three modes
the draft defines (the other two — plaintext and stream-cipher
— leak the server-id without keys).  n00b ships only the
block-cipher mode at this revision.

## Phase 4 implications

When Phase 4 lands (RPC layer), the auth pipeline will be
wired into channel-bytes-blocking: a channel's first frame
carries the bearer token + optional DPoP header; the RPC layer
calls `n00b_quic_auth_policy_eval` before delivering any
application bytes.  Until then, the algebra is application-
visible — applications parsing their own first messages can
call `auth_policy_eval` themselves.

## Reporting

Security issues should be filed via the standard project
process.  See `~/dd/quic_3.md § 18` for acceptance criteria,
including the no-`--insecure`-anywhere line-item.
