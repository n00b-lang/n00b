#include "text/unicode/encoding.h"
#include <string.h>
#include <stdint.h>
#include <assert.h>
#include "internal/text/unicode/raw.h"
#include "core/alloc.h"

// ---------------------------------------------------------------------------
// Word-at-a-time ASCII detection helpers (c-utf8 / GLib style).
//
// On 64-bit targets sizeof(size_t) == 8, so we check 8 bytes per word.
// A word is all-ASCII iff no byte has its MSB set AND no byte is NUL.
// The classic trick: ((word - ONES) | word) & HIGH tests both conditions.
// ---------------------------------------------------------------------------

#define _N00B_ASCII_ONES ((size_t)0x0101010101010101ULL)
#define _N00B_ASCII_HIGH ((size_t)0x8080808080808080ULL)

#define _n00b_word_is_ascii(w) \
    (((((w) - _N00B_ASCII_ONES) | (w)) & _N00B_ASCII_HIGH) == 0)

#define _n00b_ptr_aligned(p, a) (((uintptr_t)(p) & ((uintptr_t)(a) - 1)) == 0)

// ---------------------------------------------------------------------------
// Multi-byte validation helper (Unicode Table 3-7 exhaustive check).
//
// Validates a multi-byte UTF-8 sequence starting at p without decoding
// the codepoint value.  Returns the byte count consumed (2, 3, or 4),
// or 0 if the sequence is malformed.
// ---------------------------------------------------------------------------

static inline int
_n00b_validate_multibyte(const uint8_t *p, const uint8_t *end)
{
    uint8_t b0 = p[0];

    // 2-byte: C2..DF  80..BF
    // (C0/C1 are overlong encodings of ASCII and are never valid.)
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (n00b_unlikely(p + 2 > end))
            return 0;
        if (n00b_unlikely((p[1] & 0xC0) != 0x80))
            return 0;
        return 2;
    }

    // 3-byte: E0..EF  (continuation bytes must be 80..BF unless noted)
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (n00b_unlikely(p + 3 > end))
            return 0;

        uint8_t b1 = p[1];

        if (b0 == 0xE0) {
            // Overlong guard: second byte must be A0..BF.
            if (n00b_unlikely(b1 < 0xA0 || b1 > 0xBF))
                return 0;
        }
        else if (b0 == 0xED) {
            // Surrogate guard: second byte must be 80..9F.
            if (n00b_unlikely(b1 > 0x9F || b1 < 0x80))
                return 0;
        }
        else {
            // E1..EC, EE..EF: second byte 80..BF.
            if (n00b_unlikely((b1 & 0xC0) != 0x80))
                return 0;
        }

        if (n00b_unlikely((p[2] & 0xC0) != 0x80))
            return 0;
        return 3;
    }

    // 4-byte: F0..F4  (F5..FF are never valid lead bytes.)
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (n00b_unlikely(p + 4 > end))
            return 0;

        uint8_t b1 = p[1];

        if (b0 == 0xF0) {
            // Overlong guard: second byte must be 90..BF.
            if (n00b_unlikely(b1 < 0x90 || b1 > 0xBF))
                return 0;
        }
        else if (b0 == 0xF4) {
            // Max-codepoint guard: second byte must be 80..8F.
            if (n00b_unlikely(b1 > 0x8F || b1 < 0x80))
                return 0;
        }
        else {
            // F1..F3: second byte 80..BF.
            if (n00b_unlikely((b1 & 0xC0) != 0x80))
                return 0;
        }

        if (n00b_unlikely((p[2] & 0xC0) != 0x80))
            return 0;
        if (n00b_unlikely((p[3] & 0xC0) != 0x80))
            return 0;
        return 4;
    }

    // C0, C1, F5..FF, or stray continuation byte (80..BF): invalid.
    return 0;
}

// ===========================================================================
// Lemire/Keiser SIMD UTF-8 validation (Apache 2.0).
//
// Uses GCC/Clang vector_size(16) + __builtin_shuffle which compiles to
// pshufb on x86 and vtbl on ARM — no platform-specific intrinsics needed.
//
// After SIMD validation confirms well-formedness, decode and count
// operations can skip per-codepoint validation checks entirely.
// ===========================================================================

#ifdef N00B_HAVE_VECTOR_BUILTINS

typedef unsigned char _n00b_v16u8 __attribute__((vector_size(16)));

// 4-bit table lookup.
static inline _n00b_v16u8
_n00b_v_lookup(_n00b_v16u8 table, _n00b_v16u8 indices)
{
#ifdef __clang__
    // Clang lacks __builtin_shuffle; element-wise lookup auto-vectorizes
    // to tbl on ARM and pshufb on x86.
    _n00b_v16u8 r;
    for (int _i = 0; _i < 16; _i++) r[_i] = table[indices[_i] & 0x0f];
    return r;
#else
    return __builtin_shuffle(table, indices);
#endif
}

// Shift the 16-byte window one byte left, pulling the last byte of prev.
static inline _n00b_v16u8
_n00b_v_prev1(_n00b_v16u8 cur, _n00b_v16u8 prev)
{
#ifdef __clang__
    _n00b_v16u8 r;
    r[0] = prev[15];
    for (int _i = 1; _i < 16; _i++) r[_i] = cur[_i - 1];
    return r;
#else
    _n00b_v16u8 mask = {15, 16, 17, 18, 19, 20, 21, 22,
                        23, 24, 25, 26, 27, 28, 29, 30};
    return __builtin_shuffle(prev, cur, mask);
#endif
}

// Shift by 2 bytes, pulling prev[14..15].
static inline _n00b_v16u8
_n00b_v_prev2(_n00b_v16u8 cur, _n00b_v16u8 prev)
{
#ifdef __clang__
    _n00b_v16u8 r;
    r[0] = prev[14];
    r[1] = prev[15];
    for (int _i = 2; _i < 16; _i++) r[_i] = cur[_i - 2];
    return r;
#else
    _n00b_v16u8 mask = {14, 15, 16, 17, 18, 19, 20, 21,
                        22, 23, 24, 25, 26, 27, 28, 29};
    return __builtin_shuffle(prev, cur, mask);
#endif
}

// Shift by 3 bytes, pulling prev[13..15].
static inline _n00b_v16u8
_n00b_v_prev3(_n00b_v16u8 cur, _n00b_v16u8 prev)
{
#ifdef __clang__
    _n00b_v16u8 r;
    r[0] = prev[13];
    r[1] = prev[14];
    r[2] = prev[15];
    for (int _i = 3; _i < 16; _i++) r[_i] = cur[_i - 3];
    return r;
#else
    _n00b_v16u8 mask = {13, 14, 15, 16, 17, 18, 19, 20,
                        21, 22, 23, 24, 25, 26, 27, 28};
    return __builtin_shuffle(prev, cur, mask);
#endif
}

// Per-lane unsigned saturating subtraction: max(a - b, 0).
static inline _n00b_v16u8
_n00b_v_sat_sub(_n00b_v16u8 a, _n00b_v16u8 b)
{
    _n00b_v16u8 mask = a > b;
    return (a - b) & mask;
}

static inline bool
_n00b_v_any_nonzero(_n00b_v16u8 v)
{
    uint64_t lo, hi;
    __builtin_memcpy(&lo, &v, 8);
    __builtin_memcpy(&hi, (const char *)&v + 8, 8);
    return (lo | hi) != 0;
}

// ---------------------------------------------------------------------------
// Lemire/Keiser error-class lookup tables.
//
// Each byte position in the 16-byte table is a bitmask of error classes.
// Three lookups (indexed by high nibble of prev byte, low nibble of prev
// byte, high nibble of current byte) are ANDed together.  Non-zero result
// means the byte pair is invalid.
//
// Error classes (simdjson bit assignment):
//   0x01  TOO_SHORT      — lead byte not followed by enough continuations
//   0x02  TOO_LONG       — continuation byte where none expected
//   0x04  OVERLONG_3     — E0 followed by 80-9F
//   0x08  TOO_LARGE      — F4 90+ or F5+ lead
//   0x10  SURROGATE      — ED followed by A0-BF
//   0x20  OVERLONG_2     — C0/C1 lead bytes
//   0x40  TOO_LARGE_1000 — F1+ leads (>= U+10000 helper)
//   0x40  OVERLONG_4     — F0 followed by 80-8F (shares bit)
//   0x80  TWO_CONTS      — continuation after continuation
//
// The 3-way AND of the tables produces a "special cases" result (sc).
// A companion check_multibyte_lengths step XORs bit 7 against a mask of
// positions that SHOULD be continuations (based on lead bytes 2 and 3
// positions back), so that valid continuation chains are cleared while
// structural mismatches are flagged.
// ---------------------------------------------------------------------------

// Error category bits (simdjson / Lemire–Keiser assignment).
#define _LK_TOO_SHORT      0x01  // Lead not followed by continuation
#define _LK_TOO_LONG       0x02  // Continuation not preceded by lead
#define _LK_OVERLONG_3     0x04  // 3-byte overlong (E0 80..9F)
#define _LK_TOO_LARGE      0x08  // > U+10FFFF (F4 90+ or F5+)
#define _LK_SURROGATE      0x10  // Surrogate half (ED A0..BF)
#define _LK_OVERLONG_2     0x20  // 2-byte overlong (C0, C1)
#define _LK_TOO_LARGE_1000 0x40  // >= U+10000 helper (F1+ lead)
#define _LK_OVERLONG_4     0x40  // 4-byte overlong (F0 80..8F) — shares bit
#define _LK_TWO_CONTS      0x80  // Two continuations in a row
#define _LK_CARRY \
    (_LK_TOO_SHORT | _LK_TOO_LONG | _LK_TWO_CONTS)

// Table 1: indexed by high nibble of previous byte.
static const _n00b_v16u8 _n00b_utf8_tab1 = {
    // 0x-7x: ASCII predecessor — continuation after ASCII is TOO_LONG
    _LK_TOO_LONG, _LK_TOO_LONG, _LK_TOO_LONG, _LK_TOO_LONG,
    _LK_TOO_LONG, _LK_TOO_LONG, _LK_TOO_LONG, _LK_TOO_LONG,
    // 8x-Bx: continuation predecessor — two continuations = TWO_CONTS
    _LK_TWO_CONTS, _LK_TWO_CONTS, _LK_TWO_CONTS, _LK_TWO_CONTS,
    // Cx: 2-byte lead, may be overlong (C0/C1)
    _LK_TOO_SHORT | _LK_OVERLONG_2,
    // Dx: 2-byte lead
    _LK_TOO_SHORT,
    // Ex: 3-byte lead, may be overlong or surrogate
    _LK_TOO_SHORT | _LK_OVERLONG_3 | _LK_SURROGATE,
    // Fx: 4-byte lead, may be overlong or too large
    _LK_TOO_SHORT | _LK_TOO_LARGE | _LK_TOO_LARGE_1000 | _LK_OVERLONG_4,
};

// Table 2: indexed by low nibble of previous byte.
// Every entry includes CARRY (TOO_SHORT | TOO_LONG | TWO_CONTS) so that
// structural errors always propagate through the 3-way AND.
static const _n00b_v16u8 _n00b_utf8_tab2 = {
    // x0: E0 = overlong-3, F0 = overlong-4, C0 = overlong-2
    _LK_CARRY | _LK_OVERLONG_3 | _LK_OVERLONG_2 | _LK_OVERLONG_4,
    // x1: C1 = overlong-2
    _LK_CARRY | _LK_OVERLONG_2,
    // x2-x3: no special (CARRY only)
    _LK_CARRY, _LK_CARRY,
    // x4: F4 90+ = too large
    _LK_CARRY | _LK_TOO_LARGE,
    // x5-xC: F5+ leads are too large
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    // xD: ED = surrogate range (ED A0-BF)
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000 | _LK_SURROGATE,
    // xE-xF: no special beyond too-large
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
    _LK_CARRY | _LK_TOO_LARGE | _LK_TOO_LARGE_1000,
};

// Table 3: indexed by high nibble of current byte.
//
// Non-continuation entries (0-7, C-F) carry only TOO_SHORT so that "lead
// followed by non-continuation" is the sole error that fires; all other
// special-case bits are cleared.  Continuation entries (8-B) carry the
// error bits that are COMPATIBLE with that continuation range, so invalid
// special-case combinations survive the AND.
static const _n00b_v16u8 _n00b_utf8_tab3 = {
    // 0x-7x: ASCII (not a continuation)
    _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT,
    _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT,
    // 8x: continuation 80-8F — overlong-3, overlong-4, too-large-1000
    _LK_TOO_LONG | _LK_OVERLONG_2 | _LK_TWO_CONTS
        | _LK_OVERLONG_3 | _LK_TOO_LARGE_1000 | _LK_OVERLONG_4,
    // 9x: continuation 90-9F — too-large (F4 90+), overlong-3
    _LK_TOO_LONG | _LK_OVERLONG_2 | _LK_TWO_CONTS
        | _LK_OVERLONG_3 | _LK_TOO_LARGE,
    // Ax-Bx: continuation A0-BF — surrogate, too-large
    _LK_TOO_LONG | _LK_OVERLONG_2 | _LK_TWO_CONTS
        | _LK_SURROGATE | _LK_TOO_LARGE,
    _LK_TOO_LONG | _LK_OVERLONG_2 | _LK_TWO_CONTS
        | _LK_SURROGATE | _LK_TOO_LARGE,
    // Cx-Fx: lead bytes (not a continuation)
    _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT, _LK_TOO_SHORT,
};

static bool
_n00b_utf8_validate_simd(const uint8_t *data, size_t len)
{
    _n00b_v16u8 prev_input = {};
    _n00b_v16u8 errors     = {};
    _n00b_v16u8 four       = prev_input + 4;

    // Thresholds for check_multibyte_lengths (saturating subtraction).
    // A byte 2 back >= 0xE0 means current must be continuation (3/4-byte).
    // A byte 3 back >= 0xF0 means current must be continuation (4-byte).
    _n00b_v16u8 e0_thr  = prev_input + 0x60; // 0xE0 - 0x80
    _n00b_v16u8 f0_thr  = prev_input + 0x70; // 0xF0 - 0x80
    _n00b_v16u8 bit7    = prev_input + 0x80;

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        _n00b_v16u8 input;
        __builtin_memcpy(&input, data + i, 16);

        // check_special_cases: 3-way AND of lookup tables.
        _n00b_v16u8 prev1   = _n00b_v_prev1(input, prev_input);
        _n00b_v16u8 hi_prev = _n00b_v_lookup(_n00b_utf8_tab1, prev1 >> four);
        _n00b_v16u8 lo_prev = _n00b_v_lookup(_n00b_utf8_tab2, prev1 & 0x0F);
        _n00b_v16u8 hi_cur  = _n00b_v_lookup(_n00b_utf8_tab3, input >> four);
        _n00b_v16u8 sc      = hi_prev & lo_prev & hi_cur;

        // check_multibyte_lengths: XOR expected continuations against the
        // TWO_CONTS bit from the special-cases check.  This clears
        // TWO_CONTS for valid 3/4-byte continuation chains and flags it
        // for unexpected continuation-after-continuation.
        _n00b_v16u8 prev2     = _n00b_v_prev2(input, prev_input);
        _n00b_v16u8 prev3     = _n00b_v_prev3(input, prev_input);
        _n00b_v16u8 is_third  = _n00b_v_sat_sub(prev2, e0_thr);
        _n00b_v16u8 is_fourth = _n00b_v_sat_sub(prev3, f0_thr);
        _n00b_v16u8 must23    = (is_third | is_fourth) & bit7;
        _n00b_v16u8 chunk_errs = must23 ^ sc;

        errors = errors | chunk_errs;
        prev_input = input;
    }

    if (_n00b_v_any_nonzero(errors)) {
        return false;
    }

    // Rewind to the start of the last complete multi-byte sequence so
    // the scalar validator can handle the tail (including any sequence
    // that straddles the SIMD/scalar boundary).
    if (i > 0 && i < len) {
        // Walk backward over continuation bytes (10xxxxxx).
        size_t rewind = i;
        while (rewind > 0 && (data[rewind - 1] & 0xC0) == 0x80) {
            rewind--;
        }
        // One more step back to include the lead byte.
        if (rewind > 0 && data[rewind - 1] >= 0xC0) {
            rewind--;
        }
        // Only rewind if we'd actually be inside a multi-byte sequence.
        if (rewind < i) {
            i = rewind;
        }
    }

    // Validate the tail with the scalar helper.
    const uint8_t *p   = data + i;
    const uint8_t *end = data + len;
    while (p < end) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int adv = _n00b_validate_multibyte(p, end);
        if (n00b_unlikely(adv == 0))
            return false;
        p += adv;
    }

    return true;
}

// Trusted decoder — no validation checks.  Only safe after SIMD validation.
static inline int32_t
_n00b_utf8_decode_trusted(const uint8_t *p, uint32_t *advance)
{
    uint8_t b0 = p[0];

    if (b0 < 0x80) {
        *advance = 1;
        return b0;
    }
    if (b0 < 0xE0) {
        *advance = 2;
        return ((b0 & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if (b0 < 0xF0) {
        *advance = 3;
        return ((b0 & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    *advance = 4;
    return ((b0 & 0x07) << 18) | ((p[1] & 0x3F) << 12)
         | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

// Trusted counter — count non-continuation bytes.  Only safe after SIMD
// validation confirms the buffer is well-formed UTF-8.
static int64_t
_n00b_utf8_count_trusted(const uint8_t *data, size_t len)
{
    const uint8_t *p     = data;
    const uint8_t *end   = data + len;
    int64_t        count = 0;

    // Word-at-a-time ASCII fast path.
    while (p + sizeof(size_t) <= end) {
        size_t w;
        __builtin_memcpy(&w, p, sizeof(size_t));
        if (_n00b_word_is_ascii(w)) {
            count += (int64_t)sizeof(size_t);
            p += sizeof(size_t);
            continue;
        }
        break;
    }

    // Byte-at-a-time: every non-continuation byte starts a codepoint.
    while (p < end) {
        count += ((*p & 0xC0) != 0x80);
        p++;
    }

    return count;
}

#endif // N00B_HAVE_VECTOR_BUILTINS

// ---------------------------------------------------------------------------
// UTF-8 decode: returns codepoint at src[*pos], advances *pos.
// Returns -1 on invalid sequence or if *pos >= len.
// ---------------------------------------------------------------------------

int32_t
n00b_unicode_utf8_decode(const char *src, uint32_t len, uint32_t *pos)
{
    if (*pos >= len) {
        return -1;
    }

    uint8_t b0 = (uint8_t)src[*pos];

    if (b0 < 0x80) {
        (*pos)++;
        return b0;
    }

    n00b_codepoint_t cp;
    uint32_t         need;

    if ((b0 & 0xE0) == 0xC0) {
        cp   = b0 & 0x1F;
        need = 2;
    }
    else if ((b0 & 0xF0) == 0xE0) {
        cp   = b0 & 0x0F;
        need = 3;
    }
    else if ((b0 & 0xF8) == 0xF0) {
        cp   = b0 & 0x07;
        need = 4;
    }
    else {
        (*pos)++;
        return -1; // invalid lead byte
    }

    if (*pos + need > len) {
        (*pos)++;
        return -1; // truncated
    }

    for (uint32_t i = 1; i < need; i++) {
        uint8_t b = (uint8_t)src[*pos + i];
        if ((b & 0xC0) != 0x80) {
            (*pos)++;
            return -1; // bad continuation
        }
        cp = (cp << 6) | (b & 0x3F);
    }

    *pos += need;

    // Overlong check
    if (need == 2 && cp < 0x80)
        return -1;
    if (need == 3 && cp < 0x800)
        return -1;
    if (need == 4 && cp < 0x10000)
        return -1;

    // Surrogates and out-of-range
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return -1;
    if (cp > 0x10FFFF)
        return -1;

    return (int32_t)cp;
}

// ---------------------------------------------------------------------------
// UTF-8 encode: write codepoint to dst, return bytes written (1-4).
// dst must have at least 4 bytes available.
// ---------------------------------------------------------------------------

uint32_t
n00b_unicode_utf8_encode(n00b_codepoint_t cp, char *dst)
{
    if (cp < 0x80) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        dst[0] = (char)(0xC0 | (cp >> 6));
        dst[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        dst[0] = (char)(0xE0 | (cp >> 12));
        dst[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    dst[0] = (char)(0xF0 | (cp >> 18));
    dst[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

// ---------------------------------------------------------------------------
// UTF-8 validation (c-utf8 / GLib style).
//
// Three phases:
//   1. Byte-at-a-time until the pointer is sizeof(size_t)-aligned.
//   2. Word-at-a-time: check two size_t words per iteration (16 bytes
//      on 64-bit) for all-ASCII content.
//   3. Remaining bytes: byte-at-a-time with opportunistic word checks.
//
// Multi-byte sequences are validated via _n00b_validate_multibyte()
// without decoding the codepoint value.
// ---------------------------------------------------------------------------

bool
n00b_unicode_utf8_validate(const char *src, uint32_t len)
{
#ifdef N00B_HAVE_VECTOR_BUILTINS
    if (len >= 32)
        return _n00b_utf8_validate_simd((const uint8_t *)src, len);
#endif

    const uint8_t *p   = (const uint8_t *)src;
    const uint8_t *end = p + len;

    // Phase 1: align.
    while (p < end && !_n00b_ptr_aligned(p, sizeof(size_t))) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int adv = _n00b_validate_multibyte(p, end);
        if (n00b_unlikely(adv == 0))
            return false;
        p += adv;
    }

    // Phase 2: word-at-a-time ASCII fast path.
    while (p + 2 * sizeof(size_t) <= end) {
        size_t w0 = *(const size_t *)(const void *)p;
        size_t w1 = *(const size_t *)(const void *)(p + sizeof(size_t));

        if (_n00b_word_is_ascii(w0) && _n00b_word_is_ascii(w1)) {
            p += 2 * sizeof(size_t);
            continue;
        }
        break;
    }

    // Phase 3: remaining bytes.
    while (p < end) {
        if (*p < 0x80) {
            if (_n00b_ptr_aligned(p, sizeof(size_t))
                && p + sizeof(size_t) <= end) {
                size_t w = *(const size_t *)(const void *)p;
                if (_n00b_word_is_ascii(w)) {
                    p += sizeof(size_t);
                    continue;
                }
            }
            p++;
            continue;
        }
        int adv = _n00b_validate_multibyte(p, end);
        if (n00b_unlikely(adv == 0))
            return false;
        p += adv;
    }

    return true;
}

bool
n00b_unicode_str_validate(n00b_string_t s)
{
    return n00b_unicode_utf8_validate(s.data, (uint32_t)s.u8_bytes);
}

// ---------------------------------------------------------------------------
// Count codepoints (c-utf8 style, no full decode).
//
// Same three-phase structure as validation.  Each ASCII byte is one
// codepoint; each multi-byte sequence (validated inline) is one codepoint.
// Returns -1 on invalid UTF-8 (preserving the existing API contract).
// ---------------------------------------------------------------------------

int64_t
n00b_unicode_utf8_count_codepoints_raw(const char *src, uint32_t len)
{
#ifdef N00B_HAVE_VECTOR_BUILTINS
    if (len >= 32) {
        if (!_n00b_utf8_validate_simd((const uint8_t *)src, len))
            return -1;
        return _n00b_utf8_count_trusted((const uint8_t *)src, len);
    }
#endif

    const uint8_t *p     = (const uint8_t *)src;
    const uint8_t *end   = p + len;
    int64_t        count = 0;

    // Phase 1: align.
    while (p < end && !_n00b_ptr_aligned(p, sizeof(size_t))) {
        if (*p < 0x80) {
            p++;
            count++;
            continue;
        }
        int adv = _n00b_validate_multibyte(p, end);
        if (n00b_unlikely(adv == 0))
            return -1;
        p += adv;
        count++;
    }

    // Phase 2: word-at-a-time ASCII fast path.
    while (p + 2 * sizeof(size_t) <= end) {
        size_t w0 = *(const size_t *)(const void *)p;
        size_t w1 = *(const size_t *)(const void *)(p + sizeof(size_t));

        if (_n00b_word_is_ascii(w0) && _n00b_word_is_ascii(w1)) {
            count += 2 * (int64_t)sizeof(size_t);
            p += 2 * sizeof(size_t);
            continue;
        }
        break;
    }

    // Phase 3: remaining bytes.
    while (p < end) {
        if (*p < 0x80) {
            if (_n00b_ptr_aligned(p, sizeof(size_t))
                && p + sizeof(size_t) <= end) {
                size_t w = *(const size_t *)(const void *)p;
                if (_n00b_word_is_ascii(w)) {
                    count += (int64_t)sizeof(size_t);
                    p += sizeof(size_t);
                    continue;
                }
            }
            p++;
            count++;
            continue;
        }
        int adv = _n00b_validate_multibyte(p, end);
        if (n00b_unlikely(adv == 0))
            return -1;
        p += adv;
        count++;
    }

    return count;
}

int64_t
n00b_unicode_utf8_count_codepoints(n00b_string_t s)
{
    return n00b_unicode_utf8_count_codepoints_raw(s.data, (uint32_t)s.u8_bytes);
}

// ---------------------------------------------------------------------------
// UTF-16 conversion (single-pass).
//
// Allocates a worst-case buffer (len + 1 uint16_t elements) and decodes
// in one pass.  The previous implementation did a count pre-pass followed
// by a decode pass; this eliminates the redundant first pass.
// ---------------------------------------------------------------------------

static uint16_t *
n00b_unicode_to_utf16_raw(const char       *data,
                          int64_t           len,
                          uint32_t         *out_len,
                          n00b_allocator_t *allocator)
{
    uint16_t *buf      = n00b_alloc_array_with_opts(uint16_t,
                                                   (size_t)len + 1,
                                                   &(n00b_alloc_opts_t){.allocator = allocator});
    uint32_t  idx      = 0;
    uint32_t  byte_len = (uint32_t)len;

#ifdef N00B_HAVE_VECTOR_BUILTINS
    if (byte_len >= 32
        && _n00b_utf8_validate_simd((const uint8_t *)data, byte_len)) {
        const uint8_t *p   = (const uint8_t *)data;
        const uint8_t *end = p + byte_len;
        while (p < end) {
            uint32_t adv;
            int32_t  cp = _n00b_utf8_decode_trusted(p, &adv);
            if (cp < 0x10000) {
                buf[idx++] = (uint16_t)cp;
            }
            else {
                cp -= 0x10000;
                buf[idx++] = (uint16_t)(0xD800 + (cp >> 10));
                buf[idx++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            }
            p += adv;
        }
        if (out_len)
            *out_len = idx;
        return buf;
    }
#endif

    uint32_t pos = 0;
    while (pos < byte_len) {
        int32_t cp = n00b_unicode_utf8_decode(data, byte_len, &pos);
        if (cp < 0)
            break;

        if (cp < 0x10000) {
            buf[idx++] = (uint16_t)cp;
        }
        else {
            cp -= 0x10000;
            buf[idx++] = (uint16_t)(0xD800 + (cp >> 10));
            buf[idx++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        }
    }

    if (out_len)
        *out_len = idx;
    return buf;
}

n00b_array_t(uint16_t)
n00b_unicode_to_utf16(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;
    uint32_t  count = 0;
    uint16_t *raw   = n00b_unicode_to_utf16_raw(s.data, s.u8_bytes, &count, allocator);
    n00b_array_t(uint16_t) result = n00b_array_checked_ptr(uint16_t,
                                                            (size_t)s.u8_bytes + 1,
                                                            raw);
    result.len = count;
    return result;
}

// ---------------------------------------------------------------------------
// UTF-32 conversion (single-pass).
// ---------------------------------------------------------------------------

static n00b_codepoint_t *
n00b_unicode_to_utf32_raw(const char       *data,
                          int64_t           len,
                          uint32_t         *out_len,
                          n00b_allocator_t *allocator)
{
    n00b_codepoint_t *buf = n00b_alloc_array_with_opts(n00b_codepoint_t,
                                                      (size_t)len + 1,
                                                      &(n00b_alloc_opts_t){.allocator = allocator});
    uint32_t idx      = 0;
    uint32_t byte_len = (uint32_t)len;

#ifdef N00B_HAVE_VECTOR_BUILTINS
    if (byte_len >= 32
        && _n00b_utf8_validate_simd((const uint8_t *)data, byte_len)) {
        const uint8_t *p   = (const uint8_t *)data;
        const uint8_t *end = p + byte_len;
        while (p < end) {
            uint32_t adv;
            int32_t  cp = _n00b_utf8_decode_trusted(p, &adv);
            buf[idx++]  = (n00b_codepoint_t)cp;
            p += adv;
        }
        if (out_len)
            *out_len = idx;
        return buf;
    }
#endif

    uint32_t pos = 0;
    while (pos < byte_len) {
        int32_t cp = n00b_unicode_utf8_decode(data, byte_len, &pos);
        if (cp < 0)
            break;
        buf[idx++] = (n00b_codepoint_t)cp;
    }

    if (out_len)
        *out_len = idx;
    return buf;
}

n00b_array_t(n00b_codepoint_t)
n00b_unicode_to_utf32(n00b_string_t s) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (!allocator)
        allocator = nullptr;
    uint32_t          count = 0;
    n00b_codepoint_t *raw   = n00b_unicode_to_utf32_raw(s.data, s.u8_bytes, &count, allocator);
    n00b_array_t(n00b_codepoint_t) result = n00b_array_checked_ptr(n00b_codepoint_t,
                                                                    (size_t)s.u8_bytes + 1,
                                                                    raw);
    result.len = count;
    return result;
}

n00b_string_t
n00b_unicode_from_utf16(const uint16_t *src, uint32_t len) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    assert(!len || src);

    if (!allocator)
        allocator = nullptr;

    // Worst case: each code unit -> 4 UTF-8 bytes
    char    *buf = n00b_alloc_array(char, (size_t)len * 4 + 1);
    uint32_t pos = 0;

    for (uint32_t i = 0; i < len; i++) {
        n00b_codepoint_t cp;
        if (src[i] >= 0xD800 && src[i] <= 0xDBFF && i + 1 < len && src[i + 1] >= 0xDC00
            && src[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((n00b_codepoint_t)(src[i] - 0xD800) << 10) + (src[i + 1] - 0xDC00);
            i++;
        }
        else {
            cp = src[i];
        }
        pos += n00b_unicode_utf8_encode(cp, buf + pos);
    }

    buf[pos] = '\0';

    n00b_string_t result = n00b_string_from_raw(buf, pos, .allocator = allocator);
    n00b_free(buf);
    return result;
}

n00b_string_t
n00b_unicode_from_utf32(const n00b_codepoint_t *src, uint32_t len) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    assert(!len || src);

    if (!allocator)
        allocator = nullptr;

    char    *buf = n00b_alloc_array(char, (size_t)len * 4 + 1);
    uint32_t pos = 0;

    for (uint32_t i = 0; i < len; i++) {
        pos += n00b_unicode_utf8_encode(src[i], buf + pos);
    }

    buf[pos] = '\0';

    n00b_string_t result = n00b_string_from_raw(buf, pos, .allocator = allocator);
    n00b_free(buf);
    return result;
}

// ---------------------------------------------------------------------------
// BOM detection
// ---------------------------------------------------------------------------

n00b_unicode_bom_t
n00b_unicode_detect_bom(const char *data, uint32_t len, uint32_t *bom_len)
{
    const uint8_t *d = (const uint8_t *)data;

    if (len >= 4 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0xFE && d[3] == 0xFF) {
        if (bom_len)
            *bom_len = 4;
        return N00B_UNICODE_BOM_UTF32_BE;
    }
    if (len >= 4 && d[0] == 0xFF && d[1] == 0xFE && d[2] == 0x00 && d[3] == 0x00) {
        if (bom_len)
            *bom_len = 4;
        return N00B_UNICODE_BOM_UTF32_LE;
    }
    if (len >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
        if (bom_len)
            *bom_len = 3;
        return N00B_UNICODE_BOM_UTF8;
    }
    if (len >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
        if (bom_len)
            *bom_len = 2;
        return N00B_UNICODE_BOM_UTF16_BE;
    }
    if (len >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        if (bom_len)
            *bom_len = 2;
        return N00B_UNICODE_BOM_UTF16_LE;
    }

    if (bom_len)
        *bom_len = 0;
    return N00B_UNICODE_BOM_NONE;
}
