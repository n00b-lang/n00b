# Let's Encrypt staging interop

How to verify the n00b/QUIC ACME client end-to-end against a real
ACME server (Let's Encrypt's staging environment), using the
manual DNS-01 challenge provider.

This is **not** a CI test — running it requires a real DNS name
under your control, a way to add TXT records at
`_acme-challenge.<that-name>`, and outbound network access to
`acme-staging-v02.api.letsencrypt.org`.  Operator runs this
once per release as part of the n00b/QUIC sign-off.

The corresponding test against a per-test step-ca instance is
`test/unit/test_quic_acme_session.c` and runs in CI; that test
exercises the *protocol* mechanics.  This one exercises *interop*
with a real production-shape ACME server.

---

## Prerequisites

1. A DNS name you control, e.g., `acme-test.example.com`.  Subdomain
   carved out of an existing zone is fine.  Apex / wildcard certs
   work too but the simplest first run is a single subdomain.
2. Access to set TXT records at `_acme-challenge.<name>`.  The
   manual provider will prompt you to add the record by hand and
   wait for confirmation.
3. The n00b/QUIC build with the `quic_acme_demo` binary present:

   ```bash
   bash build.sh
   ls build_debug/quic_acme_demo
   ```

---

## Run

```bash
build_debug/quic_acme_demo \
    --directory https://acme-staging-v02.api.letsencrypt.org/directory \
    --domain    acme-test.example.com \
    > acme-test.example.com.chain.pem
```

What happens, in order:

1. The client opens the ACME directory and registers an account
   key (ephemeral, generated freshly per run).
2. It places a new order for `acme-test.example.com`.
3. It walks the order's authorizations.  For DNS-01, it computes
   the TXT value (RFC 8555 § 8.4: `base64url(SHA-256(<key authz>))`)
   and prints a prompt to stderr like:

   ```
   [n00b acme] Please add the following DNS TXT record:
     Name:  _acme-challenge.acme-test.example.com
     Value: "C7…43-char-base64url…f0"

   Then press ENTER to continue, or Ctrl-C to abort.
   ```

4. **You add the record** in your DNS console of choice.  Wait
   30-60 seconds for global propagation (Let's Encrypt's
   validators query from multiple geographic locations).
5. **Press ENTER** in the demo's terminal.  The demo POSTs the
   challenge "ready" signal and polls for the validation result.
6. On success, the demo:
   - Builds a CSR for the domain (ECDSA-P-256).
   - Submits it to the order's finalize URL.
   - Polls the order until status="valid".
   - Downloads the issued cert chain.
   - Writes the PEM bytes to **stdout**.
   - Prints a one-line cleanup hint to stderr.
7. **You remove the TXT record** in your DNS console.

---

## Verify the cert

```bash
openssl x509 -in acme-test.example.com.chain.pem -text -noout | head -40
```

Expected to show:
- `Issuer: C=US, O=(STAGING) Let's Encrypt, CN=(STAGING) ...`
- `Subject:` with your domain in `subjectAltName`.
- `Validity` — Let's Encrypt staging issues 90-day certs (same
  shape as production).

The cert chain is **not trusted by the OS trust store** — Let's
Encrypt staging is signed by their staging CA, which most systems
do not include.  This is correct and expected.  For production
use, point `--directory` at
`https://acme-v02.api.letsencrypt.org/directory` (production).

---

## Common failure modes

**Symptom**: demo prints "ACME staging request failed: err=-9 (protocol violation)"

- DNS record not yet propagated.  Wait longer, then press ENTER.
- TXT value typoed.  Re-check the prompted value byte-for-byte.

**Symptom**: order status stays "pending" forever

- The ACME server is hitting your DNS but not finding the record.
  Check with `dig +short TXT _acme-challenge.<name>` from a
  geographic location *other* than where you set the record — LE
  validates from multiple regions.

**Symptom**: cert chain is empty

- Order's finalize call succeeded but cert URL hasn't been
  populated yet.  This is a poll-timing thing; bump the
  `--poll-max-wait-ms` (not currently a CLI flag — adjust the
  demo's `n00b_acme_acquire_certificate` call site).

**Symptom**: rate-limit error

- Let's Encrypt staging has soft rate limits (much higher than
  production: ~30k account creations / 3h).  If you hit one,
  wait an hour and use `--account-key keychain:something-stable`
  to reuse an existing account.

---

## Rate-limit + key reuse

Repeated test runs should reuse a single account key to avoid
hitting account-creation limits:

```bash
# First run — generate a stable account key in the macOS Keychain
# (or wherever your secret provider points).  Once.
build_debug/quic_acme_demo \
    --directory https://acme-staging-v02.api.letsencrypt.org/directory \
    --domain    acme-test.example.com \
    --account-key keychain:com.n00b.le-staging-account \
    > /tmp/cert1.pem
```

The Keychain provider isn't fully wired through Phase 2
(`ephemeral:` is the only fully-wired secret URI scheme today —
Phase 3 trust-bridge work changes this).  Until then, use:

```bash
--account-key ephemeral:le-staging-acct
```

…and accept that each demo run creates a fresh account.  Rate
limits at 30k/3h give you plenty of headroom.

---

## What this verifies

| Area              | Verified                                                                  |
|-------------------|---------------------------------------------------------------------------|
| HTTPS shim        | Real TLS 1.3 handshake against LE staging through SecTrust / libssl       |
| ACME directory    | JSON parser handles the production-shape directory                        |
| Account creation  | jwk-form JWS, JWK thumbprint, key authorization                           |
| Order placement   | kid-form JWS, identifier list                                             |
| Authz fetch       | POST-as-GET with kid-form                                                 |
| DNS-01 challenge  | TXT-value computation per RFC 8555 § 8.4                                  |
| Manual provider   | Prompt + ENTER round-trip                                                 |
| Finalize          | CSR DER bytes + base64url + JSON wrap                                     |
| Cert pickup       | PEM-chain response decoding                                               |

The cert chain bytes that come out are **the actual on-the-wire
output of a real CA**.  Anything else is a bug in our client.

---

## Going to production

When the demo works end-to-end against staging, repeat with
`--directory https://acme-v02.api.letsencrypt.org/directory`
to get a *real* publicly-trusted cert.  Production rate limits
are stricter (5 duplicate certs / 7 days; 50 certs / 7 days per
domain) so use sparingly.

For long-term automation, this demo is *not* the right tool —
the manifest + `quic_server_managed` example wraps the same
mechanics with a deployment-grade renewal loop.  Once Phase 2's
cloud DNS providers (Cloudflare, Route53, GCP DNS) ship, the
manifest can declare the DNS provider directly and renewal
becomes fully automated:

```json
"cert": {
  "kind": "acme",
  "directory_url": "https://acme-v02.api.letsencrypt.org/directory",
  "subject_names": ["api.example.com"],
  "challenge": "dns-01",
  "dns_provider": {
    "kind": "cloudflare",
    "api_token": "vault:...",
    "zone_id":   "..."
  },
  "account_key_uri": "keychain:..."
}
```

---

## See also

- `examples/quic_acme_demo/main.c` — the demo binary's source.
- `docs/net/quic/manifest.md` — the deployment-grade configuration shape.
- `docs/net/quic/cert_lifecycle.md` — how renewal works after the
  initial acquisition.
- `~/dd/quic_2.md` § 5 + § 12.4 — design rationale for ACME +
  manual interop.
