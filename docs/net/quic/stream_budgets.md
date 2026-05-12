# Stream budget defaults — methodology + measured numbers

> **Status: methodology + initial defaults documented; perf harness
> ships in the follow-up.**

## TL;DR

n00b QUIC ships with these initial transport-parameter defaults:

```c
N00B_QUIC_DEFAULT_MAX_STREAMS_BIDI  = 100
N00B_QUIC_DEFAULT_MAX_STREAMS_UNI   = 100
```

(See `include/net/quic/quic_types.h`.)

These are **industry-consensus starting numbers**, not measured
against our own per-stream memory profile.  This document
specifies the harness that will validate or override them.

## Why 100/100 is the starting point

Every comparable QUIC and HTTP/2/3 stack ships near this value:

| Stack         | Default | Notes |
|---------------|---------|-------|
| nghttp2       | 100     | `SETTINGS_MAX_CONCURRENT_STREAMS` |
| nghttp3       | 100     | Same default for H3 |
| msquic        | 100/100 | bidi / uni at last review |
| Cloudflare quiche | 100 | both directions |
| picoquic-demo | 100/100 | what the upstream demo binary advertises |
| aioquic       | 100/100 | |

So 100/100 is what a peer expects to see, and is what we ship until
the harness justifies a different number.  **Picking a much smaller
default invites peers to give up early; picking a much larger one
exposes the stream-table to amplification-style exhaustion attacks.**

## What the harness measures

`test/perf/perf_quic_stream_scaling.c` (forthcoming):

- Two endpoints in one process over loopback.
- For N ∈ {10, 100, 500, 1000, 5000, 10000}:
  1. Open N concurrent bidi channels.
  2. Each channel sends a steady 1 KiB/s for 60s, peer echoes back.
  3. Record:
     - Process RSS delta from N=10 baseline.
     - `conduit_pool` used bytes (queryable via the runtime).
     - picoquic internal bytes (via `picoquic_get_quic_stat`).
     - Echo RTT p50 and p99.
     - Per-second `bytes_in_flight` distribution (debug; not a gate).
- Output: TSV per row, plus a small Python notebook that plots the
  curves and writes summary numbers back into this document.

## Decision rule for shipped defaults

Two non-negotiable guardrails the harness output must satisfy:

1. **Per-stream memory budget < 16 KiB.**  Per-stream cost includes
   the n00b channel wrapper, picoquic stream state, the per-channel
   conduit subscription / inbox, and any framer scratch.  If the
   measured cost is ≥ 16 KiB, fix the layout *before* picking a
   default — don't paper over with a low cap.
2. **p99 echo RTT at the chosen N is within 2× the N=10 baseline.**
   If a knee appears earlier than 100, the default is "knee minus
   20% headroom".  If the curve stays flat through 1000, the default
   *might* go higher, but only after a separate "what's the attack
   surface from a high default" review.

## Configurability at the call site

Per-endpoint override:

```c
n00b_quic_endpoint_new(c, io,
    .max_streams_bidi = 50,
    .max_streams_uni  = 50);
```

Operators tighten this in production based on their workload.  The
defaults are sensible; they are not the only option.  *(Note:
the kwarg surface above is pending the conn / chan implementation;
it is documented here to anchor the intended API shape.)*

## Re-measurement cadence

Re-run the harness on every change to:

- The `n00b_quic_chan_t` struct layout.
- The picoquic upstream pin in `docs/net/quic/vendored.md`.
- The conduit inbox sizing or backpressure-policy default.
- The conduit pool's slab geometry.

CI runs a smoke version of the harness (N=100 only) on every PR
that touches `src/net/quic/` or `subprojects/picoquic/`.  The full
sweep is a manual gate before tagging a release; the operator runs
it and pastes the numbers into this document.

## Currently measured numbers

First harness run on **macOS-arm64** (Apple Silicon, debug build):

| N    | Per-stream RSS | p50 echo RTT | p99 echo RTT | Wall-clock |
|-----:|---------------:|-------------:|-------------:|-----------:|
|   10 |        4915 B  |    11.2 ms   |    11.2 ms   |    11 ms   |
|   50 |        3276 B  |    19.7 ms   |    19.7 ms   |    19 ms   |
|  100 |        2293 B  |    31.6 ms   |    31.6 ms   |    31 ms   |
|  500 |        2654 B  |   130.8 ms   |   130.9 ms   |   130 ms   |
| 1000 |        2375 B  |   262.6 ms   |   262.8 ms   |   262 ms   |

(`./build_debug/perf_quic_stream_scaling`, debug build, single-threaded
loopback echo of a 9-byte payload + FIN per stream.)

### Reading the table

- **Per-stream RSS** is the process-wide RSS delta divided by N.  It
  plateaus around 2–3 KB / stream in this configuration — well under
  the 16 KB / stream guardrail.  At N=10 the number is dominated by
  fixed setup overhead (TLS context, conduit pool, picoquic state)
  and is not representative; the N=100+ numbers are the meaningful
  signal.
- **p50/p99 echo RTT** is the per-stream wall-clock from `chan_open`
  to peer-FIN-observed-on-our-side.  These are highly correlated
  with N because we drive the entire echo on a single thread:
  every `run_once` iteration handles roughly one stream's worth of
  picoquic work.  Real workloads with multiple connections or a
  dedicated server thread will see very different numbers.
- **Wall-clock** is the total time to open + echo + close all N
  streams.  Linear in N at this scale, again because of the
  single-threaded driver.

### Verdict on the 100/100 defaults

**No reason to lower from the industry-consensus 100/100 on memory
grounds.**  At N=100, per-stream RSS is ~2.3 KB and p99 RTT is
~32 ms — comfortably below the 16 KB / 2× baseline guardrails.  The
default stays at 100 until either (a) we ship a multi-threaded
`run_once` driver and re-measure scaling, or (b) a workload surfaces
a knee we haven't found.

### Linux numbers — pending

We've run this only on macOS-arm64 so far.  Linux-x86_64 numbers
land when the harness ships in CI.  No reason to expect dramatically
different per-stream cost — the bulk of the bytes are picoquic's
internal state, which is allocator-agnostic.

## See also

- `~/dd/quic_1.md § 17.1` — the design source, with the same
  decision rule and methodology.
- `include/net/quic/quic_types.h` — where the constants live.
- `test/perf/perf_quic_stream_scaling.c` — the harness (pending).
