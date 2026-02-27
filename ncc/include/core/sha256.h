#pragma once

// SHA-256 cryptographic hash — standalone, no dependencies beyond libc.

#include <stdint.h>
#include <stddef.h>

#define N00B_SHA256_BLOCK_SIZE 64
#define N00B_SHA256_DIGEST_WORDS 8

typedef uint32_t n00b_sha256_digest_t[N00B_SHA256_DIGEST_WORDS];

typedef struct {
    n00b_sha256_digest_t hv;
    uint8_t              buffer[N00B_SHA256_BLOCK_SIZE];
    uint64_t             byte_count;
} n00b_sha256_ctx_t;

void n00b_sha256_init(n00b_sha256_ctx_t *ctx);
void n00b_sha256_update(n00b_sha256_ctx_t *ctx, const void *data, size_t len);
void n00b_sha256_finalize(n00b_sha256_ctx_t *ctx, n00b_sha256_digest_t digest);
void n00b_sha256_hash(const void *data, size_t len, n00b_sha256_digest_t digest);
