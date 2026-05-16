# libchalk — Integration Guide

A C library for adding and removing **chalk marks** on software
artifacts: ELF / Mach-O / PE binaries, shell wrappers, ZIP archives,
Python `.pyc` files, ML weights (GGUF / SafeTensors), source files,
certs, plus arbitrary files via sidecars. The mark format is
compatible with [chalk](https://github.com/crashappsec/chalk) v2.0.x.

Status: chalk-module branch. Public ABI is not yet stable — use the
versioned tarball headers, don't pin against in-tree paths.

---

## Table of contents

1. [What's in the box](#whats-in-the-box)
2. [Linking and headers](#linking-and-headers)
3. [The mark](#the-mark)
4. [Insert / delete / extract / hash](#insert--delete--extract--hash)
5. [Codec selection](#codec-selection)
6. [Memory contracts](#memory-contracts)
7. [Re-signing after a chalk insert](#re-signing-after-a-chalk-insert)
8. [Error handling](#error-handling)
9. [Sidecars](#sidecars)
10. [Examples](#examples)

---

## What's in the box

**Codecs** (one per artifact format):

| Codec                | Where the mark lives                      |
|----------------------|-------------------------------------------|
| ELF                  | New `.chalk.mark` section                 |
| Mach-O               | New `LC_NOTE` load command, owner="chalk" |
| PE                   | New `.chalk` section                      |
| macOS shell wrapper  | Magic-prefixed shell snippet              |
| ZIP                  | Comment of one entry                      |
| `.pyc`               | Trailing JSON after the bytecode          |
| GGUF                 | Custom string KV at the end               |
| SafeTensors          | Custom string KV at the end               |
| Source code          | Trailing line comment                     |
| Cert (PEM)           | Sibling `<file>.chalk` sidecar            |
| Model weights        | Sibling `<file>.chalk` sidecar            |

**Dispatcher** — auto-detects codec by magic bytes / path extension
and forwards. You can also call per-codec entry points directly if
you already know the format.

---

## Linking and headers

The library is built by n00b's meson layout. To use it from another
project link against `libn00b_chalk` and include the umbrella:

```c
#include <chalk/n00b_chalk.h>
```

Or pull in only what you need:

```c
#include <chalk/n00b_chalk_codec.h>   // dispatcher + result types
#include <chalk/n00b_chalk_mark.h>    // mark construction
#include <chalk/n00b_chalk_macho.h>   // Mach-O-specific entry points
#include <chalk/n00b_chalk_pe.h>      // PE
// ... etc
```

n00b runtime must be initialized before any libchalk call:

```c
int main(int argc, char **argv) {
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    // ... libchalk calls here ...
    n00b_shutdown();
}
```

---

## The mark

libchalk emits a fixed six-key mark, mark v2.0:

| Key                           | Source                                       |
|-------------------------------|----------------------------------------------|
| `MAGIC`                       | Constant `"dadfedabbadabbed"`                |
| `CHALK_VERSION`               | `"2.0.0"`                                    |
| `CHALK_ID`                    | base32v of `HASH` (first 100 bits)           |
| `METADATA_ID`                 | base32v of normalize-and-hash of the mark    |
| `HASH`                        | SHA-256 of the unchalked artifact (hex)      |
| `TIMESTAMP_WHEN_CHALKED`      | Milliseconds since epoch                     |
| `ATTESTATION` (optional)      | Caller-supplied JSON, passed through         |

You build a mark with `n00b_chalk_mark_new()`, optionally attach an
attestation with `n00b_chalk_mark_set_attestation(mark, json)`, and
hand it to an insert function. The codec computes `HASH` over the
unchalked-form bytes, then `n00b_chalk_mark_finalize()` derives the
remaining fields and returns canonical JSON bytes ready for embedding.

You don't typically call `finalize` yourself — the codec calls it.
But if you're building a custom codec, the contract is:

```c
auto fin = n00b_chalk_mark_finalize(mark, unchalked_sha256_buf /* 32 bytes */);
n00b_buffer_t *json_bytes = n00b_result_get(fin);
```

---

## Insert / delete / extract / hash

Every codec exposes four operations in two flavors (buffer-mode and
file-mode):

```c
// Buffer mode — caller owns the bytes.
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_insert_buffer(n00b_buffer_t *bytes, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_delete_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_extract_buffer(n00b_buffer_t *bytes);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_hash_buffer(n00b_buffer_t *bytes);

// File mode — libchalk reads/writes the path.
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_insert_file(n00b_string_t *path, n00b_chalk_mark_t *mark);
n00b_result_t(n00b_chalk_io_result_t *)
    n00b_chalk_delete_file(n00b_string_t *path);
n00b_result_t(n00b_chalk_extract_result_t *)
    n00b_chalk_extract_file(n00b_string_t *path);
n00b_result_t(n00b_buffer_t *)
    n00b_chalk_hash_file(n00b_string_t *path);
```

`n00b_chalk_io_result_t` carries the rewritten artifact bytes (for
in-band codecs) or sidecar bytes (for cert / model_sidecar). The
`kind` field tells you which it is.

`n00b_chalk_extract_result_t` carries the parsed mark as an ordered
dict keyed by string → JSON node, plus the codec id and the source
discriminator (in-band vs sidecar).

`hash` returns the **unchalked** SHA-256 of the artifact — the same
value that would go into `HASH` if you were to mark it. Useful for
caching, deduplication, or pre-flight checks.

---

## Codec selection

`n00b_chalk_insert_file` and friends auto-detect the codec:

1. Try magic bytes (ELF, Mach-O, PE, GGUF, SafeTensors, ZIP, pyc).
2. Try the macOS wrapper sentinel.
3. Try file extension (`.exe`, `.dll`, `.sys` → PE; `.py` → source;
   etc.).
4. Fall back to the model sidecar if nothing else matches.

If you already know the codec — e.g. you're handling each artifact
type with codec-specific logic — call the codec's entry point
directly:

```c
auto r = n00b_chalk_macho_insert_file(path, mark);
```

This skips the detect step.

---

## Memory contracts

libchalk's I/O is built on n00b's `core/file.h` façade. The contracts:

- **`n00b_chalk_hash_file`** — streaming SHA-256 via the `STREAM`
  substrate. Memory floor is ~64 KiB regardless of file size. Use
  this when you just need the unchalked hash.
- **`n00b_chalk_*_extract_file`** — reads via the `MMAP` substrate
  for binary codecs. The artifact is page-mapped read-only; resident
  set scales with the parser's working window, not file size.
- **`n00b_chalk_*_insert_file` / `_delete_file`** — read via MMAP,
  parser builds output buffers in RAM. Peak resident set ≈ input
  size + output size. For ELF / PE this means **2×N** with N = file
  size. For Mach-O the in-place mutation path keeps it close to
  **1×N**. For sidecar codecs only the artifact is mapped (sidecar
  is written separately).

These contracts are documented per codec in the per-codec header
comments; consult those for precise per-codec behavior.

---

## Re-signing after a chalk insert

Adding a chalk mark **invalidates code signatures** on platforms
that protect the binary's contents:

| Codec   | Signature concerns                                            |
|---------|---------------------------------------------------------------|
| Mach-O  | `LC_CODE_SIGNATURE` (codesign) is invalidated. Re-sign after. |
| PE      | Authenticode signature is invalidated. Re-sign after.         |
| ELF     | No standard signature; downstream IMA/EVM may be affected.    |
| Others  | No standard signature.                                        |

libchalk provides:

- **`n00b_chalk_macho_signature_kind(bytes)`** — returns
  `MACHO_SIG_NONE` / `_ADHOC` / `_CERT` / `_MALFORMED`. Use this to
  decide whether re-signing is needed.
- **`n00b_chalk_macho_strip_signature(bytes)`** — returns artifact
  bytes with `LC_CODE_SIGNATURE` removed and `__LINKEDIT` resized.
  Call this **before** insert if you want to avoid the codesign-vs-
  chalk-mark conflict; then `codesign --sign ...` after insert.
- **`n00b_chalk_pe_signature_kind(bytes)`** — returns `PE_SIG_NONE`
  or `_AUTHENTICODE`. Strip the existing certificate table entry
  before re-signing with `signtool`.

The actual re-signing is the caller's responsibility — libchalk does
not invoke `codesign` / `signtool` / IMA-evmctl. Typical flow:

```c
// 1. Decide if the artifact is signed.
auto kind = n00b_chalk_macho_signature_kind(bytes);
if (kind == N00B_CHALK_MACHO_SIG_CERT) {
    // 2. Strip — produces unsigned bytes.
    auto r = n00b_chalk_macho_strip_signature(bytes);
    bytes  = n00b_result_get(r);
}
// 3. Insert the chalk mark.
auto ir = n00b_chalk_macho_insert_buffer(bytes, mark);
// 4. Write to disk and codesign --sign ... (out-of-process).
```

---

## Error handling

Every libchalk function returns `n00b_result_t(T)`. Check with
`n00b_result_is_err(r)` and unpack with `n00b_result_get(r)` or
`n00b_result_get_err(r)`. There are no `errno`-style globals.

The error codes are small integers (positive). They identify the
*step* that failed rather than a unix `errno` (the codecs have many
internal stages); treat any non-zero as "operation failed", and
report the integer for debugging. For `_file` variants, the
underlying file open/read/write errors propagate as `errno` values
(positive).

`n00b_chalk_detect_*` returns `N00B_CHALK_CODEC_NONE` when no codec
matches. This is a probe, not an error.

---

## Sidecars

Two codecs always emit sidecar files:

- `N00B_CHALK_CODEC_SIDECAR_MODEL` — for ML weights and other
  artifacts where in-band marking would corrupt the format.
- `N00B_CHALK_CODEC_SIDECAR_CERT` — for X.509 certs.

The sidecar lives next to the artifact with a `.chalk` suffix.
The dispatcher writes it automatically in `_insert_file`. For
`_insert_buffer`, the caller is responsible for writing the
returned bytes to `<artifact-path>.chalk` (the codec sets
`io_result.sidecar_suffix` to `.chalk`).

To extract a sidecar mark directly:

```c
auto r = n00b_chalk_extract_sidecar_buffer(sidecar_bytes);
```

---

## Examples

### Mark a file by auto-detect

```c
n00b_runtime_t rt;
n00b_init(&rt, argc, argv);

n00b_string_t     *path = n00b_string_from_cstr("/usr/local/bin/myapp");
n00b_chalk_mark_t *mark = n00b_chalk_mark_new();

auto r = n00b_chalk_insert_file(path, mark);
if (n00b_result_is_err(r)) {
    fprintf(stderr, "insert failed: err=%d\n", n00b_result_get_err(r));
    return 1;
}
n00b_chalk_io_result_t *io = n00b_result_get(r);
// io->kind == N00B_CHALK_OUT_IN_BAND for binaries; the dispatcher
// has already rewritten the file. For sidecar codecs the dispatcher
// has written the sidecar.
```

### Get just the unchalked hash

```c
auto hr = n00b_chalk_hash_file(path);
n00b_buffer_t *sha256_32 = n00b_result_get(hr);
// 32-byte raw SHA-256
```

### Extract and inspect the mark

```c
auto er = n00b_chalk_extract_file(path);
if (n00b_result_is_err(er)) return 0;  // no mark
n00b_chalk_extract_result_t *ex = n00b_result_get(er);
// ex->mark is a dict<string, json_node>. Iterate or look up by key.
```

### Sign-then-chalk-then-re-sign on macOS

```c
auto bytes_r = n00b_file_open(path, .kind = N00B_FILE_KIND_MMAP);
n00b_buffer_t *bytes = n00b_file_as_buffer_get(n00b_result_get(bytes_r));

if (n00b_chalk_macho_signature_kind(bytes) == N00B_CHALK_MACHO_SIG_CERT) {
    auto sr = n00b_chalk_macho_strip_signature(bytes);
    bytes   = n00b_result_get(sr);
}
auto ir = n00b_chalk_macho_insert_buffer(bytes, mark);
n00b_buffer_t *out = n00b_result_get(ir)->bytes;
// Write `out` to a temp path, then invoke `codesign --sign ...`.
```
