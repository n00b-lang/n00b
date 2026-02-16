/**
 * @file sha256.h
 * @brief SHA-256 cryptographic hash implementation with module struct API.
 *
 * Provides both streaming (init/update/finalize) and one-shot interfaces
 * for computing SHA-256 hashes.
 *
 * ## Namespace Configuration
 *
 * Default prefix is "ncc_". To change:
 *
 *     #define NCC_LIB_PREFIX mylib_
 *     #include "sha256.h"
 *     // Now: mylib_sha256.hash(), mylib_sha256_ctx_t, etc.
 *
 * ## Usage
 *
 *     uint32_t digest[8];
 *     ncc_sha256.hash("hello", 5, digest);
 *
 *     // Or streaming:
 *     ncc_sha256_ctx_t ctx;
 *     ncc_sha256.init(&ctx);
 *     ncc_sha256.update(&ctx, data, len);
 *     ncc_sha256.finalize(&ctx, digest);
 */
#pragma once

#include <stdint.h>
#include <arpa/inet.h>

#define NCC_SHA256_BLOCK_SIZE (512/8)

typedef uint32_t ncc_sha256_digest_t[NCC_SHA256_BLOCK_SIZE / sizeof(void *)];

/**
 * @brief SHA-256 context for streaming hashes (internal definition).
 */
typedef struct ncc_sha256_ctx {
    ncc_sha256_digest_t hv;                           /**< Current hash values */
    uint64_t                 len;                          /**< Total bytes processed */
    uint8_t                  buffer[NCC_SHA256_BLOCK_SIZE]; /**< Partial block buffer */
    uint32_t                 byte_count;                   /**< Bytes in partial buffer */
} ncc_sha256_ctx_t;

extern void ncc_sha256_init(ncc_sha256_ctx_t *ctx);
extern void ncc_sha256_update(ncc_sha256_ctx_t *ctx, const void *data, uint64_t len);
extern void ncc_sha256_finalize(ncc_sha256_ctx_t *ctx, ncc_sha256_digest_t digest);
extern void ncc_sha256_hash(void *data, uint64_t len, ncc_sha256_digest_t digest);
