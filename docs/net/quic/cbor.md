# QUIC CBOR (Phase 4 § 4.1)

CBOR (RFC 8949) is the on-the-wire payload codec for n00b's H3-based
RPC.  Every request and response body is a single CBOR item — most
often a map representing the user's request/response struct.  The
codec lives at `include/net/quic/cbor.h` + `src/net/quic/cbor_encoder.c` +
`src/net/quic/cbor_decoder.c`.

## Why CBOR

| Constraint | CBOR fit |
|---|---|
| Wire-stable across the n00b versions a service must speak | Schema-free, self-describing. |
| Cheap to encode + decode in C | Tagged single-byte head + raw payload; no string-keyed lookups during parse. |
| Integer + binary data + Unicode all native | Major types 0/1/2/3 plus tagged extensions. |
| Hardenable against malicious input | Bounded recursion + length caps; refuse indefinite-length items. |

JSON would force every binary field through base64; protobuf would
force a separate `.proto` build step into the user's project (and
would force gRPC compatibility decisions we explicitly defer — see
`quic_4.md` § 2 "Out of scope").

## Type mapping (Phase 4 plan § 6)

| n00b type            | CBOR major type / shape                    |
|----------------------|--------------------------------------------|
| `int64_t`            | unsigned (0) or negative (1) integer        |
| `bool`               | simple value (true=21, false=20)           |
| `double`             | float (7, ai=27)                           |
| `n00b_string_t *`    | text string (3); UTF-8 enforced            |
| `n00b_buffer_t *`    | byte string (2)                            |
| `n00b_list_t(T) *`   | array (4) of T                             |
| `n00b_dict_t<K,V> *` | map (5) of K → V                           |
| `n00b_option_t(T)`   | nullable: `null` (7,22) or T                |
| `n00b_result_t(T)`   | tagged: tag 27 ok | tag 28 err              |
| `n00b_bigint_t *`    | tagged bignum (tag 2 / 3)                   |
| `n00b_time_t`        | tagged date-time (tag 0 RFC3339 / tag 1 epoch) |

The bignum / time bindings emit and parse the tag wrappers but do not
ship rich n00b-side types in v1 — n00b's bigint and time wrapper
haven't landed yet.  Phase 4.5 (`@rpc` annotation) wires the
annotation-driven dispatcher around the typed extractors below.

## Canonical encoding

The encoder follows RFC 8949 § 4.2.1 ("Core Deterministic Encoding"):

  - Integers use the smallest length that fits.
  - Floats are emitted as 64-bit (binary64).  Down-conversion to
    binary16/binary32 is a follow-up if profiling shows the wire
    overhead matters; the v1 wire is byte-stable across platforms,
    which matters more for content-addressing.
  - Arrays + maps are definite-length only.

## Decoder hardening

Two hard limits enforced before any allocation:

| Limit                        | Default     | Override                |
|------------------------------|-------------|-------------------------|
| `N00B_CBOR_MAX_DEPTH`        | 32          | `-DN00B_CBOR_MAX_DEPTH=N` |
| `N00B_CBOR_MAX_INPUT_BYTES`  | 16 MiB      | `-DN00B_CBOR_MAX_INPUT_BYTES=N` |

Indefinite-length items (RFC 8949 § 3.2) are refused outright.  RPC
bodies don't need them; they multiply the fuzz attack surface; the
break-stop machinery is a known source of nasty corner cases.

UTF-8 is structurally validated at decode time — a text-string
header followed by ill-formed bytes produces
`N00B_QUIC_ERR_PROTOCOL`.

Decoder errors map to existing QUIC error codes:

| Code                              | When                                         |
|-----------------------------------|----------------------------------------------|
| `N00B_QUIC_ERR_NULL_ARG`          | `nullptr` input                              |
| `N00B_QUIC_ERR_NEED_MORE_DATA`    | Truncated mid-head or mid-string             |
| `N00B_QUIC_ERR_FRAME_TOO_LARGE`   | Input > `N00B_CBOR_MAX_INPUT_BYTES`          |
| `N00B_QUIC_ERR_PROTOCOL`          | Malformed bytes, depth-cap, indefinite, etc. |
| `N00B_QUIC_ERR_BAD_TYPE`          | Extractor invoked on the wrong AST kind      |

## Memory discipline

Every CBOR-owned allocation routes through `conduit_pool` per the
project's allocator audit.  The decoder's AST is allocated from the
same pool: the entire tree dies with the pool, so callers don't
free node-by-node.  Encoder buffers are likewise pool-allocated so
RPC senders can hand them straight to the conduit without copying.

## Encoder usage

### Single-value convenience

```c
#include "net/quic/cbor.h"

n00b_buffer_t *body = n00b_cbor_encode((int64_t)42);
n00b_buffer_t *flag = n00b_cbor_encode(true);
n00b_buffer_t *name = n00b_cbor_encode(n00b_string_from_cstr("alice"));
```

The `n00b_cbor_encode()` macro dispatches via `_Generic`; supported
types are `int64_t`, `bool`, `double`, `n00b_string_t *`, and
`n00b_buffer_t *`.

### Streaming a compound document

For maps, arrays, and nested shapes, build the buffer directly:

```c
n00b_buffer_t *out = n00b_buffer_empty();

n00b_cbor_write_map_header(out, 3);
n00b_cbor_write_string(out, n00b_string_from_cstr("name"));
n00b_cbor_write_string(out, n00b_string_from_cstr("alice"));
n00b_cbor_write_string(out, n00b_string_from_cstr("age"));
n00b_cbor_write_int(out, 30);
n00b_cbor_write_string(out, n00b_string_from_cstr("scopes"));
n00b_cbor_write_array_header(out, 2);
n00b_cbor_write_string(out, n00b_string_from_cstr("read"));
n00b_cbor_write_string(out, n00b_string_from_cstr("write"));
```

The caller is responsible for emitting exactly the number of items
declared in the header; there is no runtime check.  Static-analysis
tools and the future `@rpc` annotation transform make this trivial
for generated code.

## Decoder usage

### One-shot decode-to-T

```c
auto r = n00b_cbor_decode_to(int64_t, body);
if (n00b_result_is_ok(r)) {
    int64_t v = n00b_result_get(r);
    /* ... */
}
```

Supported `T`: `int64_t`, `bool`, `double`, `n00b_string_t *`,
`n00b_buffer_t *`.

### AST inspection

For compound shapes — or whenever the caller hasn't bound the
schema to a static type yet — decode to the AST:

```c
auto r = n00b_cbor_decode(body);
if (n00b_result_is_err(r)) { /* handle */ }

n00b_cbor_value_t *root = n00b_result_get(r);
if (root->kind != N00B_CBOR_VT_MAP) { /* shape error */ }

for (size_t i = 0; i < root->u.map.count; i++) {
    n00b_cbor_pair_t pair = root->u.map.pairs[i];
    if (pair.key->kind != N00B_CBOR_VT_STRING) { /* shape error */ }

    if (memcmp(pair.key->u.string->data, "name", 4) == 0) {
        auto sr = n00b_cbor_value_to_string(pair.val);
        /* ... */
    }
}
```

### Tags

The decoder surfaces tags with the inner value already parsed:

```c
auto r = n00b_cbor_decode(body);
n00b_cbor_value_t *v = n00b_result_get(r);
if (v->kind == N00B_CBOR_VT_TAG) {
    switch (v->u.tag.tag) {
    case N00B_CBOR_TAG_RESULT_OK:
        /* unwrap v->u.tag.inner as the ok arm */
        break;
    case N00B_CBOR_TAG_RESULT_ERR:
        /* unwrap as err */
        break;
    case N00B_CBOR_TAG_DATETIME_RFC3339:
        /* v->u.tag.inner is a text string */
        break;
    }
}
```

## Testing

### Unit tests — `test_quic_cbor`

Run with:

```sh
N00B_TEST=1 bash build.sh
meson test -C build_debug --print-errorlogs --suite unit quic_cbor
```

Coverage:

  - RFC 8949 Appendix A vectors (encode + decode side-by-side).
  - n00b round-trips for every supported primitive (boundary
    values for int64, ±inf + NaN for double, multi-byte UTF-8 for
    string).
  - Heterogeneous + nested container round-trips.
  - Tag round-trip + the RFC 8949 datetime vector.
  - The `n00b_cbor_decode_to(T, buf)` macro for every supported T.
  - Decoder hardening: truncated input, indefinite-length refusal,
    trailing bytes, length-cap, depth-cap, lying-count maps, bad
    UTF-8, reserved AI nibbles, type-mismatch on extractors.
  - Light in-process fuzz pass (20k random short inputs).

### libFuzzer harness — `test/fuzz/fuzz_quic_cbor.c`

Build with Clang:

```sh
clang -O1 -g -fsanitize=fuzzer,address \
      -I include -I include/internal \
      test/fuzz/fuzz_quic_cbor.c \
      -L build_debug -ln00b -lpicoquic-core -lpicotls-core ... \
      -o build_debug/fuzz_quic_cbor
build_debug/fuzz_quic_cbor corpus/ -max_total_time=300
```

Suggested seed corpus: bytes extracted from the unit-test hex
vectors plus the cbor-test-vectors github project (RFC 8949 expanded
set).  The harness asserts the documented decoder contract — any
crash, ASan/UBSan flag, or undocumented error code is a hard fail.

## Phase 4 hand-off

Phase 4.5 (`@rpc` ncc annotation) generates marshaling stubs that
call into this module.  The generated dispatcher decodes the request
body to the static request type via the typed-extractor surface,
calls the user function, and re-encodes the reply via the streaming
writer surface.  The CBOR module makes no assumptions about its
caller's threading: every call is reentrant, and the conduit pool
is the only shared state.
