# QPACK module — overview

`include/net/quic/qpack.h`, `src/net/quic/qpack_*.c`.  Implements RFC 9204
(QPACK) — HTTP/3's header compression scheme.  Phase 4 § 4.2.

QPACK is the H3 analog of HPACK (RFC 7541): it compresses HTTP
headers using a static table of common entries plus a per-connection
dynamic table that the encoder builds up over the connection's
lifetime and the decoder mirrors.  The wire format is designed for
QUIC's stream-multiplexed, out-of-order delivery: instead of
inlining dynamic-table updates with field sections (HPACK), QPACK
uses two dedicated unidirectional streams (encoder stream, decoder
stream) so each side can update / acknowledge state in parallel
with the actual request/response data on bidi streams.

## What ships in v1

- **Static table** — full 99 entries from RFC 9204 Appendix A.
- **Huffman codec** — RFC 7541 Appendix B (256 symbols + EOS).
  Encoder + decoder; padding rules per § 5.2 enforced.
- **Dynamic table** — insertion + FIFO eviction; mutex-protected;
  per-encoder + per-decoder.
- **Encoder stream protocol** — Insert with Name Reference, Insert
  with Literal Name, Set Dynamic Table Capacity, Duplicate.
- **Decoder stream protocol** — Section Acknowledgment, Insert
  Count Increment, Stream Cancellation.
- **Field-line encodings** — Indexed (static + dynamic), Literal
  with Name Reference (static + dynamic, including the
  never-indexed "N" bit), Literal Without Name Reference,
  Indexed Post-Base, Literal Post-Base.

The encoder + decoder are independently testable with no real H3
stack: feed bytes in, get bytes (or fields) out.

## Public API surface

```c
n00b_qpack_encoder_t *n00b_qpack_encoder_new(uint64_t cap, uint64_t blocked);
n00b_qpack_decoder_t *n00b_qpack_decoder_new(uint64_t cap, uint64_t blocked);
void n00b_qpack_encoder_close(...);
void n00b_qpack_decoder_close(...);

n00b_result_t(bool) n00b_qpack_encode(enc, stream_id, fields, n,
                                       out_section, out_encoder_stream);
n00b_result_t(bool) n00b_qpack_decode(dec, stream_id, bytes, len,
                                       out_fields, cap, &n_out,
                                       out_decoder_stream);

/* Encoder/decoder stream "feed me bytes" entrypoints. */
n00b_result_t(size_t)
    n00b_qpack_encoder_consume_decoder_stream(enc, bytes, len);
n00b_result_t(size_t)
    n00b_qpack_decoder_consume_encoder_stream(dec, bytes, len,
                                               out_decoder_stream);

/* Capacity negotiation (called by the H3 layer post-SETTINGS). */
n00b_result_t(bool)
    n00b_qpack_encoder_set_capacity(enc, capacity, out_encoder_stream);

/* Huffman exposed for direct use (and unit-test access). */
n00b_result_t(size_t) n00b_qpack_huffman_encode(...);
n00b_result_t(size_t) n00b_qpack_huffman_decode(...);
```

`n00b_qpack_field_t` is `(name, name_len, value, value_len)`.  All
four fields are read-only views; the QPACK module copies into the
dynamic table on insertion.

## Architecture

```
       application                          peer (over QUIC)
        ┌────────┐                          ┌────────┐
   Hdrs │encoder │ ── field section ────────▶│decoder │ Hdrs
        │        │                          │        │
        │        │ ── encoder stream  ──────▶│        │
        │        │                          │        │
        │        │◀────── decoder stream ───│        │
        └────────┘                          └────────┘
            ▲                                    ▲
            │                                    │
        consume                               consume
        decoder-stream                        encoder-stream

  encoder.dyn  ── mirror via encoder stream ──▶  decoder.dyn
```

Each side keeps its own `n00b_qpack_dyn_table_t` (mutex-protected
ring buffer of `n00b_qpack_dyn_entry_t`).  The encoder mutates its
table by emitting Insert / Set-Capacity instructions on the encoder
stream; the decoder applies them when it consumes those bytes.  The
two tables stay coherent as long as encoder-stream bytes are
delivered in order — which they are, since QUIC streams are
in-order within a stream.

The decoder confirms forward progress via the decoder stream:
Section Acknowledgment per stream (after a field section that
referenced dynamic entries was successfully decoded), Insert Count
Increment (batched count of new dynamic-table entries the decoder
has processed), and Stream Cancellation (drop pending acks for a
cancelled stream).  The encoder uses these to compute its
`known_received_count` — the largest insertion id the peer has
acknowledged — which gates whether new field sections may
reference unacked entries (per `max_blocked_streams`).

## v1 simplifications + known limitations

Documented up front so the H3 stack (sub-phase 4.3) and any
operational team knows where to look:

1. **Required-Insert-Count encoding.**  The encoder always uses
   the simple invariant `encoded_RIC = RIC + 1` (or 0 when no
   dynamic refs).  The decoder uses `RIC = encoded_RIC - 1` for
   `encoded_RIC > 0`.  RFC 9204 § 4.5.1.1 specifies a richer
   modulo-`2 * MaxEntries` encoding to handle wrapping; our
   implementation is correct for `RIC < 2 * MaxEntries` (which
   holds for any reasonable dynamic-table capacity).  Full
   wrapping support is a v1.1 follow-up.

2. **Base = Required.**  The encoder always emits `Delta Base = 0,
   S = 0`, i.e., base equals required.  This means dynamic-table
   references in the body use relative-to-base indexing.
   Post-base indexing is supported on the decoder side (we accept
   it from peers) but the encoder never emits it.

3. **Encoder caching heuristic.**  The encoder caches everything
   except a small denylist of known high-cardinality headers
   (`cookie`, `set-cookie`, `authorization`, `traceparent`,
   `tracestate`, `if-none-match`, `if-modified-since`).  Tunable
   in `should_cache()` at `qpack_encoder.c`.

4. **Blocked streams.**  We don't track blocked streams beyond a
   coarse "if `max_blocked_streams == 0` then never emit
   blocking references" gate.  The encoder's `known_received_count`
   is updated from decoder-stream acks but isn't used to limit
   in-flight insertions — that lives in the H3 stack's stream
   scheduler.

5. **Pending-section list is unbounded.**  Each `n00b_qpack_encode`
   call that references a dynamic entry pushes onto a list;
   Section Ack pops one entry.  In practice the list stays small
   (one entry per in-flight request).  A misbehaving peer that
   never acks could grow it; the H3 stack's max-streams limit
   bounds the worst case.

## DoS hardening

Hard caps in `qpack.h`:

- `N00B_QPACK_MAX_FIELD_LINE` — 64 KiB per name/value.
- `N00B_QPACK_MAX_FIELDS_PER_SECTION` — 1024 fields.
- `N00B_QPACK_MAX_DYNAMIC_CAPACITY` — 1 MiB hard ceiling on the
  advertised dynamic-table size.

Decoder rejects:
- Out-of-range static-table indices (`PROTOCOL`).
- Forward-references to unreceived dynamic entries (`PROTOCOL` /
  `NEED_MORE_DATA`).
- Truncated prefix-int encodings (`PROTOCOL`).
- Huffman padding > 7 bits or embedded EOS (`PROTOCOL`).

The libFuzzer harness at `test/fuzz/fuzz_quic_qpack.c` drives
random bytes through both `consume_encoder_stream` and
`decode`; build with Clang `-fsanitize=fuzzer,address`.

## File layout

| File                                        | Role                              |
|---------------------------------------------|-----------------------------------|
| `include/net/quic/qpack.h`                      | Public API + hard caps             |
| `include/internal/net/quic/qpack_internal.h`    | Static + Huffman table types,      |
|                                             | dyn-table struct, prefix-int decl  |
| `src/net/quic/qpack_static.c`                   | RFC 9204 Appendix A static table   |
| `src/net/quic/qpack_huffman.c`                  | RFC 7541 Appendix B Huffman codec  |
| `src/net/quic/qpack_encoder.c`                  | Encoder + shared support code      |
| `src/net/quic/qpack_decoder.c`                  | Decoder + encoder-stream consumer  |
| `test/unit/test_quic_qpack.c`               | RFC + dyn-table stress             |
| `test/fuzz/fuzz_quic_qpack.c`               | libFuzzer harness                  |

## Allocator discipline

Every allocation goes through `n00b_qpack_alloc()`, which returns
the runtime's `conduit_pool` allocator.  Decoded field bytes are
dup'd into the conduit pool on each `n00b_qpack_decode` call;
they remain valid through the decoder handle's lifetime (the
conduit pool collects them on shutdown).  Dynamic-table entry
bytes are dup'd into the conduit pool on each insertion; eviction
removes the entry from the ring but the bytes themselves survive
(harmless — the conduit pool compacts on its own schedule).

## Tests

`build_debug/test_quic_qpack` runs to "All quic_qpack tests
passed. (17 sub-tests)".  Coverage:

- Static-table spot checks (entries 0, 17, 25, 98).
- Huffman encoder + decoder round-trip across all 256 bytes.
- Huffman against the RFC 7541 Appendix C.4.1 vector for
  `"www.example.com"`.
- Huffman padding hard-rejects (oversized + non-1s padding).
- Encode + decode static-only, custom literal, static name-ref.
- Encoder/decoder mirror via the encoder stream.
- Section acknowledgment / Insert Count Increment / Stream
  Cancellation routing through the decoder stream.
- 1000-message dynamic-table stress (insert, evict, mirror
  invariant: encoder.insert_count == decoder.insert_count
  after every iteration).
- Decoder rejects out-of-range static-table indices.
- Decoder errors on forward-referenced dynamic entries.
- Realistic 6-field request round-trip.
- Encoder refuses oversized field-line (DoS guard).
