/**
 * @file sha256.c
 * @brief SHA-256 implementation with module struct API.
 */

#define NCC_LIB_IMPL       // Prevent compat macros from interfering with definitions
#define NCC_SHA256_INTERNAL // Enable internal struct definition
#include "sha256.h"

#include <string.h>
#include <stddef.h>

// clang-format off
static const uint32_t K[] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};


static const uint32_t init_vec[] = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static_assert(sizeof(K) / sizeof(K[0]) == 64);
static_assert(sizeof(init_vec) / sizeof(init_vec[0]) == 8);

// clang-format on

#define PAD_START 0x80

static inline uint32_t
ROTR(uint32_t val, uint32_t n)
{
    return (val >> n) | (val << (32 - n));
}

static inline uint32_t
CH(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (~(x)&z);
}

static inline uint32_t
MAJ(uint32_t x, uint32_t y, uint32_t z)
{
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline uint32_t
SIGMA0(uint32_t x)
{
    return ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22);
}

static inline uint32_t
SIGMA1(uint32_t x)
{
    return ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25);
}

static inline uint32_t
sigma0(uint32_t x)
{
    return ROTR(x, 7) ^ ROTR(x, 18) ^ (x >> 3);
}

static inline uint32_t
sigma1(uint32_t x)
{
    return ROTR(x, 17) ^ ROTR(x, 19) ^ (x >> 10);
}

static void
sha256_one_block(ncc_sha256_ctx_t *ctx, const uint8_t *block)
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;

    for (int i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i * 4 + 0] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] << 8)
             | ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = sigma1(W[i - 2]) + W[i - 7] + sigma0(W[i - 15]) + W[i - 16];
    }

    a = ctx->hv[0];
    b = ctx->hv[1];
    c = ctx->hv[2];
    d = ctx->hv[3];
    e = ctx->hv[4];
    f = ctx->hv[5];
    g = ctx->hv[6];
    h = ctx->hv[7];

    // Main loop
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SIGMA1(e) + CH(e, f, g) + K[i] + W[i];
        uint32_t t2 = SIGMA0(a) + MAJ(a, b, c);
        h           = g;
        g           = f;
        f           = e;
        e           = d + t1;
        d           = c;
        c           = b;
        b           = a;
        a           = t1 + t2;
    }

    // Update state
    ctx->hv[0] += a;
    ctx->hv[1] += b;
    ctx->hv[2] += c;
    ctx->hv[3] += d;
    ctx->hv[4] += e;
    ctx->hv[5] += f;
    ctx->hv[6] += g;
    ctx->hv[7] += h;
}

void
ncc_sha256_init(ncc_sha256_ctx_t *ctx)
{
    ctx->byte_count = 0;
    memcpy(ctx->hv, init_vec, sizeof(init_vec));
}

void
ncc_sha256_update(ncc_sha256_ctx_t *ctx, const void *data, uint64_t bytelen)
{
    const uint8_t *p        = data;
    size_t         buffered = ctx->byte_count % 64;

    ctx->byte_count += bytelen;

    // If we have buffered data, try to complete a block
    if (buffered > 0) {
        size_t needed = 64 - buffered;
        if (bytelen < needed) {
            memcpy(ctx->buffer + buffered, p, bytelen);
            return;
        }
        memcpy(ctx->buffer + buffered, p, needed);
        sha256_one_block(ctx, ctx->buffer);
        p += needed;
        bytelen -= needed;
    }

    // Process complete blocks
    while (bytelen >= 64) {
        sha256_one_block(ctx, p);
        p += 64;
        bytelen -= 64;
    }

    // Buffer remaining bytes
    if (bytelen > 0) {
        memcpy(ctx->buffer, p, bytelen);
    }
}

void
ncc_sha256_finalize(ncc_sha256_ctx_t *ctx, ncc_sha256_digest_t outbuf)
{
    size_t   buffered = ctx->byte_count % 64;
    uint64_t bits     = ctx->byte_count * 8;

    ctx->buffer[buffered++] = PAD_START;

    // If not enough room for length, pad and process the block.
    if (buffered > 56) {
        memset(ctx->buffer + buffered, 0, 64 - buffered);
        sha256_one_block(ctx, ctx->buffer);
        buffered = 0;
    }

    // Pad with zeros
    memset(ctx->buffer + buffered, 0, 56 - buffered);

    // Append length in big-endian
    ctx->buffer[56] = (uint8_t)(bits >> 56);
    ctx->buffer[57] = (uint8_t)(bits >> 48);
    ctx->buffer[58] = (uint8_t)(bits >> 40);
    ctx->buffer[59] = (uint8_t)(bits >> 32);
    ctx->buffer[60] = (uint8_t)(bits >> 24);
    ctx->buffer[61] = (uint8_t)(bits >> 16);
    ctx->buffer[62] = (uint8_t)(bits >> 8);
    ctx->buffer[63] = (uint8_t)(bits);

    sha256_one_block(ctx, ctx->buffer);

    for (unsigned int i = 0; i < NCC_SHA256_BLOCK_SIZE / sizeof(void *); i++) {
        ctx->hv[i] = htonl(ctx->hv[i]);
    }

    memcpy(outbuf, ctx->hv, sizeof(ctx->hv));
}

void
ncc_sha256_hash(void *data, uint64_t bytelen, ncc_sha256_digest_t outbuf)
{
    ncc_sha256_ctx_t ctx;
    ncc_sha256_init(&ctx);
    ncc_sha256_update(&ctx, data, bytelen);
    ncc_sha256_finalize(&ctx, outbuf);
}
