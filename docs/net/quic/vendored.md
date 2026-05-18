# Vendored libraries — picoquic + picotls

This document is the source of truth for the upstream pin used by the
n00b QUIC module.  Both libraries live under `subprojects/` and were
imported from a shallow clone of upstream (the inner `.git/` was
stripped, so the working copy contains a clean snapshot at the pinned
commit).  Re-vendoring follows the procedure in § Update.

## Pins

| Library | Upstream | Commit | License | Imported |
| ------- | -------- | ------ | ------- | -------- |
| picotls  | https://github.com/h2o/picotls          | `cfe2ea97ac8fa0b2852cbb8dbdde1ecc83b4ca33` (master @ 2026-05-07) | MIT     | 2026-05-07 |
| picoquic | https://github.com/private-octopus/picoquic | `f54239a39145aa3042903c7c61609b0609e804c9` (master @ 2026-05-07) | BSD-3   | 2026-05-07 |

## Local patches

None.  If a local edit becomes necessary, add a row here describing
the patch and its rationale, and file an upstream PR.  Local patches
without an upstream proposal are technical debt.

## Internal header dependencies

The following n00b source files `#include "picoquic_internal.h"` and
depend on struct layouts / functions that picoquic does not expose
in its public `picoquic.h`.  Each is a re-vendoring risk: an upstream
refactor of these internals will force a corresponding update on
our side.

| File | What it uses | Why |
| ---- | ------------ | --- |
| `src/net/quic/picotls_sni.c` | picotls / picoquic context internals for SNI-keyed cert selection | Phase 2 SNI routing requires reaching past the public surface. |
| `src/net/quic/chan.c` | `picoquic_find_stream`, `picoquic_stream_head_t::{maxdata_remote, sent_offset, maxdata_local, consumed_offset, sack_list}`, `picoquic_sack_list_first` | Per-stream `send_window` / `recv_window` / `bytes_in_flight` for `n00b_quic_chan_stats`; picoquic has no public accessor for these today. |

If any of these break on re-vendor, the n00b QUIC test suite will
flag it before anything ships.  When that happens, prefer filing an
upstream PR to expose an accessor over carrying the internal-header
dependency indefinitely.

## Update procedure

The first vendor was performed via shallow clone.  For consistency
with how `mir/` and `ncc/` are likely to be re-imported, the
recommended ongoing mechanism is `git subtree`:

```bash
# From the repo root.
git subtree pull --prefix=subprojects/picotls \
    https://github.com/h2o/picotls.git master --squash

git subtree pull --prefix=subprojects/picoquic \
    https://github.com/private-octopus/picoquic.git master --squash
```

After the pull:

1. Update the **Commit** column above with the new resolved hash.
2. Update the **Imported** date.
3. Run `bash build.sh` and the QUIC test suite to confirm the new
   snapshot still compiles and passes.
4. If anything breaks, either fix forward in n00b's wrapper or pin
   to the previous commit and file an upstream issue.

## Build integration status

**Vendored libraries build cleanly under the n00b meson build.**  The
shims (`subprojects/picotls/meson.build` and
`subprojects/picoquic/meson.build`) are n00b-authored — upstream ships
CMake.  Both libs are produced as static archives:

- `build_debug/subprojects/picotls/libpicotls-core.a`
- `build_debug/subprojects/picotls/libpicotls-minicrypto.a`
- `build_debug/subprojects/picoquic/libpicoquic-core.a`

Backend selection — see `meson.build` headers in each subproject for
exact source-file inventories:

- **picotls**: minicrypto only (cifra primitives + uECC).  No OpenSSL,
  no Fusion, no mbedTLS, no certificate_compression (would need
  brotli).
- **picoquic**: built with `-DPTLS_WITHOUT_OPENSSL -DPTLS_WITHOUT_FUSION`
  and without defining `PICOQUIC_WITH_MBEDTLS`.  `picoquic_ptls_minicrypto.c`
  is the only backend wrapper compiled.

Variables exposed at the top-level meson scope:

- `picotls_minicrypto_dep` — depends on both picotls-core and picotls-minicrypto.
- `picoquic_dep` — depends on picoquic-core, picotls, and threads.

**Linking status into `libn00b`:** the deps are declared but **not yet
linked into `libn00b.a`**.  The QUIC module's picoquic-consuming code
(endpoint / conn / chan / qlog / trust / secret) lands in the next
phase of work; until then the vendored libs sit unused at link time.
The framer, types, stats, and UDP-conduit code that *is* in tree does
not touch picoquic and links cleanly without it.

## What we get from each library

- **picotls** — TLS 1.3 + the QUIC keyschedule + AEAD primitives
  (AES-GCM, ChaCha20-Poly1305).  We delegate the entire crypto +
  handshake path to it; we do not author new crypto code in n00b.
- **picoquic** — the QUIC state machine itself.  Pull-style API
  (`picoquic_prepare_next_packet` / `picoquic_incoming_packet`)
  driven from n00b's conduit IO loop.  Multiple congestion
  controllers (NewReno, Cubic, BBR) and the RFC 9221 datagram
  extension are supported in upstream.

## What we explicitly do not vendor

- **OpenSSL.**  picotls can use OpenSSL or its own minicrypto
  backend.  Phase 1 will pick the minicrypto backend by default to
  keep the dependency closure tight; users can opt into the OpenSSL
  backend at meson configure time when their deployment already
  links OpenSSL.
- **nghttp3.**  Will be vendored in Phase 4 alongside the H3 layer,
  not before.  Phase 1 has no use for it.
- **uacme / step-cli / mkcert.**  These are *test-time* tools used
  by the dev PKI script, not library dependencies.  They live on
  the developer's host, not in the repository.

## Test-time host tools (optional, not linked)

A handful of unit tests `popen()` host binaries to get an
independent second opinion on bytes we produce.  None of these are
build- or link-time dependencies of libn00b; if they're missing,
the relevant test sub-steps print a SKIP line and exit 0:

- `openssl` — used by `test/unit/test_quic_acme_csr.c` to run
  `openssl req -inform DER -verify -noout` against the CSR builder
  output.  Picks the first available of
  `/opt/homebrew/opt/openssl@3/bin/openssl`,
  `/opt/homebrew/opt/openssl/bin/openssl`, `/usr/bin/openssl`.
- `step-ca` + `step` — `test/fixtures/stepca/start.sh` spins up a
  per-test CA for the ACME integration test
  (`test/unit/test_quic_acme_session.c`).
- `xxd` — used by the same fixture script for a one-shot hex
  conversion when capturing the CA's leaf-cert SHA-256
  fingerprint.

**These are tools, not libraries.**  No runtime symbol resolution,
no header inclusion in shipping code, no linker line entry.  They
exist purely to keep us honest at test time.
