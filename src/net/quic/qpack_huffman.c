/*
 * qpack_huffman.c — RFC 7541 Appendix B Huffman codec.
 *
 * The QPACK spec (RFC 9204 § 4.1.2) reuses HPACK's Huffman table
 * verbatim; the entries are reproduced here.  Symbols 0..255 are
 * data bytes; symbol 256 is the End-of-String (EOS) marker, which
 * is **never emitted** — it's only used to derive the padding rule
 * (RFC 7541 § 5.2): a string ends with up to 7 high-bits-of-EOS
 * padding, never 8+ bits, never with EOS embedded.
 *
 * Implementation:
 *
 *   - Encode is a per-symbol bit append.  We accumulate into a
 *     64-bit register and flush full bytes.
 *
 *   - Decode is bit-by-bit, walking down the table using the
 *     "implicit code-length" property: codes are sorted in
 *     numeric order within each length class; we precompute a
 *     small lookup for each length to accelerate the search.
 *
 * Decode-by-bit is slow vs. tree-walk implementations but the
 * inputs are short (header values ≤ 64 KiB).  Optimization is a
 * later concern.
 */
#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "internal/net/quic/qpack_internal.h"
#include "net/quic/qpack.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Huffman code table — RFC 7541 Appendix B.
 *
 * Indexed by symbol (0..256).  Each entry stores the right-aligned
 * code (low `bits` bits) and the code length in bits.
 * =========================================================================== */

const n00b_qpack_huffman_sym_t n00b_qpack_huffman_table[257] = {
    /*   0 */ { 0x00001ff8u, 13 }, /*   1 */ { 0x007fffd8u, 23 },
    /*   2 */ { 0x0fffffe2u, 28 }, /*   3 */ { 0x0fffffe3u, 28 },
    /*   4 */ { 0x0fffffe4u, 28 }, /*   5 */ { 0x0fffffe5u, 28 },
    /*   6 */ { 0x0fffffe6u, 28 }, /*   7 */ { 0x0fffffe7u, 28 },
    /*   8 */ { 0x0fffffe8u, 28 }, /*   9 */ { 0x00ffffeau, 24 },
    /*  10 */ { 0x3ffffffcu, 30 }, /*  11 */ { 0x0fffffe9u, 28 },
    /*  12 */ { 0x0fffffeau, 28 }, /*  13 */ { 0x3ffffffdu, 30 },
    /*  14 */ { 0x0fffffebu, 28 }, /*  15 */ { 0x0fffffecu, 28 },
    /*  16 */ { 0x0fffffedu, 28 }, /*  17 */ { 0x0fffffeeu, 28 },
    /*  18 */ { 0x0fffffefu, 28 }, /*  19 */ { 0x0ffffff0u, 28 },
    /*  20 */ { 0x0ffffff1u, 28 }, /*  21 */ { 0x0ffffff2u, 28 },
    /*  22 */ { 0x3ffffffeu, 30 }, /*  23 */ { 0x0ffffff3u, 28 },
    /*  24 */ { 0x0ffffff4u, 28 }, /*  25 */ { 0x0ffffff5u, 28 },
    /*  26 */ { 0x0ffffff6u, 28 }, /*  27 */ { 0x0ffffff7u, 28 },
    /*  28 */ { 0x0ffffff8u, 28 }, /*  29 */ { 0x0ffffff9u, 28 },
    /*  30 */ { 0x0ffffffau, 28 }, /*  31 */ { 0x0ffffffbu, 28 },
    /*  32 */ { 0x00000014u,  6 }, /*  33 */ { 0x000003f8u, 10 },
    /*  34 */ { 0x000003f9u, 10 }, /*  35 */ { 0x00000ffau, 12 },
    /*  36 */ { 0x00001ff9u, 13 }, /*  37 */ { 0x00000015u,  6 },
    /*  38 */ { 0x000000f8u,  8 }, /*  39 */ { 0x000007fau, 11 },
    /*  40 */ { 0x000003fau, 10 }, /*  41 */ { 0x000003fbu, 10 },
    /*  42 */ { 0x000000f9u,  8 }, /*  43 */ { 0x000007fbu, 11 },
    /*  44 */ { 0x000000fau,  8 }, /*  45 */ { 0x00000016u,  6 },
    /*  46 */ { 0x00000017u,  6 }, /*  47 */ { 0x00000018u,  6 },
    /*  48 */ { 0x00000000u,  5 }, /*  49 */ { 0x00000001u,  5 },
    /*  50 */ { 0x00000002u,  5 }, /*  51 */ { 0x00000019u,  6 },
    /*  52 */ { 0x0000001au,  6 }, /*  53 */ { 0x0000001bu,  6 },
    /*  54 */ { 0x0000001cu,  6 }, /*  55 */ { 0x0000001du,  6 },
    /*  56 */ { 0x0000001eu,  6 }, /*  57 */ { 0x0000001fu,  6 },
    /*  58 */ { 0x0000005cu,  7 }, /*  59 */ { 0x000000fbu,  8 },
    /*  60 */ { 0x00007ffcu, 15 }, /*  61 */ { 0x00000020u,  6 },
    /*  62 */ { 0x00000ffbu, 12 }, /*  63 */ { 0x000003fcu, 10 },
    /*  64 */ { 0x00001ffau, 13 }, /*  65 */ { 0x00000021u,  6 },
    /*  66 */ { 0x0000005du,  7 }, /*  67 */ { 0x0000005eu,  7 },
    /*  68 */ { 0x0000005fu,  7 }, /*  69 */ { 0x00000060u,  7 },
    /*  70 */ { 0x00000061u,  7 }, /*  71 */ { 0x00000062u,  7 },
    /*  72 */ { 0x00000063u,  7 }, /*  73 */ { 0x00000064u,  7 },
    /*  74 */ { 0x00000065u,  7 }, /*  75 */ { 0x00000066u,  7 },
    /*  76 */ { 0x00000067u,  7 }, /*  77 */ { 0x00000068u,  7 },
    /*  78 */ { 0x00000069u,  7 }, /*  79 */ { 0x0000006au,  7 },
    /*  80 */ { 0x0000006bu,  7 }, /*  81 */ { 0x0000006cu,  7 },
    /*  82 */ { 0x0000006du,  7 }, /*  83 */ { 0x0000006eu,  7 },
    /*  84 */ { 0x0000006fu,  7 }, /*  85 */ { 0x00000070u,  7 },
    /*  86 */ { 0x00000071u,  7 }, /*  87 */ { 0x00000072u,  7 },
    /*  88 */ { 0x000000fcu,  8 }, /*  89 */ { 0x00000073u,  7 },
    /*  90 */ { 0x000000fdu,  8 }, /*  91 */ { 0x00001ffbu, 13 },
    /*  92 */ { 0x0007fff0u, 19 }, /*  93 */ { 0x00001ffcu, 13 },
    /*  94 */ { 0x00003ffcu, 14 }, /*  95 */ { 0x00000022u,  6 },
    /*  96 */ { 0x00007ffdu, 15 }, /*  97 */ { 0x00000003u,  5 },
    /*  98 */ { 0x00000023u,  6 }, /*  99 */ { 0x00000004u,  5 },
    /* 100 */ { 0x00000024u,  6 }, /* 101 */ { 0x00000005u,  5 },
    /* 102 */ { 0x00000025u,  6 }, /* 103 */ { 0x00000026u,  6 },
    /* 104 */ { 0x00000027u,  6 }, /* 105 */ { 0x00000006u,  5 },
    /* 106 */ { 0x00000074u,  7 }, /* 107 */ { 0x00000075u,  7 },
    /* 108 */ { 0x00000028u,  6 }, /* 109 */ { 0x00000029u,  6 },
    /* 110 */ { 0x0000002au,  6 }, /* 111 */ { 0x00000007u,  5 },
    /* 112 */ { 0x0000002bu,  6 }, /* 113 */ { 0x00000076u,  7 },
    /* 114 */ { 0x0000002cu,  6 }, /* 115 */ { 0x00000008u,  5 },
    /* 116 */ { 0x00000009u,  5 }, /* 117 */ { 0x0000002du,  6 },
    /* 118 */ { 0x00000077u,  7 }, /* 119 */ { 0x00000078u,  7 },
    /* 120 */ { 0x00000079u,  7 }, /* 121 */ { 0x0000007au,  7 },
    /* 122 */ { 0x0000007bu,  7 }, /* 123 */ { 0x00007ffeu, 15 },
    /* 124 */ { 0x000007fcu, 11 }, /* 125 */ { 0x00003ffdu, 14 },
    /* 126 */ { 0x00001ffdu, 13 }, /* 127 */ { 0x0ffffffcu, 28 },
    /* 128 */ { 0x000fffe6u, 20 }, /* 129 */ { 0x003fffd2u, 22 },
    /* 130 */ { 0x000fffe7u, 20 }, /* 131 */ { 0x000fffe8u, 20 },
    /* 132 */ { 0x003fffd3u, 22 }, /* 133 */ { 0x003fffd4u, 22 },
    /* 134 */ { 0x003fffd5u, 22 }, /* 135 */ { 0x007fffd9u, 23 },
    /* 136 */ { 0x003fffd6u, 22 }, /* 137 */ { 0x007fffdau, 23 },
    /* 138 */ { 0x007fffdbu, 23 }, /* 139 */ { 0x007fffdcu, 23 },
    /* 140 */ { 0x007fffddu, 23 }, /* 141 */ { 0x007fffdeu, 23 },
    /* 142 */ { 0x00ffffebu, 24 }, /* 143 */ { 0x007fffdfu, 23 },
    /* 144 */ { 0x00ffffecu, 24 }, /* 145 */ { 0x00ffffedu, 24 },
    /* 146 */ { 0x003fffd7u, 22 }, /* 147 */ { 0x007fffe0u, 23 },
    /* 148 */ { 0x00ffffeeu, 24 }, /* 149 */ { 0x007fffe1u, 23 },
    /* 150 */ { 0x007fffe2u, 23 }, /* 151 */ { 0x007fffe3u, 23 },
    /* 152 */ { 0x007fffe4u, 23 }, /* 153 */ { 0x001fffdcu, 21 },
    /* 154 */ { 0x003fffd8u, 22 }, /* 155 */ { 0x007fffe5u, 23 },
    /* 156 */ { 0x003fffd9u, 22 }, /* 157 */ { 0x007fffe6u, 23 },
    /* 158 */ { 0x007fffe7u, 23 }, /* 159 */ { 0x00ffffefu, 24 },
    /* 160 */ { 0x003fffdau, 22 }, /* 161 */ { 0x001fffddu, 21 },
    /* 162 */ { 0x000fffe9u, 20 }, /* 163 */ { 0x003fffdbu, 22 },
    /* 164 */ { 0x003fffdcu, 22 }, /* 165 */ { 0x007fffe8u, 23 },
    /* 166 */ { 0x007fffe9u, 23 }, /* 167 */ { 0x001fffdeu, 21 },
    /* 168 */ { 0x007fffeau, 23 }, /* 169 */ { 0x003fffddu, 22 },
    /* 170 */ { 0x003fffdeu, 22 }, /* 171 */ { 0x00fffff0u, 24 },
    /* 172 */ { 0x001fffdfu, 21 }, /* 173 */ { 0x003fffdfu, 22 },
    /* 174 */ { 0x007fffebu, 23 }, /* 175 */ { 0x007fffecu, 23 },
    /* 176 */ { 0x001fffe0u, 21 }, /* 177 */ { 0x001fffe1u, 21 },
    /* 178 */ { 0x003fffe0u, 22 }, /* 179 */ { 0x001fffe2u, 21 },
    /* 180 */ { 0x007fffedu, 23 }, /* 181 */ { 0x003fffe1u, 22 },
    /* 182 */ { 0x007fffeeu, 23 }, /* 183 */ { 0x007fffefu, 23 },
    /* 184 */ { 0x000fffeau, 20 }, /* 185 */ { 0x003fffe2u, 22 },
    /* 186 */ { 0x003fffe3u, 22 }, /* 187 */ { 0x003fffe4u, 22 },
    /* 188 */ { 0x007ffff0u, 23 }, /* 189 */ { 0x003fffe5u, 22 },
    /* 190 */ { 0x003fffe6u, 22 }, /* 191 */ { 0x007ffff1u, 23 },
    /* 192 */ { 0x03ffffe0u, 26 }, /* 193 */ { 0x03ffffe1u, 26 },
    /* 194 */ { 0x000fffebu, 20 }, /* 195 */ { 0x0007fff1u, 19 },
    /* 196 */ { 0x003fffe7u, 22 }, /* 197 */ { 0x007ffff2u, 23 },
    /* 198 */ { 0x003fffe8u, 22 }, /* 199 */ { 0x01ffffecu, 25 },
    /* 200 */ { 0x03ffffe2u, 26 }, /* 201 */ { 0x03ffffe3u, 26 },
    /* 202 */ { 0x03ffffe4u, 26 }, /* 203 */ { 0x07ffffdeu, 27 },
    /* 204 */ { 0x07ffffdfu, 27 }, /* 205 */ { 0x03ffffe5u, 26 },
    /* 206 */ { 0x00fffff1u, 24 }, /* 207 */ { 0x01ffffedu, 25 },
    /* 208 */ { 0x0007fff2u, 19 }, /* 209 */ { 0x001fffe3u, 21 },
    /* 210 */ { 0x03ffffe6u, 26 }, /* 211 */ { 0x07ffffe0u, 27 },
    /* 212 */ { 0x07ffffe1u, 27 }, /* 213 */ { 0x03ffffe7u, 26 },
    /* 214 */ { 0x07ffffe2u, 27 }, /* 215 */ { 0x00fffff2u, 24 },
    /* 216 */ { 0x001fffe4u, 21 }, /* 217 */ { 0x001fffe5u, 21 },
    /* 218 */ { 0x03ffffe8u, 26 }, /* 219 */ { 0x03ffffe9u, 26 },
    /* 220 */ { 0x0ffffffdu, 28 }, /* 221 */ { 0x07ffffe3u, 27 },
    /* 222 */ { 0x07ffffe4u, 27 }, /* 223 */ { 0x07ffffe5u, 27 },
    /* 224 */ { 0x000fffecu, 20 }, /* 225 */ { 0x00fffff3u, 24 },
    /* 226 */ { 0x000fffedu, 20 }, /* 227 */ { 0x001fffe6u, 21 },
    /* 228 */ { 0x003fffe9u, 22 }, /* 229 */ { 0x001fffe7u, 21 },
    /* 230 */ { 0x001fffe8u, 21 }, /* 231 */ { 0x007ffff3u, 23 },
    /* 232 */ { 0x003fffeau, 22 }, /* 233 */ { 0x003fffebu, 22 },
    /* 234 */ { 0x01ffffeeu, 25 }, /* 235 */ { 0x01ffffefu, 25 },
    /* 236 */ { 0x00fffff4u, 24 }, /* 237 */ { 0x00fffff5u, 24 },
    /* 238 */ { 0x03ffffeau, 26 }, /* 239 */ { 0x007ffff4u, 23 },
    /* 240 */ { 0x03ffffebu, 26 }, /* 241 */ { 0x07ffffe6u, 27 },
    /* 242 */ { 0x03ffffecu, 26 }, /* 243 */ { 0x03ffffedu, 26 },
    /* 244 */ { 0x07ffffe7u, 27 }, /* 245 */ { 0x07ffffe8u, 27 },
    /* 246 */ { 0x07ffffe9u, 27 }, /* 247 */ { 0x07ffffeau, 27 },
    /* 248 */ { 0x07ffffebu, 27 }, /* 249 */ { 0x0ffffffeu, 28 },
    /* 250 */ { 0x07ffffecu, 27 }, /* 251 */ { 0x07ffffedu, 27 },
    /* 252 */ { 0x07ffffeeu, 27 }, /* 253 */ { 0x07ffffefu, 27 },
    /* 254 */ { 0x07fffff0u, 27 }, /* 255 */ { 0x03ffffeeu, 26 },
    /* 256 EOS */ { 0x3fffffffu, 30 },
};

/* ===========================================================================
 * Encode size — sum of bit-lengths, rounded up to byte.
 * =========================================================================== */

size_t
n00b_qpack_huffman_encoded_size(const uint8_t *src, size_t src_len)
{
    if (!src && src_len > 0) return 0;
    size_t bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        bits += n00b_qpack_huffman_table[src[i]].bits;
    }
    return (bits + 7) / 8;
}

/* ===========================================================================
 * Encode
 * =========================================================================== */

n00b_result_t(size_t)
n00b_qpack_huffman_encode(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap)
{
    if (!src && src_len > 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!dst && dst_cap > 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }

    size_t need = n00b_qpack_huffman_encoded_size(src, src_len);
    if (need > dst_cap) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    uint64_t reg     = 0;     /* bit accumulator (high bits valid first) */
    int      reg_bits = 0;    /* number of bits currently in reg, top-aligned */
    size_t   out_off = 0;

    for (size_t i = 0; i < src_len; i++) {
        const n00b_qpack_huffman_sym_t s = n00b_qpack_huffman_table[src[i]];
        /* Append code's `bits` bits to reg from the top. */
        reg |= ((uint64_t)s.code) << (64 - reg_bits - s.bits);
        reg_bits += s.bits;
        while (reg_bits >= 8) {
            dst[out_off++] = (uint8_t)(reg >> 56);
            reg <<= 8;
            reg_bits -= 8;
        }
    }
    /* Padding: high bits of EOS' code 0x3fffffff (30 bits all-ones).  Top
     * `8 - reg_bits` bits are all-ones, which is exactly the EOS prefix. */
    if (reg_bits > 0) {
        uint8_t pad_bits = (uint8_t)(8 - reg_bits);
        uint64_t pad     = ((UINT64_C(1) << pad_bits) - 1)
                           << (64 - 8);  /* pad_bits ones in top of next byte */
        reg |= pad;
        dst[out_off++] = (uint8_t)(reg >> 56);
    }
    return n00b_result_ok(size_t, out_off);
}

/* ===========================================================================
 * Decode (bit-by-bit; precomputed first-code-of-each-length)
 *
 * Algorithm (Hilbert / canonical-Huffman style): build at startup
 * - first_code[L]      = smallest code of length L (left-aligned)
 * - num_codes[L]       = count of codes of length L
 * - sym_offset[L]      = where symbols-of-length-L start in `sorted_syms`
 * - sorted_syms[]      = symbols sorted by (length, code)
 *
 * Decode: read bits one at a time into accumulator `acc`; when
 * `acc >= first_code[L] && acc < first_code[L] + num_codes[L]` we
 * have a match: symbol = sorted_syms[sym_offset[L] + (acc - first_code[L])].
 *
 * For RFC 7541's table, code lengths span 5..30 bits.
 * =========================================================================== */

#define MIN_LEN  5
#define MAX_LEN  30

static uint32_t s_first_code[MAX_LEN + 2];     /* indexed by L; L+1 = +inf */
static uint32_t s_num_codes [MAX_LEN + 2];
static uint32_t s_sym_offset[MAX_LEN + 2];
static uint16_t s_sorted_syms[257];
static bool     s_init = false;

static void
huff_init_decoder_tables(void)
{
    if (s_init) return;

    /* count by length */
    for (int L = 0; L <= MAX_LEN + 1; L++) {
        s_num_codes[L] = 0;
        s_first_code[L] = 0;
    }
    for (int sym = 0; sym <= 256; sym++) {
        s_num_codes[n00b_qpack_huffman_table[sym].bits]++;
    }
    /* offsets */
    uint32_t off = 0;
    for (int L = 0; L <= MAX_LEN + 1; L++) {
        s_sym_offset[L] = off;
        off += s_num_codes[L];
    }
    /* place symbols, sorted by code ascending within length */
    /* easy approach: copy lengths into temp + sort */
    for (int sym = 0; sym <= 256; sym++) {
        uint32_t L = n00b_qpack_huffman_table[sym].bits;
        /* find insertion point in [s_sym_offset[L], s_sym_offset[L]+...) */
        uint32_t base = s_sym_offset[L];
        /* Insertion-sort by code */
        uint32_t i = 0;
        for (; i < s_num_codes[L]; i++) {
            uint16_t cur = s_sorted_syms[base + i];
            if (!cur) {
                /* Use 0xffff as "empty" sentinel; symbol 0 has its own
                 * length (13), so symbol 0 only exists in its own bucket
                 * — but to avoid the ambiguity we initialize bucket-by-bucket
                 * below.  Skip this block. */
                break;
            }
        }
        (void)i;
    }
    /* Rewrite cleanly: per L, collect symbols with that L, sort, store. */
    for (int L = 0; L <= MAX_LEN; L++) {
        uint32_t base = s_sym_offset[L];
        uint32_t k = 0;
        for (int sym = 0; sym <= 256; sym++) {
            if ((int)n00b_qpack_huffman_table[sym].bits == L) {
                s_sorted_syms[base + k++] = (uint16_t)sym;
            }
        }
        /* Sort ascending by code (insertion sort; small bins). */
        for (uint32_t i = 1; i < k; i++) {
            uint16_t cur = s_sorted_syms[base + i];
            uint32_t cur_code = n00b_qpack_huffman_table[cur].code;
            uint32_t j = i;
            while (j > 0 &&
                   n00b_qpack_huffman_table[s_sorted_syms[base + j - 1]].code
                   > cur_code) {
                s_sorted_syms[base + j] = s_sorted_syms[base + j - 1];
                j--;
            }
            s_sorted_syms[base + j] = cur;
        }
    }

    /* first_code[L] = (smallest code of length L) << (32 - L) — left-aligned. */
    for (int L = 0; L <= MAX_LEN + 1; L++) {
        if (s_num_codes[L] == 0) {
            /* Inherit (left-aligned smallest impossible code) using
             * standard canonical-Huffman recursion. */
            if (L == 0) {
                s_first_code[L] = 0;
            } else {
                /* "next" formula isn't strictly needed in this lookup
                 * approach — we only use first_code[L] when num_codes[L] > 0. */
                s_first_code[L] = 0xffffffffu;
            }
        } else {
            uint32_t smallest =
                n00b_qpack_huffman_table[s_sorted_syms[s_sym_offset[L]]].code;
            s_first_code[L] = smallest << (32 - L);
        }
    }
    s_init = true;
}

n00b_result_t(size_t)
n00b_qpack_huffman_decode(const uint8_t *src, size_t src_len,
                          uint8_t *dst, size_t dst_cap)
{
    huff_init_decoder_tables();

    if (!src && src_len > 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!dst && dst_cap > 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }

    size_t   src_off = 0;
    int      bits_in = 0;     /* total bits available in `acc` */
    uint64_t acc     = 0;     /* left-aligned bit buffer (upper bits valid) */
    size_t   dst_off = 0;

    while (1) {
        /* Refill acc up to 56 bits. */
        while (bits_in <= 56 && src_off < src_len) {
            acc |= ((uint64_t)src[src_off++]) << (56 - bits_in);
            bits_in += 8;
        }
        if (bits_in == 0) break;

        /* Try each length class.  Use the top L bits of acc. */
        int  matched = 0;
        for (int L = MIN_LEN; L <= MAX_LEN && L <= bits_in; L++) {
            if (s_num_codes[L] == 0) continue;
            uint32_t code_la = (uint32_t)(acc >> (64 - L));   /* L bits */
            uint32_t code    = code_la;
            uint32_t base    = n00b_qpack_huffman_table
                               [s_sorted_syms[s_sym_offset[L]]].code;
            if (code >= base && code < base + s_num_codes[L]) {
                uint16_t sym =
                    s_sorted_syms[s_sym_offset[L] + (code - base)];
                if (sym == 256) {
                    /* RFC 7541 § 5.2: EOS MUST NOT be embedded. */
                    return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
                }
                if (dst_off >= dst_cap) {
                    return n00b_result_err(size_t,
                                           N00B_QUIC_ERR_FRAME_TOO_LARGE);
                }
                dst[dst_off++] = (uint8_t)sym;
                acc <<= L;
                bits_in -= L;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            /* Not enough bits or no symbol consumed at any length.  We
             * must be at the end of input with < MAX_LEN bits left; the
             * remainder must be all-ones (EOS prefix) and < 8 bits. */
            if (src_off < src_len) {
                /* Still bytes to read but no match found — bug. */
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            if (bits_in >= 8) {
                /* RFC 7541 § 5.2: padding strictly less than 8 bits. */
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            /* bits_in in [1..7]: top `bits_in` bits must be all 1s. */
            uint64_t expected = ((UINT64_C(1) << bits_in) - 1)
                                << (64 - bits_in);
            uint64_t got      = acc & (((UINT64_C(1) << bits_in) - 1)
                                       << (64 - bits_in));
            if (got != expected) {
                return n00b_result_err(size_t, N00B_QUIC_ERR_PROTOCOL);
            }
            break;
        }
    }
    return n00b_result_ok(size_t, dst_off);
}
