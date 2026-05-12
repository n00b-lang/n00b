# Vendored Pebble fixtures

Files in this directory are reused by the Phase 5 § 5.6 K8s
fixture (cert-manager variant A).  Only `pebble.minica.pem` and
`pebble-config.json` (the upstream config rewritten to point at
`/pebble/certs/`) are sourced verbatim from the Pebble repo.  The
TLS cert/key (`cert.pem`, `key.pem`) had to be re-signed locally
because the upstream pair only carries SANs `localhost`, `pebble`,
`127.0.0.1` — cert-manager addresses Pebble via the in-cluster
Service FQDN `pebble.pebble.svc` (cross-namespace from the
`cert-manager` namespace), which the upstream cert doesn't cover,
and the ACME directory fetch fails with `x509: certificate is
valid for localhost, pebble, not pebble.pebble.svc`.

## Origin

| File | Source | Notes |
|---|---|---|
| `pebble.minica.pem` | upstream `letsencrypt/pebble@main` `test/certs/pebble.minica.pem` | Mini-CA cert; pinned in cert-manager ClusterIssuer's `acme.caBundle` |
| `pebble-config.json` | upstream `letsencrypt/pebble@main` `test/config/pebble-config.json` | Single edit: cert + privateKey paths point at `/pebble/certs/` (the in-pod mount) |
| `cert.pem` | locally re-signed | Issued by the Mini-CA above; SANs cover all K8s-internal DNS variants of Pebble's Service |
| `key.pem` | locally generated | RSA-2048 server key |

## Re-signing the TLS cert

If the cert ever needs renewal or the SANs change, regenerate via:

```bash
# From this directory.  Requires the Mini-CA private key from
# upstream (test/certs/pebble.minica.key.pem) — fetch into a temp
# location; never vendor it into n00b.
ca_pem=pebble.minica.pem
ca_key=$(mktemp)
trap 'rm -f "$ca_key"' EXIT
curl -sSLf https://raw.githubusercontent.com/letsencrypt/pebble/main/test/certs/pebble.minica.key.pem \
    -o "$ca_key"

openssl genrsa -out key.pem 2048

cat > /tmp/san.cnf <<'EOF'
[req]
default_md = sha256
prompt = no
distinguished_name = dn
req_extensions = v3_req

[dn]
CN = pebble

[v3_req]
# n00b's Linux trust path uses OpenSSL's X509_verify_cert which
# rejects server certs that lack standard server-auth extensions.
# Match the upstream pebble.minica-signed cert's extension set
# verbatim so any verifier (Go's stdlib used by cert-manager,
# OpenSSL used by n00b) accepts the chain.
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
basicConstraints = critical, CA:FALSE
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = pebble
DNS.3 = pebble.pebble
DNS.4 = pebble.pebble.svc
DNS.5 = pebble.pebble.svc.cluster.local
IP.1 = 127.0.0.1
EOF

openssl req -new -key key.pem -out /tmp/pebble.csr -config /tmp/san.cnf
openssl x509 -req -in /tmp/pebble.csr -CA "$ca_pem" -CAkey "$ca_key" \
    -CAcreateserial -out cert.pem -days 7300 \
    -extensions v3_req -extfile /tmp/san.cnf
```

The Mini-CA private key is *not* vendored under n00b — it's only
fetched at re-sign time, used, and discarded.  Any committed
keypair signed by it is acceptable test crypto; the chain still
ends at the well-known upstream Mini-CA cert that's pinned in
`cert-manager/cluster-issuer.yaml`.
