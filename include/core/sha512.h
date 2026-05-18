#pragma once

// SHA-512 / SHA-384 cryptographic hash — standalone, no dependencies
// beyond libc.  Per FIPS 180-4.  Mirrors the shape of
// `include/core/sha256.h`: a context type, init/update/finalize, and
// a convenience one-shot hash function.
//
// SHA-384 is mechanically derived from SHA-512: same block, same
// rounds, same compression, different initial-hash-value constants,
// and the final digest is the first 48 bytes of the 64-byte SHA-512
// output (i.e., the first 6 of 8 internal 64-bit words).

#include <stdint.h>
#include <stddef.h>

#define N00B_SHA512_BLOCK_SIZE    128
#define N00B_SHA512_DIGEST_WORDS  8
#define N00B_SHA512_DIGEST_BYTES  64
#define N00B_SHA384_DIGEST_BYTES  48

typedef uint64_t n00b_sha512_digest_t[N00B_SHA512_DIGEST_WORDS];

typedef struct {
    n00b_sha512_digest_t hv;
    uint8_t              buffer[N00B_SHA512_BLOCK_SIZE];
    uint64_t             byte_count;
} n00b_sha512_ctx_t;

// SHA-512 ----------------------------------------------------------

void n00b_sha512_init(n00b_sha512_ctx_t *ctx);
void n00b_sha512_update(n00b_sha512_ctx_t *ctx, const void *data, size_t len);
void n00b_sha512_finalize(n00b_sha512_ctx_t *ctx, n00b_sha512_digest_t digest);

// Convenience: one-shot SHA-512 of @p data into @p digest.
void n00b_sha512_hash(const void *data, size_t len,
                      n00b_sha512_digest_t digest);

// Convenience: one-shot SHA-512 producing 64 bytes in big-endian
// canonical form (matches what `openssl dgst -sha512 -binary`
// emits).  The internal hv is uint64_t in host byte order; this
// helper does the byte-swap into the output.
void n00b_sha512_hash_be(const void *data, size_t len,
                         uint8_t out[N00B_SHA512_DIGEST_BYTES]);

// SHA-384 ----------------------------------------------------------

// SHA-384 shares the context type with SHA-512; only the init
// constants and final truncation differ.

void n00b_sha384_init(n00b_sha512_ctx_t *ctx);
void n00b_sha384_finalize(n00b_sha512_ctx_t *ctx,
                          uint8_t digest[N00B_SHA384_DIGEST_BYTES]);
void n00b_sha384_hash(const void *data, size_t len,
                      uint8_t out[N00B_SHA384_DIGEST_BYTES]);
