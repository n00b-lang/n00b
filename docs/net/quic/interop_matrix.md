# n00b QUIC Interop Matrix

Phase 5 § 5.14.  Manual interop matrix covering n00b QUIC + H3
against the major contemporary implementations.  Each cell
documents a reproducible test case + observed result.  Not a
CI gate — refreshed per-release with new entries appended.

## Implementations under test

| Peer        | Lang   | URL                                                  |
|-------------|--------|------------------------------------------------------|
| **n00b**    | C      | this repo                                            |
| ngtcp2      | C      | https://github.com/ngtcp2/ngtcp2                     |
| msquic      | C++    | https://github.com/microsoft/msquic                  |
| quiche      | Rust   | https://github.com/cloudflare/quiche                 |
| aioquic     | Python | https://github.com/aiortc/aioquic                    |

Existing CI smokes (`test/unit/test_quic_h3_caddy_smoke.c`,
`test_quic_h3_nginx_smoke.c`) are gated on
`N00B_TEST_DOCKER=1` and cover Caddy + nginx-quic.  Those
proxies use ngtcp2 (Caddy) and Cloudflare's quiche (nginx-quic)
internally, so they double as ngtcp2/quiche coverage at the
H3 layer.

## Coverage targets

For each (n00b, peer) pair we document at minimum:

- **Handshake** — TLS 1.3 + ALPN negotiation + 1-RTT key exchange
- **GET** — single-frame request/response, status 200
- **POST** — request body + response body
- **Server push** — n00b consumes a peer-initiated push (where
  applicable)
- **GOAWAY** — graceful drain initiated by either side
- **Resumption (no 0-RTT)** — session resumption keeps connections
  warm

## Test environment

All tests run inside `test/integration/phase5_interop/`
(planned — sub-phase 5.14.1).  Each peer is launched in its
own Docker container with its standard test config.  n00b
runs from `examples/quic_phase5_demo` with the relevant flag
adapter (`--peer ngtcp2`, etc.).

## Result template

Each entry follows:

```
### n00b (client) ↔ <peer> (server) — handshake

Repro:
  $ docker run --rm <peer-image> ...
  $ ./build_debug/quic_phase5_demo --client --target ...

Observed:
  - Handshake completes: <yes/no>
  - ALPN: <selected>
  - First-byte latency p50: <ms>

Status:
  ☐ interoperates
  ☐ known protocol gap (link to spec section)
  ☐ our bug (link to issue)
  ☐ their bug (link to upstream issue)
  ☐ environment-only (e.g., docker network NAT)

Notes:
  Free text.
```

## Matrix (initial set — pending fixture in 5.14.1)

| Test                              | ngtcp2 | msquic | quiche | aioquic |
|-----------------------------------|--------|--------|--------|---------|
| n00b client → peer server: handshake | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| n00b client → peer server: GET    | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| n00b client → peer server: POST   | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| peer client → n00b server: handshake | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| peer client → n00b server: GET    | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| peer client → n00b server: POST   | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| GOAWAY (n00b ← peer)              | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| GOAWAY (peer ← n00b)              | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| Session resumption (no 0-RTT)     | ⏳ pending | ⏳ pending | ⏳ pending | ⏳ pending |
| Server push                       | n/a (Phase 4 not pushing) | n/a | n/a | n/a |

Legend:
  - ✅ — interoperates
  - ⚠️  — partial (with notes)
  - ❌ — does not interoperate (with link to root cause)
  - 🛠 — gap; tracked
  - ⏳ — pending (test not yet run)

Existing data (from Phase 4 CI):
- **Caddy (ngtcp2-based) ↔ n00b client**: GET succeeds in
  `test/unit/test_quic_h3_caddy_smoke.c`.
- **nginx-quic (quiche-based) ↔ n00b client**: GET succeeds
  in `test/unit/test_quic_h3_nginx_smoke.c`.
- **picoquicdemo ↔ n00b**: handshake-only manual run during
  Phase 1 acceptance (`docs/net/quic/quic.md` § Interop).

## Sub-phase 5.14.1 — fixture build-out

Planned but not in 5.14:

- `test/integration/phase5_interop/Dockerfile.<peer>` per peer
- `run.sh` orchestrating the matrix
- Per-peer assertion scripts
- Markdown auto-generation from the test results (the table
  above gets regenerated per-run)

The fixture build-out is a follow-up sub-phase since each
peer image's quirks (cert formats, ALPN negotiation, log
formats) require careful per-peer adapters.

## Reporting bugs

When a cell goes from ✅ to ❌:
1. Capture qlog from both sides.
2. Match against the relevant RFC clause.
3. File on the side that's non-conformant; cross-link the
   row in this matrix.
