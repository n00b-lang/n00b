# QUIC Auth (Phase 3)

Phase 3 fills the gap between transport (Phase 1) and protocols
(Phase 4): given a connection, *is this caller allowed to do this?*

The answer is built on:

| Module                      | Role                                              |
|-----------------------------|---------------------------------------------------|
| `quic/trust.h`              | OS-trust verifier, wired into picotls.            |
| `quic/jwt.h`                | JWS verify, JWT claim validation.                 |
| `quic/oidc.h`               | OIDC discovery + JWKS cache.                      |
| `quic/dpop.h`               | RFC 9449 proof-of-possession.                     |
| `quic/mtls_token.h`         | RFC 8705 cnf.x5t#S256 binding.                    |
| `quic/auth_policy.h`        | Declarative `audience/issuer/claim/dpop/mtls`.    |
| `quic/audit.h`              | Per-decision allow/deny event fan-out.            |
| `quic/manifest.h`           | Loader for `auth.idps[]` + `auth.policies[]`.     |

## End-to-end flow

```
              ┌─────────┐  /.well-known/openid-configuration
              │   IdP   │ ─────────────────────────────►  n00b_oidc_t
              └─────────┘  /jwks                           (cached, refresh on miss)
                  │ mints                                          │
                  ▼                                                 ▼
                bearer_token ─►  n00b_jwt_verifier_new(...)  ─►  n00b_jwt_verify
                                                                   │
                                                                   ▼
                            n00b_quic_auth_policy_eval(p, creds) ─► claims
                                                                   │
                       audit ┐                                     ▼ (if require_dpop)
                  n00b_quic_audit_emit(allow|deny) ◄── DPoP verify (htm/htu/jti/jkt)
                                                                   │
                                                                   ▼ (if require_mtls)
                                                               cnf.x5t#S256 ↔ peer cert
```

## Manifest schema

```json
{
  "version": 1,
  "service_name": "demo",
  "endpoints": [ { "id": "ep", "bind_host": "0.0.0.0",
                   "bind_port": 4433, "alpn": ["h3"],
                   "cert": { "kind": "static",
                             "chain_pem_path": "...",
                             "key_secret_uri": "..." } } ],
  "auth": {
    "idps": [
      { "id": "primary",
        "issuer": "https://login.example.com",
        "jwks_cache_ttl_seconds": 3600 }
    ],
    "policies": [
      { "id": "rpc-readwrite",
        "idp": "primary",
        "audience": "checkout-api",
        "require_dpop": true,
        "require_mtls": false,
        "require_claim": [
          { "name": "scope", "contains": "rpc:write" },
          { "name": "role",  "equals":   "admin"     }
        ] }
    ]
  }
}
```

## Building a verifier

```c
#include "n00b.h"
#include "net/quic/manifest.h"
#include "net/quic/oidc.h"
#include "net/quic/auth_policy.h"

n00b_quic_manifest_t *m = n00b_result_get(
    n00b_quic_manifest_load_path("manifest.json"));

n00b_quic_preflight_report_t *r =
    n00b_result_get(n00b_quic_preflight(m));
if (!r->ok) { /* abort + print findings */ }

/* Discover the IdP. */
n00b_oidc_t *oidc = n00b_result_get(
    n00b_oidc_open(m->auth_idps[0]->issuer));

/* Realize the policy. */
n00b_quic_manifest_policy_t *mp = m->auth_policies[0];
n00b_quic_auth_policy_t *p = n00b_quic_auth_policy_new();
n00b_quic_auth_policy_require_audience(p, mp->audience);
if (mp->require_dpop) n00b_quic_auth_policy_require_dpop(p);
for (size_t i = 0; i < mp->required_claim_count; i++) {
    n00b_quic_manifest_required_claim_t *rc = mp->required_claims[i];
    if (rc->op == N00B_QUIC_MANIFEST_CLAIM_CONTAINS) {
        n00b_quic_auth_policy_require_claim_contains(p, rc->name, rc->value);
    } else {
        n00b_quic_auth_policy_require_claim(p, rc->name, rc->value);
    }
}

/* Per-request: build a verifier + present credentials. */
n00b_jwt_verifier_t *v = n00b_result_get(
    n00b_oidc_jwt_verifier(oidc, mp->audience));

n00b_quic_auth_credentials_t creds = {
    .bearer_token  = client_token,        /* from the wire */
    .jwt_verifier  = v,
    .dpop_header   = client_dpop_header,  /* if require_dpop */
    .htm           = "POST",
    .htu           = "https://api/checkout",
    .peer_cert_der = peer_cert_bytes,     /* if require_mtls */
    .peer_cert_len = peer_cert_len,
};
auto result = n00b_quic_auth_policy_eval(p, &creds);
if (n00b_result_is_ok(result)) {
    /* allow */
} else {
    /* deny — error code is one of N00B_QUIC_ERR_AUTH_* */
}
```

## Algorithm coverage (v1)

| alg     | Status                                |
|---------|---------------------------------------|
| ES256   | ✅                                    |
| ES384   | Deferred (uECC has no secp384r1)      |
| RS256   | ✅                                    |
| RS384   | Deferred (no SHA-384 in n00b yet)     |
| RS512   | Deferred (no SHA-512 in n00b yet)     |
| HS*     | Refused                               |
| EdDSA   | Deferred                              |
| `none`  | Refused                               |

DPoP `alg` is ES256 only in v1.  RSA-PSS is a follow-up.

## Audit events

`n00b_quic_audit_subscribe(fn, ctx)` registers a synchronous
subscriber.  A built-in JSONL sink is at
`n00b_quic_audit_jsonl_sink_open(path)`.

Event JSON shape:
```json
{
  "ts_ms": 1234567890000,
  "decision": "allow" | "deny",
  "reason": "ok" | "audience claim mismatch" | ...,
  "iss": "https://idp.example",
  "sub": "alice",
  "aud": "checkout-api",
  "jti": "...",
  "htm": "POST",
  "htu": "https://api/x",
  "policy_id": "rpc-readwrite"
}
```

## Demo

```
$ build_debug/quic_oidc_demo path/to/manifest.json
Loaded manifest: 1 IdP(s), 1 policy/-ies
Preflight: ok (5 finding(s))
Audit sink: /tmp/quic_oidc_demo.jsonl
policy 'rpc-rw': ALLOW (sub=alice)
Audit events appended to /tmp/quic_oidc_demo.jsonl
```

## Phase 4 hand-off

Phase 3 ships the algebra: policy + eval + audit.  Phase 4 (RPC)
wires the algebra into channel-bytes-blocking — the first frame on
a channel carries the bearer token + optional DPoP header; the RPC
layer calls `n00b_quic_auth_policy_eval` before any application
bytes flow.  Today an application can use the algebra directly by
parsing its own first message.

## Testing

Synthetic IdP: `test/fixtures/synthetic_idp.{h,c}`.  In-process,
no Docker.  Used by the offline `test_quic_synthetic_idp` end-to-
end test that exercises the entire stack including DPoP and mTLS-
bound thumbprints.

Keycloak interop: `test/fixtures/keycloak/start.sh` →
`test_quic_keycloak_interop`.  Manual; gated by `KEYCLOAK_*` env
vars.  Once-per-release per the project's interop convention.
