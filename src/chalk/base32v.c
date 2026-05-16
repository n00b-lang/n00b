/** @file src/chalk/base32v.c — nimutils "v" base32 encoder.
 *
 *  Alphabet: 0-9 then A-H, J, K, M, N, P-T, V-Z (omitting I, L, O, U).
 *  Standard 5-bit-per-char encoding, big-endian bit ordering (MSB of
 *  byte 0 → high bit of the first output char), no padding.
 *
 *  Sourced verbatim from nimutils encodings.nim:191
 *  (`declB32Encoder("v", goodB32Map)`).
 */

#include "n00b.h"
#include "core/string.h"
#include "core/alloc.h"
#include "internal/chalk/base32v.h"

static const char k_alphabet[32] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
    'G', 'H', 'J', 'K', 'M', 'N', 'P', 'Q',
    'R', 'S', 'T', 'V', 'W', 'X', 'Y', 'Z',
};

static inline char
e32(int c)
{
    return k_alphabet[c & 0x1f];
}

n00b_string_t *
n00b_chalk_base32v_encode(const uint8_t *data, size_t len)
{
    // ceil(len * 8 / 5) chars; allocate len*2 to be safe.
    size_t cap = len * 2 + 8;
    char  *out = n00b_alloc_array(char, cap);
    size_t op  = 0;

    size_t full_blocks = len / 5;
    size_t i           = 0;
    int    t1, t2;

    for (size_t b = 0; b < full_blocks; b++) {
        t1        = data[i++];
        out[op++] = e32(t1 >> 3);
        t2        = data[i++];
        out[op++] = e32((t1 << 2) | (t2 >> 6));
        out[op++] = e32(t2 >> 1);
        t1        = t2 << 4;
        t2        = data[i++];
        out[op++] = e32(t1 | (t2 >> 4));
        t1        = t2 << 1;
        t2        = data[i++];
        out[op++] = e32(t1 | (t2 >> 7));
        out[op++] = e32(t2 >> 2);
        t1        = t2 << 3;
        t2        = data[i++];
        out[op++] = e32(t1 | (t2 >> 5));
        out[op++] = e32(t2);
    }

    // Tail (0-4 remaining bytes). Mirrors nimutils' fall-through logic.
    if (i < len) {
        t1        = data[i++];
        out[op++] = e32(t1 >> 3);
        if (i == len) {
            out[op++] = e32(t1 << 2);
        }
        else {
            t2        = data[i++];
            out[op++] = e32((t1 << 2) | (t2 >> 6));
            out[op++] = e32(t2 >> 1);
            t1        = t2 << 4;
            if (i == len) {
                out[op++] = e32(t1);
            }
            else {
                t2        = data[i++];
                out[op++] = e32(t1 | (t2 >> 4));
                t1        = t2 << 1;
                if (i == len) {
                    out[op++] = e32(t1);
                }
                else {
                    t2        = data[i++];
                    out[op++] = e32(t1 | (t2 >> 7));
                    out[op++] = e32(t2 >> 2);
                    t1        = t2 << 3;
                    out[op++] = e32(t1);
                }
            }
        }
    }

    return n00b_string_from_raw(out, op);
}

static n00b_string_t *
slice_format(n00b_string_t *full)
{
    char out[24];
    for (int i = 0; i < 6; i++)  out[i]      = full->data[i];
    out[6]  = '-';
    for (int i = 0; i < 4; i++)  out[7 + i]  = full->data[6 + i];
    out[11] = '-';
    for (int i = 0; i < 4; i++)  out[12 + i] = full->data[10 + i];
    out[16] = '-';
    for (int i = 0; i < 6; i++)  out[17 + i] = full->data[14 + i];
    out[23] = '\0';
    return n00b_string_from_raw(out, 23);
}

// METADATA_ID input: 32 raw SHA-256 bytes.
n00b_string_t *
n00b_chalk_id_format_sha256_bytes(const uint8_t sha256[32])
{
    return slice_format(n00b_chalk_base32v_encode(sha256, 32));
}

// CHALK_ID input: the 64-char lowercase hex string of the SHA-256.
n00b_string_t *
n00b_chalk_id_format_sha256_hex(const uint8_t sha256[32])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t hex[64];
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = (uint8_t)hexd[(sha256[i] >> 4) & 0xf];
        hex[i * 2 + 1] = (uint8_t)hexd[sha256[i] & 0xf];
    }
    return slice_format(n00b_chalk_base32v_encode(hex, 64));
}
