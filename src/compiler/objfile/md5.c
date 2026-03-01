#include <string.h>
#include "compiler/objfile/md5.h"

// MD5 constants (RFC 1321)
#define F(x, y, z) (((x) & (y)) | ((~(x)) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~(z))))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~(z))))

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define STEP(f, a, b, c, d, x, t, s) do { \
    (a) += f((b), (c), (d)) + (x) + (t);  \
    (a)  = ROL((a), (s));                   \
    (a) += (b);                             \
} while (0)

static void
md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];

    for (int i = 0; i < 16; i++) {
        memcpy(&M[i], block + i * 4, 4);
    }

    // Round 1
    STEP(F, a, b, c, d, M[ 0], 0xD76AA478,  7);
    STEP(F, d, a, b, c, M[ 1], 0xE8C7B756, 12);
    STEP(F, c, d, a, b, M[ 2], 0x242070DB, 17);
    STEP(F, b, c, d, a, M[ 3], 0xC1BDCEEE, 22);
    STEP(F, a, b, c, d, M[ 4], 0xF57C0FAF,  7);
    STEP(F, d, a, b, c, M[ 5], 0x4787C62A, 12);
    STEP(F, c, d, a, b, M[ 6], 0xA8304613, 17);
    STEP(F, b, c, d, a, M[ 7], 0xFD469501, 22);
    STEP(F, a, b, c, d, M[ 8], 0x698098D8,  7);
    STEP(F, d, a, b, c, M[ 9], 0x8B44F7AF, 12);
    STEP(F, c, d, a, b, M[10], 0xFFFF5BB1, 17);
    STEP(F, b, c, d, a, M[11], 0x895CD7BE, 22);
    STEP(F, a, b, c, d, M[12], 0x6B901122,  7);
    STEP(F, d, a, b, c, M[13], 0xFD987193, 12);
    STEP(F, c, d, a, b, M[14], 0xA679438E, 17);
    STEP(F, b, c, d, a, M[15], 0x49B40821, 22);

    // Round 2
    STEP(G, a, b, c, d, M[ 1], 0xF61E2562,  5);
    STEP(G, d, a, b, c, M[ 6], 0xC040B340,  9);
    STEP(G, c, d, a, b, M[11], 0x265E5A51, 14);
    STEP(G, b, c, d, a, M[ 0], 0xE9B6C7AA, 20);
    STEP(G, a, b, c, d, M[ 5], 0xD62F105D,  5);
    STEP(G, d, a, b, c, M[10], 0x02441453,  9);
    STEP(G, c, d, a, b, M[15], 0xD8A1E681, 14);
    STEP(G, b, c, d, a, M[ 4], 0xE7D3FBC8, 20);
    STEP(G, a, b, c, d, M[ 9], 0x21E1CDE6,  5);
    STEP(G, d, a, b, c, M[14], 0xC33707D6,  9);
    STEP(G, c, d, a, b, M[ 3], 0xF4D50D87, 14);
    STEP(G, b, c, d, a, M[ 8], 0x455A14ED, 20);
    STEP(G, a, b, c, d, M[13], 0xA9E3E905,  5);
    STEP(G, d, a, b, c, M[ 2], 0xFCEFA3F8,  9);
    STEP(G, c, d, a, b, M[ 7], 0x676F02D9, 14);
    STEP(G, b, c, d, a, M[12], 0x8D2A4C8A, 20);

    // Round 3
    STEP(H, a, b, c, d, M[ 5], 0xFFFA3942,  4);
    STEP(H, d, a, b, c, M[ 8], 0x8771F681, 11);
    STEP(H, c, d, a, b, M[11], 0x6D9D6122, 16);
    STEP(H, b, c, d, a, M[14], 0xFDE5380C, 23);
    STEP(H, a, b, c, d, M[ 1], 0xA4BEEA44,  4);
    STEP(H, d, a, b, c, M[ 4], 0x4BDECFA9, 11);
    STEP(H, c, d, a, b, M[ 7], 0xF6BB4B60, 16);
    STEP(H, b, c, d, a, M[10], 0xBEBFBC70, 23);
    STEP(H, a, b, c, d, M[13], 0x289B7EC6,  4);
    STEP(H, d, a, b, c, M[ 0], 0xEAA127FA, 11);
    STEP(H, c, d, a, b, M[ 3], 0xD4EF3085, 16);
    STEP(H, b, c, d, a, M[ 6], 0x04881D05, 23);
    STEP(H, a, b, c, d, M[ 9], 0xD9D4D039,  4);
    STEP(H, d, a, b, c, M[12], 0xE6DB99E5, 11);
    STEP(H, c, d, a, b, M[15], 0x1FA27CF8, 16);
    STEP(H, b, c, d, a, M[ 2], 0xC4AC5665, 23);

    // Round 4
    STEP(I, a, b, c, d, M[ 0], 0xF4292244,  6);
    STEP(I, d, a, b, c, M[ 7], 0x432AFF97, 10);
    STEP(I, c, d, a, b, M[14], 0xAB9423A7, 15);
    STEP(I, b, c, d, a, M[ 5], 0xFC93A039, 21);
    STEP(I, a, b, c, d, M[12], 0x655B59C3,  6);
    STEP(I, d, a, b, c, M[ 3], 0x8F0CCC92, 10);
    STEP(I, c, d, a, b, M[10], 0xFFEFF47D, 15);
    STEP(I, b, c, d, a, M[ 1], 0x85845DD1, 21);
    STEP(I, a, b, c, d, M[ 8], 0x6FA87E4F,  6);
    STEP(I, d, a, b, c, M[15], 0xFE2CE6E0, 10);
    STEP(I, c, d, a, b, M[ 6], 0xA3014314, 15);
    STEP(I, b, c, d, a, M[13], 0x4E0811A1, 21);
    STEP(I, a, b, c, d, M[ 4], 0xF7537E82,  6);
    STEP(I, d, a, b, c, M[11], 0xBD3AF235, 10);
    STEP(I, c, d, a, b, M[ 2], 0x2AD7D2BB, 15);
    STEP(I, b, c, d, a, M[ 9], 0xEB86D391, 21);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void
n00b_md5_init(n00b_md5_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->count    = 0;
}

void
n00b_md5_update(n00b_md5_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t         idx = (size_t)(ctx->count & 63);

    ctx->count += len;

    if (idx > 0) {
        size_t fill = 64 - idx;

        if (len < fill) {
            memcpy(ctx->buffer + idx, p, len);
            return;
        }

        memcpy(ctx->buffer + idx, p, fill);
        md5_transform(ctx->state, ctx->buffer);
        p   += fill;
        len -= fill;
    }

    while (len >= 64) {
        md5_transform(ctx->state, p);
        p   += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buffer, p, len);
    }
}

void
n00b_md5_finalize(n00b_md5_ctx_t *ctx, uint8_t digest[16])
{
    uint8_t  pad[64];
    uint64_t bits = ctx->count * 8;
    size_t   idx  = (size_t)(ctx->count & 63);
    size_t   pad_len = (idx < 56) ? (56 - idx) : (120 - idx);

    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    n00b_md5_update(ctx, pad, pad_len);

    // Append bit count (little-endian).
    uint8_t count_bytes[8];

    memcpy(count_bytes, &bits, 8);
    n00b_md5_update(ctx, count_bytes, 8);

    // Output digest (little-endian).
    for (int i = 0; i < 4; i++) {
        memcpy(digest + i * 4, &ctx->state[i], 4);
    }
}

void
n00b_md5(const uint8_t *data, size_t len, uint8_t out[16])
{
    n00b_md5_ctx_t ctx;

    n00b_md5_init(&ctx);
    n00b_md5_update(&ctx, data, len);
    n00b_md5_finalize(&ctx, out);
}
