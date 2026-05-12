/**
 * @file http_compression.h
 * @internal
 * @brief Content-Encoding decompression + Accept-Encoding hint
 *        derivation for the n00b HTTP client (Phase 6 chunk 8).
 *
 * What ships:
 *   - **gzip** (Content-Encoding: gzip)         — always available;
 *     statically linked against libz.
 *   - **deflate** (Content-Encoding: deflate)   — always available
 *     via libz.
 *   - **brotli** (Content-Encoding: br)         — opportunistic;
 *     `dlopen("libbrotlidec.so", "libbrotlidec.dylib")` at first use.
 *   - **zstd** (Content-Encoding: zstd)         — opportunistic;
 *     `dlopen("libzstd.so.1", "libzstd.dylib")` at first use.
 *
 * Capability detection at first call narrows
 * `n00b_http_accept_encoding_header()` accordingly so we never
 * advertise an encoding we can't decode.
 *
 * Request-body compression (the dispatcher's `body_encoding` kwarg)
 * land in chunk 12's polish sweep — chunk 8 covers the
 * decompression path that operators hit by default.
 *
 * @see ~/dd/quic_6.md § 7 chunk 8.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"

/**
 * @brief Decompress @p src per the wire encoding @p encoding.
 *
 * @param src       Compressed bytes (response body).
 * @param encoding  ASCII-case-insensitive `Content-Encoding` value
 *                  (`"gzip"`, `"deflate"`, `"br"`, `"zstd"`).
 *                  `"identity"` and the empty / `nullptr` value are
 *                  pass-throughs (allocates a copy of @p src).
 *
 * @kw allocator  Default per-runtime conduit pool.
 * @kw max_size   Hard cap on the decompressed size (defends against
 *                zip-bombs).  Default 64 MiB.
 *
 * @return  Result with a freshly allocated decompressed buffer on
 *          success; err on:
 *          - unsupported encoding (the codec wasn't available at
 *            runtime — caller should advertise narrower
 *            `Accept-Encoding` to avoid hitting this)
 *          - corrupt or truncated input
 *          - decompressed size exceeded @p max_size
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_http_decompress(n00b_buffer_t *src, const char *encoding)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        size_t            max_size  = 64 * 1024 * 1024;
    };

/**
 * @brief Returns the `Accept-Encoding` header value to advertise.
 *
 * The value reflects what's actually decodable at runtime, i.e.
 * always includes `gzip, deflate` and includes `br` / `zstd` only
 * when their libraries dlopen successfully.
 *
 * Heap-allocated; caller borrows.  The header is cached after first
 * call (capability probe runs once).
 *
 * @kw allocator  Default per-runtime conduit pool.
 */
extern n00b_string_t *
n00b_http_accept_encoding_header()
    _kargs {
        n00b_allocator_t *allocator = nullptr;
    };

/**
 * @brief Test hook: re-run the capability probe.  Tests use this
 *        when injecting a mocked dlopen to verify the header
 *        narrows.
 */
extern void n00b_http_compression_reset_for_test(void);

/** @brief True iff brotli is decodable in this process. */
extern bool n00b_http_have_brotli(void);

/** @brief True iff zstd is decodable in this process. */
extern bool n00b_http_have_zstd(void);

/**
 * @brief Compress @p src using @p encoding (request-body path).
 *
 * Inverse of `n00b_http_decompress`.  Today we only ship gzip and
 * deflate (libz-backed); brotli + zstd request-body compression is
 * a follow-up — most server stacks accept gzip.
 *
 * @param src       Plaintext bytes.
 * @param encoding  `"gzip"` or `"deflate"`.  Pass nullptr / `"identity"`
 *                  for a pass-through copy.
 *
 * @kw allocator  Default per-runtime conduit pool.
 * @kw level      Compression level 1..9 (libz convention); default
 *                `Z_DEFAULT_COMPRESSION` (-1).
 *
 * @return  Result with the encoded bytes; err on unsupported
 *          encoding or libz failure.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_http_compress(n00b_buffer_t *src, const char *encoding)
    _kargs {
        n00b_allocator_t *allocator = nullptr;
        int               level     = -1;
    };
