# Signing Identities

This document describes how `n00b_chalk_pe_resign` (and, in WP-005
Phase 5, `n00b_chalk_macho_resign`) discover the cert + private
key used to sign a binary.

## URI shapes

`n00b_chalk_signer_identity_resolve(uri)` accepts three URI shapes:

### `file://path/to/cert.pem,file://path/to/key.pem`

Two paired `file://` URIs separated by a single comma. The first
URI points at an X.509 PEM certificate; the second points at a
PKCS#8 PEM RSA private key.

Both halves are required. The resolver does not perform shell
expansion; paths are taken verbatim from the URI.

### `store://<name>`

XDG store lookup. The resolver searches:

- `$XDG_CONFIG_HOME/n00b-attest/signing-identities/<name>.cert.pem`
- `$XDG_CONFIG_HOME/n00b-attest/signing-identities/<name>.key.pem`

If `$XDG_CONFIG_HOME` is unset, the resolver falls back to
`$HOME/.config/n00b-attest/signing-identities/` — matching the
D-052 disposition for `registries.json`.

Both `<name>.cert.pem` and `<name>.key.pem` must exist; missing
either half yields `Err(N00B_CHALK_ERR_SIGNER_IDENTITY_NOT_FOUND)`.

### `nullptr` URI

`n00b_chalk_signer_identity_resolve(nullptr)` returns
`Ok(nullptr)`. Callers pass that through to
`n00b_chalk_pe_resign(.signer_identity = nullptr)` to opt into
**strip-only mode** — any prior Authenticode signature is removed
from the binary and the result is written back unsigned, with a
structured warning emitted to stderr.

## Cert + key file pairing

Both halves of a `store://` identity must use the same basename:
`<name>.cert.pem` for the certificate and `<name>.key.pem` for the
key.

Single-file PKCS#12 / `.p12` / `.pfx` bundles are **not** supported
in WP-005 — that is a future ergonomics WP if a real consumer asks
for it. Mixing a `file://` cert with a `store://` key (or vice
versa) is also not supported.

## Caller-passed-handle shape

A caller-passed-handle identity shape — analogous to the OCI auth
caller-handle precedent — is a future ergonomics WP. WP-005 does
not need it for the v1 Crayon-side consumer.

## Deterministic signing

WP-005 ships **without** an RFC 3161 timestamp authority
(counter-signature). The signature is computed deterministically
over the input bytes plus the signer identity — repeated calls
with the same inputs produce byte-identical output. A future
ergonomics WP may add a `--timestamp-url <url>` kwarg if real
consumers request it.

## Allocator discipline

`n00b_chalk_signer_identity_resolve` accepts `.allocator = nullptr`
(default: runtime). The opaque identity handle stores the
allocator and threads it through every subsequent allocation
(cert / key DER copies, issuer DN slice, serial bytes).

`n00b_chalk_signer_identity_release(id)` scrubs the private-key
bytes (the PKCS#8 key DER and the cached serial bytes) before
returning. The handle itself is GC-tracked / arena-tracked — the
release call does not free the backing storage; it merely makes
sure a subsequent GC sweep or arena reset leaves no key residue
in memory.
