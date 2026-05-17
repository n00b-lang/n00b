// WebAssembly SIMD (simd128) backend.
//
// Phase 10 port from resharp-c/src/simd/wasm.c.  This file is carried into
// the n00b tree for completeness but is NOT compiled by the default meson
// configuration — `wasm.c` stays commented out in
// `src/util/simd/meson.build`.  The whole TU is wrapped in
// `#if defined(__wasm_simd128__)` so even an accidental compile against a
// non-wasm32 target reduces to an empty translation unit.
//
// External symbol renames pending until WASM target detection lands;
// kept verbatim with resharp-c's "BYTE_FREQ" etc. so the diff stays
// minimal.  When the orchestrator turns this file on, it should run a
// pass equivalent to the Phase 10 rename map applied to mod.c / neon.c.
//
// NOTE: Rust const generics like `<const N: usize>` and
// `<const COLLECT_ALL: bool>` were monomorphized at compile time. In C we
// pass the discriminant as a runtime parameter; the compiler is expected to
// inline & specialize via `[[gnu::always_inline]]` on hot helpers. This
// changes generated code shape but preserves behavior.
// NOTE: SENTINEL = SIZE_MAX is used in place of Rust `Option<usize>::None`
// for the "not found" return value.

#if defined(__wasm_simd128__)

#include "util/simd/wasm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <wasm_simd128.h>

#define SENTINEL ((size_t)SIZE_MAX)

// ---------- MatchVec ----------
// Growth uses ckd_mul_sz for both `cap*2` and `cap*sizeof(MatchPair)` (so neither
// can wrap silently), and xrealloc_into for the buffer (abort-on-OOM, and the
// classic `p = realloc(p, n)` leak-on-NULL anti-pattern is avoided because
// xrealloc_into only writes back on success).

static void matchvec_push(MatchVec *v, MatchPair m) {
    if (v->len == v->cap) {
        size_t nc = v->cap ? ckd_mul_sz(v->cap, 2) : 8;
        size_t bytes = ckd_mul_sz(nc, sizeof(MatchPair));
        xrealloc_into((void **)&v->data, bytes);
        v->cap = nc;
    }
    v->data[v->len++] = m;
}

// ---------- count-trailing/leading-zeros for u32/u16 ----------

[[gnu::always_inline]] static inline unsigned ctz_u32(uint32_t x) {
    return (unsigned)__builtin_ctz(x);
}

[[gnu::always_inline]] static inline unsigned ctz_u16(uint16_t x) {
    return (unsigned)__builtin_ctz((uint32_t)x);
}

[[gnu::always_inline]] static inline unsigned clz_u16(uint16_t x) {
    // Rust's u16::leading_zeros: count of leading zero bits (0..=16).
    if (x == 0) return 16u;
    return (unsigned)__builtin_clz((uint32_t)x) - 16u;
}

#ifdef __wasm_simd128__

// ---------- teddy_chunk / teddy_chunk_rev ----------

[[gnu::always_inline]] static inline v128_t teddy_chunk(
    size_t N,
    const uint8_t *ptr,
    size_t pos,
    const size_t offsets[3],
    const v128_t masks_lo[3],
    const v128_t masks_hi[3],
    v128_t nib)
{
    v128_t l0 = wasm_v128_load((const v128_t *)(ptr + pos + offsets[0]));
    v128_t r = wasm_v128_and(
        wasm_i8x16_swizzle(masks_lo[0], wasm_v128_and(l0, nib)),
        wasm_i8x16_swizzle(masks_hi[0], wasm_u8x16_shr(l0, 4)));
    if (N >= 2) {
        v128_t l1 = wasm_v128_load((const v128_t *)(ptr + pos + offsets[1]));
        v128_t lk = wasm_v128_and(
            wasm_i8x16_swizzle(masks_lo[1], wasm_v128_and(l1, nib)),
            wasm_i8x16_swizzle(masks_hi[1], wasm_u8x16_shr(l1, 4)));
        r = wasm_v128_and(r, lk);
    }
    if (N >= 3) {
        v128_t l2 = wasm_v128_load((const v128_t *)(ptr + pos + offsets[2]));
        v128_t lk = wasm_v128_and(
            wasm_i8x16_swizzle(masks_lo[2], wasm_v128_and(l2, nib)),
            wasm_i8x16_swizzle(masks_hi[2], wasm_u8x16_shr(l2, 4)));
        r = wasm_v128_and(r, lk);
    }
    return r;
}

[[gnu::always_inline]] static inline v128_t teddy_chunk_rev(
    size_t N,
    const uint8_t *ptr,
    size_t chunk_pos,
    const v128_t masks_lo[3],
    const v128_t masks_hi[3],
    v128_t nib)
{
    v128_t l0 = wasm_v128_load((const v128_t *)(ptr + chunk_pos - 0));
    v128_t r = wasm_v128_and(
        wasm_i8x16_swizzle(masks_lo[0], wasm_v128_and(l0, nib)),
        wasm_i8x16_swizzle(masks_hi[0], wasm_u8x16_shr(l0, 4)));
    if (N >= 2) {
        v128_t l1 = wasm_v128_load((const v128_t *)(ptr + chunk_pos - 1));
        v128_t lk = wasm_v128_and(
            wasm_i8x16_swizzle(masks_lo[1], wasm_v128_and(l1, nib)),
            wasm_i8x16_swizzle(masks_hi[1], wasm_u8x16_shr(l1, 4)));
        r = wasm_v128_and(r, lk);
    }
    if (N >= 3) {
        v128_t l2 = wasm_v128_load((const v128_t *)(ptr + chunk_pos - 2));
        v128_t lk = wasm_v128_and(
            wasm_i8x16_swizzle(masks_lo[2], wasm_v128_and(l2, nib)),
            wasm_i8x16_swizzle(masks_hi[2], wasm_u8x16_shr(l2, 4)));
        r = wasm_v128_and(r, lk);
    }
    return r;
}

// ---------- scan_chunk_bytes / scan_chunk_ranges ----------

[[gnu::always_inline]] static inline v128_t scan_chunk_bytes(
    size_t N, v128_t chunk, const v128_t v[3])
{
    v128_t c0 = wasm_i8x16_eq(chunk, v[0]);
    if (N >= 3) {
        return wasm_v128_or(c0,
            wasm_v128_or(wasm_i8x16_eq(chunk, v[1]), wasm_i8x16_eq(chunk, v[2])));
    } else if (N >= 2) {
        return wasm_v128_or(c0, wasm_i8x16_eq(chunk, v[1]));
    } else {
        return c0;
    }
}

[[gnu::always_inline]] static inline v128_t scan_chunk_ranges(
    size_t N, v128_t chunk, const v128_t lo[4], const v128_t hi[4])
{
    v128_t in0 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[0]), wasm_u8x16_le(chunk, hi[0]));
    if (N >= 4) {
        v128_t in1 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[1]), wasm_u8x16_le(chunk, hi[1]));
        v128_t in2 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[2]), wasm_u8x16_le(chunk, hi[2]));
        v128_t in3 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[3]), wasm_u8x16_le(chunk, hi[3]));
        return wasm_v128_or(wasm_v128_or(in0, in1), wasm_v128_or(in2, in3));
    } else if (N >= 3) {
        v128_t in1 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[1]), wasm_u8x16_le(chunk, hi[1]));
        v128_t in2 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[2]), wasm_u8x16_le(chunk, hi[2]));
        return wasm_v128_or(in0, wasm_v128_or(in1, in2));
    } else if (N >= 2) {
        v128_t in1 = wasm_v128_and(wasm_u8x16_ge(chunk, lo[1]), wasm_u8x16_le(chunk, hi[1]));
        return wasm_v128_or(in0, in1);
    } else {
        return in0;
    }
}

// ---------- linear_scan ----------
// NOTE: Rust used a closure `compute: impl FnMut(v128) -> v128`. Here we
// dispatch via two enums (kind = bytes/ranges) and an N parameter, passing
// the precomputed splat arrays.

typedef enum LSKind { LS_BYTES, LS_RANGES } LSKind;

[[gnu::always_inline]] static inline v128_t ls_compute(
    LSKind kind, size_t N, v128_t chunk,
    const v128_t a[4], const v128_t b[4])
{
    if (kind == LS_BYTES) return scan_chunk_bytes(N, chunk, a);
    return scan_chunk_ranges(N, chunk, a, b);
}

static size_t linear_scan(
    bool FWD,
    LSKind kind, size_t N,
    const v128_t a[4], const v128_t b[4],
    const uint8_t *haystack, size_t len)
{
    if (len == 0) return SENTINEL;
    const uint8_t *ptr = haystack;
    if (FWD) {
        size_t pos = 0;
        while (pos + 16 <= len) {
            v128_t combined = ls_compute(kind, N,
                wasm_v128_load((const v128_t *)(ptr + pos)), a, b);
            if (wasm_v128_any_true(combined)) {
                uint16_t mask = (uint16_t)wasm_i8x16_bitmask(combined);
                return pos + (size_t)ctz_u16(mask);
            }
            pos += 16;
        }
        if (pos < len) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack + pos, len - pos);
            v128_t combined = ls_compute(kind, N,
                wasm_v128_load((const v128_t *)buf), a, b);
            uint16_t mask = (uint16_t)wasm_i8x16_bitmask(combined);
            mask &= (uint16_t)((1u << (len - pos)) - 1u);
            if (mask != 0) {
                return pos + (size_t)ctz_u16(mask);
            }
        }
    } else {
        if (len >= 16) {
            size_t pos = len - 16;
            for (;;) {
                v128_t combined = ls_compute(kind, N,
                    wasm_v128_load((const v128_t *)(ptr + pos)), a, b);
                if (wasm_v128_any_true(combined)) {
                    uint16_t mask = (uint16_t)wasm_i8x16_bitmask(combined);
                    return pos + 15 - (size_t)clz_u16(mask);
                }
                if (pos < 16) break;
                pos -= 16;
            }
        }
        size_t gap = (len >= 16) ? (len % 16) : len;
        if (gap > 0) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack, gap);
            v128_t combined = ls_compute(kind, N,
                wasm_v128_load((const v128_t *)buf), a, b);
            uint16_t mask = (uint16_t)wasm_i8x16_bitmask(combined);
            mask &= (uint16_t)((1u << gap) - 1u);
            if (mask != 0) {
                return 15 - (size_t)clz_u16(mask);
            }
        }
    }
    return SENTINEL;
}

#endif // __wasm_simd128__

// ============================================================
// FwdLiteralSearch
// ============================================================

size_t FwdLiteralSearch_len(const FwdLiteralSearch *self) {
    return self->needle_len;
}

uint8_t FwdLiteralSearch_rare_byte(const FwdLiteralSearch *self) {
    return self->rare_byte;
}

FwdLiteralSearch FwdLiteralSearch_new(const uint8_t *needle, size_t needle_len) {
    // debug_assert!(!needle.is_empty());
    size_t rare_idx = 0;
    uint32_t rare_freq = BYTE_FREQ[needle[0]];
    for (size_t i = 1; i < needle_len; ++i) {
        uint32_t f = BYTE_FREQ[needle[i]];
        if (f < rare_freq) {
            rare_freq = f;
            rare_idx = i;
        }
    }
    size_t confirm_idx;
    if (needle_len > 1) {
        size_t ci = (rare_idx == 0) ? 1 : 0;
        uint32_t cf = BYTE_FREQ[needle[ci]];
        for (size_t i = 0; i < needle_len; ++i) {
            if (i == rare_idx) continue;
            uint32_t f = BYTE_FREQ[needle[i]];
            if (f < cf) {
                cf = f;
                ci = i;
            }
        }
        confirm_idx = ci;
    } else {
        confirm_idx = 0;
    }

    size_t cap = (needle_len + 7) / 8;
    // xcalloc: count*size is overflow-checked via ckd_mul; if the multiply
    // overflows or the alloc fails, we PANIC instead of returning a wrapped
    // size or NULL. (Reachable callers clamp needle_len well below SIZE_MAX/8,
    // so this is mechanical hardening.)
    uint64_t *chunks = (uint64_t *)xcalloc(cap, sizeof(uint64_t));
    size_t chunks_len = 0;

    size_t i = 0;
    while (i + 8 <= needle_len) {
        uint8_t v[8];
        memcpy(v, needle + i, 8);
        uint64_t u;
        memcpy(&u, v, 8); // u64::from_ne_bytes
        chunks[chunks_len++] = u;
        i += 8;
    }
    if (i < needle_len) {
        uint8_t v[8] = {0};
        memcpy(v, needle + i, needle_len - i);
        uint64_t u;
        memcpy(&u, v, 8);
        chunks[chunks_len++] = u;
    }

    uint8_t *needle_owned = (uint8_t *)xmalloc(needle_len);
    memcpy(needle_owned, needle, needle_len);

    return (FwdLiteralSearch){
        .needle = needle_owned,
        .needle_len = needle_len,
        .chunks = chunks,
        .chunks_len = chunks_len,
        .rare_idx = rare_idx,
        .rare_byte = needle[rare_idx],
        .confirm_idx = confirm_idx,
        .confirm_byte = needle[confirm_idx],
    };
}

void FwdLiteralSearch_free(FwdLiteralSearch *self) {
    xfree(self->needle);
    xfree(self->chunks);
    *self = (FwdLiteralSearch){0};
}

[[gnu::always_inline]] static inline bool FwdLiteralSearch_verify(
    const FwdLiteralSearch *self, const uint8_t *haystack, size_t start)
{
    size_t n = self->needle_len;
    const uint8_t *hp = haystack + start;
    size_t ci = 0;
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t h;
        memcpy(&h, hp + off, 8); // read_unaligned
        if (h != self->chunks[ci]) return false;
        ci += 1;
        off += 8;
    }
    if (off < n) {
        // H1 fix (mirror neon.c::fls_verify and the constructor's tail handling
        // at lines 291-297): the previous `memcpy(&h, hp + off, 8)` always read
        // 8 bytes even when only `n - off` (1..7) needle bytes remain. Caller
        // only guarantees `start + nlen <= hlen`, so on a candidate at the very
        // end of the haystack this read up to 7 bytes past `haystack[hlen-1]`.
        // On wasm32 linear memory that load can cross a page boundary and trap;
        // on host C it pulls adjacent (possibly unmapped) bytes into the
        // equality check. Copy only the remaining bytes into a zero-padded
        // 8-byte stack buffer first, then load 8 bytes from that.
        REQUIRE(n - off <= 7, "FwdLiteralSearch_verify: tail loop invariant");
        uint8_t v[8] = {0};
        memcpy(v, hp + off, n - off);
        uint64_t h;
        memcpy(&h, v, 8);
        // Mask is now redundant (the buffer is zero-padded so high bytes of
        // both `h` and `self->chunks[ci]` are zero by construction), but keep
        // it for defense-in-depth and parity with neon.c.
        uint64_t mask = ((uint64_t)1 << ((n - off) * 8)) - (uint64_t)1;
        if (((h ^ self->chunks[ci]) & mask) != 0) return false;
    }
    return true;
}

// Common scan core. COLLECT_ALL=false returns first match start (or SENTINEL),
// COLLECT_ALL=true pushes every non-overlapping match into `matches` and
// returns SENTINEL.
static size_t FwdLiteralSearch_scan(
    const FwdLiteralSearch *self,
    bool COLLECT_ALL,
    const uint8_t *haystack, size_t hlen,
    MatchVec *matches)
{
    size_t nlen = self->needle_len;
    if (hlen < nlen) return SENTINEL;
#ifdef __wasm_simd128__
    const uint8_t *ptr = haystack;
    size_t rare_idx = self->rare_idx;
    uint8_t rare_byte = self->rare_byte;
    size_t confirm_idx = self->confirm_idx;
    uint8_t confirm_byte = self->confirm_byte;
    size_t end = hlen - nlen + rare_idx;
    v128_t vrare = wasm_u8x16_splat(rare_byte);
    size_t last_end = 0;

    // Inline `handle` closure as a lambda-style helper via macro-free local function.
    // It returns SENTINEL to mean "no match here", or a start index for first-match mode.
    #define HANDLE(start_, out_)                                                      \
        do {                                                                          \
            size_t _s = (start_);                                                     \
            out_ = SENTINEL;                                                          \
            if (COLLECT_ALL && _s < last_end) break;                                  \
            if (ptr[_s + confirm_idx] != confirm_byte                                 \
                || !FwdLiteralSearch_verify(self, haystack, _s)) break;               \
            if (COLLECT_ALL) {                                                        \
                size_t _me = _s + nlen;                                               \
                matchvec_push(matches, (MatchPair){_s, _me});                         \
                last_end = _me;                                                       \
            } else {                                                                  \
                out_ = _s;                                                            \
            }                                                                         \
        } while (0)

    size_t pos = rare_idx;
    while (pos + 16 <= end + 1) {
        v128_t chunk = wasm_v128_load((const v128_t *)(ptr + pos));
        uint32_t mask = (uint32_t)(uint16_t)wasm_i8x16_bitmask(wasm_i8x16_eq(chunk, vrare));
        while (mask != 0) {
            unsigned bit = ctz_u32(mask);
            size_t start = pos + (size_t)bit - rare_idx;
            size_t res;
            HANDLE(start, res);
            if (!COLLECT_ALL && res != SENTINEL) return res;
            mask &= mask - 1u;
        }
        pos += 16;
    }
    while (pos <= end) {
        if (ptr[pos] == rare_byte) {
            size_t start = pos - rare_idx;
            size_t res;
            HANDLE(start, res);
            if (!COLLECT_ALL && res != SENTINEL) return res;
        }
        pos += 1;
    }
    #undef HANDLE
#else
    (void)matches; (void)haystack;
#endif
    return SENTINEL;
}

size_t FwdLiteralSearch_find_fwd(const FwdLiteralSearch *self,
                                 const uint8_t *haystack, size_t hlen)
{
    MatchVec sink = {0};
    size_t r = FwdLiteralSearch_scan(self, false, haystack, hlen, &sink);
    xfree(sink.data);
    return r;
}

void FwdLiteralSearch_find_all_fixed(const FwdLiteralSearch *self,
                                     const uint8_t *haystack, size_t hlen,
                                     MatchVec *matches)
{
    (void)FwdLiteralSearch_scan(self, true, haystack, hlen, matches);
}

// ============================================================
// RevSearchBytes
// ============================================================

RevSearchBytes RevSearchBytes_new(const uint8_t *bytes, size_t bytes_len) {
    // debug_assert!(!bytes.is_empty() && bytes.len() <= 3);
    uint8_t *owned = (uint8_t *)xmalloc(bytes_len);
    memcpy(owned, bytes, bytes_len);
    return (RevSearchBytes){ .bytes = owned, .bytes_len = bytes_len };
}

void RevSearchBytes_free(RevSearchBytes *self) {
    xfree(self->bytes);
    *self = (RevSearchBytes){0};
}

const uint8_t *RevSearchBytes_bytes(const RevSearchBytes *self, size_t *out_len) {
    if (out_len) *out_len = self->bytes_len;
    return self->bytes;
}

static size_t RevSearchBytes_search(const RevSearchBytes *self, bool FWD,
                                    const uint8_t *haystack, size_t hlen)
{
#ifdef __wasm_simd128__
    size_t n = self->bytes_len;
    v128_t v[3] = {
        wasm_u8x16_splat(self->bytes[0]),
        wasm_u8x16_splat(self->bytes[(n >= 2) ? 1 : 0]),
        wasm_u8x16_splat(self->bytes[(n >= 3) ? 2 : 0]),
    };
    size_t N = (n == 1) ? 1 : (n == 2) ? 2 : 3;
    return linear_scan(FWD, LS_BYTES, N, v, nullptr, haystack, hlen);
#else
    (void)self; (void)FWD; (void)haystack; (void)hlen;
    return SENTINEL;
#endif
}

size_t RevSearchBytes_find_fwd(const RevSearchBytes *self,
                               const uint8_t *haystack, size_t hlen) {
    return RevSearchBytes_search(self, true, haystack, hlen);
}

size_t RevSearchBytes_find_rev(const RevSearchBytes *self,
                               const uint8_t *haystack, size_t hlen) {
    return RevSearchBytes_search(self, false, haystack, hlen);
}

// ============================================================
// RevSearchRanges
// ============================================================

RevSearchRanges RevSearchRanges_new(const U8Pair *ranges, size_t ranges_len) {
    REQUIRE(ranges_len != 0 && ranges_len <= 4,
            "RevSearchRanges_new: ranges_len must be in 1..=4");
    // ckd_mul_sz: ranges_len * sizeof(U8Pair) is overflow-checked. The size
    // is reused for both alloc and copy so we compute it once.
    size_t bytes = ckd_mul_sz(ranges_len, sizeof(U8Pair));
    U8Pair *owned = (U8Pair *)xmalloc(bytes);
    memcpy(owned, ranges, bytes);
    return (RevSearchRanges){ .ranges = owned, .ranges_len = ranges_len };
}

void RevSearchRanges_free(RevSearchRanges *self) {
    xfree(self->ranges);
    *self = (RevSearchRanges){0};
}

const U8Pair *RevSearchRanges_ranges(const RevSearchRanges *self, size_t *out_len) {
    if (out_len) *out_len = self->ranges_len;
    return self->ranges;
}

static size_t RevSearchRanges_search(const RevSearchRanges *self, bool FWD,
                                     const uint8_t *haystack, size_t hlen)
{
#ifdef __wasm_simd128__
    size_t n = self->ranges_len;
    v128_t lo[4] = {
        wasm_u8x16_splat(self->ranges[0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 2) ? 1 : 0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 3) ? 2 : 0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 4) ? 3 : 0].lo),
    };
    v128_t hi[4] = {
        wasm_u8x16_splat(self->ranges[0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 2) ? 1 : 0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 3) ? 2 : 0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 4) ? 3 : 0].hi),
    };
    size_t N = (n == 1) ? 1 : (n == 2) ? 2 : (n == 3) ? 3 : 4;
    return linear_scan(FWD, LS_RANGES, N, lo, hi, haystack, hlen);
#else
    (void)self; (void)FWD; (void)haystack; (void)hlen;
    return SENTINEL;
#endif
}

size_t RevSearchRanges_find_fwd(const RevSearchRanges *self,
                                const uint8_t *haystack, size_t hlen) {
    return RevSearchRanges_search(self, true, haystack, hlen);
}

size_t RevSearchRanges_find_rev(const RevSearchRanges *self,
                                const uint8_t *haystack, size_t hlen) {
    return RevSearchRanges_search(self, false, haystack, hlen);
}

// ============================================================
// FwdRangeSearch
// ============================================================

FwdRangeSearch FwdRangeSearch_new(size_t len, size_t anchor_pos,
                                  U8Pair *ranges, size_t ranges_len,
                                  TSet *sets, size_t sets_len)
{
    // debug_assert!(!ranges.is_empty() && ranges.len() <= 3);
    // debug_assert!(anchor_pos < len);
    return (FwdRangeSearch){
        .len = len,
        .anchor_pos = anchor_pos,
        .ranges = ranges,
        .ranges_len = ranges_len,
        .sets = sets,
        .sets_len = sets_len,
    };
}

void FwdRangeSearch_free(FwdRangeSearch *self) {
    xfree(self->ranges);
    xfree(self->sets);
    *self = (FwdRangeSearch){0};
}

size_t FwdRangeSearch_len(const FwdRangeSearch *self) { return self->len; }

static size_t FwdRangeSearch_verify_tail_fwd(const FwdRangeSearch *self,
                                             const uint8_t *haystack, size_t hlen,
                                             size_t start)
{
    if (hlen < self->len) return SENTINEL;
    size_t end = hlen - self->len;
    size_t pos = start;
    while (pos <= end) {
        bool ok = true;
        for (size_t i = 0; i < self->len; ++i) {
            if (!TSet_contains_byte(&self->sets[i], haystack[pos + i])) {
                ok = false;
                break;
            }
        }
        if (ok) return pos;
        pos += 1;
    }
    return SENTINEL;
}

size_t FwdRangeSearch_find_fwd(const FwdRangeSearch *self,
                               const uint8_t *haystack, size_t hlen,
                               size_t start)
{
#ifdef __wasm_simd128__
    const uint8_t *ptr = haystack;
    size_t n = self->ranges_len;
    size_t anchor = self->anchor_pos;
    v128_t lo[4] = {
        wasm_u8x16_splat(self->ranges[0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 2) ? 1 : 0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 3) ? 2 : 0].lo),
        wasm_u8x16_splat(self->ranges[(n >= 4) ? 3 : 0].lo),
    };
    v128_t hi[4] = {
        wasm_u8x16_splat(self->ranges[0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 2) ? 1 : 0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 3) ? 2 : 0].hi),
        wasm_u8x16_splat(self->ranges[(n >= 4) ? 3 : 0].hi),
    };

    // saturating_sub
    size_t needed = 15 + self->len - 1;
    size_t simd_end = (hlen > needed) ? (hlen - needed) : 0;
    size_t pos = start;
    while (pos < simd_end) {
        v128_t chunk = wasm_v128_load((const v128_t *)(ptr + pos + anchor));
        v128_t combined;
        if (n == 1)      combined = scan_chunk_ranges(1, chunk, lo, hi);
        else if (n == 2) combined = scan_chunk_ranges(2, chunk, lo, hi);
        else if (n == 3) combined = scan_chunk_ranges(3, chunk, lo, hi);
        else             combined = scan_chunk_ranges(4, chunk, lo, hi);
        uint32_t mask = (uint32_t)(uint16_t)wasm_i8x16_bitmask(combined);
        while (mask != 0) {
            unsigned bit = ctz_u32(mask);
            size_t candidate = pos + (size_t)bit;
            bool ok = true;
            for (size_t i = 0; i < self->len; ++i) {
                if (!TSet_contains_byte(&self->sets[i], ptr[candidate + i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return candidate;
            mask &= mask - 1u;
        }
        pos += 16;
    }
    return FwdRangeSearch_verify_tail_fwd(self, haystack, hlen, pos);
#else
    return FwdRangeSearch_verify_tail_fwd(self, haystack, hlen, start);
#endif
}

// ============================================================
// RevLiteralInner — literal-needle reverse search
// ============================================================

static RevLiteralInner rev_literal_inner_new(uint8_t *needle, size_t needle_len,
                                              size_t tail_offset)
{
    REQUIRE(needle_len != 0,
            "rev_literal_inner_new: needle must be non-empty");

    size_t rare_idx = 0;
    uint32_t rare_freq = BYTE_FREQ[needle[0]];
    for (size_t i = 1; i < needle_len; ++i) {
        uint32_t f = BYTE_FREQ[needle[i]];
        if (f < rare_freq) {
            rare_freq = f;
            rare_idx = i;
        }
    }
    size_t confirm_idx;
    if (needle_len > 1) {
        size_t ci = (rare_idx == 0) ? 1 : 0;
        uint32_t cf = BYTE_FREQ[needle[ci]];
        for (size_t i = 0; i < needle_len; ++i) {
            if (i == rare_idx) continue;
            uint32_t f = BYTE_FREQ[needle[i]];
            if (f < cf) {
                cf = f;
                ci = i;
            }
        }
        confirm_idx = ci;
    } else {
        confirm_idx = 0;
    }

    size_t cap = (needle_len + 7) / 8;
    uint64_t *chunks = (uint64_t *)xcalloc(cap, sizeof(uint64_t));
    size_t chunks_len = 0;
    size_t i = 0;
    while (i + 8 <= needle_len) {
        uint8_t v[8];
        memcpy(v, needle + i, 8);
        uint64_t u;
        memcpy(&u, v, 8);
        chunks[chunks_len++] = u;
        i += 8;
    }
    if (i < needle_len) {
        uint8_t v[8] = {0};
        memcpy(v, needle + i, needle_len - i);
        uint64_t u;
        memcpy(&u, v, 8);
        chunks[chunks_len++] = u;
    }

    return (RevLiteralInner){
        .needle = needle,
        .needle_len = needle_len,
        .chunks = chunks,
        .chunks_len = chunks_len,
        .rare_idx = rare_idx,
        .rare_byte = needle[rare_idx],
        .confirm_idx = confirm_idx,
        .confirm_byte = needle[confirm_idx],
        .tail_offset = tail_offset,
    };
}

[[gnu::always_inline]] static inline bool
rli_verify(const RevLiteralInner *self, const uint8_t *haystack, size_t start)
{
    size_t n = self->needle_len;
    const uint8_t *hp = haystack + start;
    size_t ci = 0;
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t h;
        memcpy(&h, hp + off, 8);
        if (h != self->chunks[ci]) return false;
        ci += 1;
        off += 8;
    }
    if (off < n) {
        // Same OOB-vs-Rust-unsafe bug as FwdLiteralSearch_verify above —
        // see the H1 fix comment there. Copy only the remaining bytes
        // into a zero-padded buffer; the mask isolates the actual tail.
        uint64_t h = 0;
        memcpy(&h, hp + off, n - off);
        uint64_t mask = ((uint64_t)1 << ((n - off) * 8)) - 1;
        if (((h ^ self->chunks[ci]) & mask) != 0) return false;
    }
    return true;
}

#ifdef __wasm_simd128__
static size_t rli_find_rev_wasm(const RevLiteralInner *self,
                                 const uint8_t *haystack, size_t end)
{
    size_t nlen = self->needle_len;
    if (end + 1 < nlen) return SENTINEL;
    const uint8_t *ptr = haystack;
    size_t rare_idx = self->rare_idx;
    uint8_t rare_byte = self->rare_byte;
    size_t confirm_idx = self->confirm_idx;
    uint8_t confirm_byte = self->confirm_byte;
    v128_t vrare = wasm_u8x16_splat(rare_byte);
    size_t min_rare_pos = rare_idx;
    size_t pos = end - (nlen - 1) + rare_idx;

    while (pos >= min_rare_pos + 16) {
        v128_t chunk = wasm_v128_load((const v128_t *)(ptr + pos - 15));
        uint16_t mask = (uint16_t)wasm_i8x16_bitmask(wasm_i8x16_eq(chunk, vrare));
        while (mask != 0) {
            size_t bit = (size_t)(15u - clz_u16(mask));
            size_t start = pos - 15 + bit - rare_idx;
            if (ptr[start + confirm_idx] == confirm_byte
                && rli_verify(self, haystack, start)) {
                return start + nlen - 1;
            }
            mask &= (uint16_t)~(1u << bit);
        }
        pos -= 16;
    }
    for (;;) {
        if (ptr[pos] == rare_byte) {
            size_t start = pos - rare_idx;
            if (ptr[start + confirm_idx] == confirm_byte
                && rli_verify(self, haystack, start)) {
                return start + nlen - 1;
            }
        }
        if (pos == min_rare_pos) break;
        pos -= 1;
    }
    return SENTINEL;
}
#endif

// ============================================================
// RevTeddySearch (tagged Teddy/Literal)
// ============================================================

static TeddyMasks *teddy_masks_new_zero(void) {
    TeddyMasks *m = (TeddyMasks *)xcalloc(1, sizeof(TeddyMasks));
    return m;
}

RevTeddySearch rev_prefix_search_new(size_t len,
                                    const ByteVec *byte_sets_raw,
                                    TSet *all_sets, size_t all_sets_len,
                                    size_t tail_offset)
{
    bool is_literal = true;
    for (size_t i = 0; i < len; ++i) {
        if (byte_sets_raw[i].len != 1) {
            is_literal = false;
            break;
        }
    }

    if (is_literal) {
        // needle = byte_sets_raw.iter().rev().map(|bs| bs[0]).collect()
        uint8_t *needle = (uint8_t *)xmalloc(len);
        for (size_t i = 0; i < len; ++i) {
            needle[i] = byte_sets_raw[len - 1 - i].data[0];
        }
        // Literal path doesn't use the Teddy sets; release them to match
        // Rust's drop of the moved Vec<TSet>.
        xfree(all_sets);
        return (RevTeddySearch){
            .kind = REV_SEARCH_KIND_LITERAL,
            .u = { .literal = rev_literal_inner_new(needle, len, tail_offset) },
        };
    }

    (void)all_sets_len;
    size_t num_simd = len < 3 ? len : 3;
    TeddyMasks *masks = teddy_masks_new_zero();
    for (size_t i = 0; i < num_simd; ++i) {
        uint8_t lo[16] = {0};
        uint8_t hi[16] = {0};
        const ByteVec *bv = &byte_sets_raw[i];
        for (size_t k = 0; k < bv->len; ++k) {
            uint8_t b = bv->data[k];
            lo[b & 0xF] |= 0x80;
            hi[(b >> 4) & 0xF] |= 0x80;
        }
        memcpy(&masks->lo[i][0], lo, 16);
        memcpy(&masks->lo[i][16], lo, 16);
        memcpy(&masks->hi[i][0], hi, 16);
        memcpy(&masks->hi[i][16], hi, 16);
    }
    return (RevTeddySearch){
        .kind = REV_SEARCH_KIND_TEDDY,
        .u = { .teddy = (RevTeddyInner){
            .len = len,
            .num_simd = num_simd,
            .masks = masks,
            .sets = all_sets,
            .sets_len = len,
            .tail_offset = tail_offset,
        } },
    };
}

void RevTeddySearch_free(RevTeddySearch *self) {
    if (self->kind == REV_SEARCH_KIND_LITERAL) {
        xfree(self->u.literal.needle);
        xfree(self->u.literal.chunks);
    } else {
        xfree(self->u.teddy.masks);
        xfree(self->u.teddy.sets);
    }
    *self = (RevTeddySearch){0};
}

RevTeddySearch rev_prefix_search_add_tail_offset(RevTeddySearch self, uint32_t extra) {
    if (self.kind == REV_SEARCH_KIND_LITERAL) {
        self.u.literal.tail_offset += (size_t)extra;
    } else {
        self.u.teddy.tail_offset += (size_t)extra;
    }
    return self;
}

size_t rev_prefix_search_len(const RevTeddySearch *self) {
    if (self->kind == REV_SEARCH_KIND_LITERAL) return self->u.literal.needle_len;
    return self->u.teddy.len;
}

static size_t RevTeddySearch_verify_tail(const RevTeddyInner *t,
                                          const uint8_t *haystack, size_t end)
{
    size_t min_pos = t->len - 1;
    size_t pos = end;
    for (;;) {
        if (pos < min_pos) return SENTINEL;
        bool ok = true;
        for (size_t i = 0; i < t->len; ++i) {
            if (!TSet_contains_byte(&t->sets[i], haystack[pos - i])) {
                ok = false;
                if (pos == min_pos) return SENTINEL;
                pos -= 1;
                break;
            }
        }
        if (ok) return pos;
    }
}

#ifdef __wasm_simd128__
[[gnu::always_inline]] static inline size_t RevTeddySearch_verify_rev_inline(
    const uint8_t *ptr, size_t chunk_pos, uint16_t bits,
    const TSet *sets_ptr, size_t len)
{
    while (bits != 0) {
        size_t bit = (size_t)(15u - clz_u16(bits));
        size_t candidate = chunk_pos + bit;
        if (candidate + 1 < len) {
            bits &= (uint16_t)~(1u << bit);
            continue;
        }
        bool ok = true;
        size_t j = 0;
        while (j < len) {
            if (!TSet_contains_byte(&sets_ptr[j], ptr[candidate - j])) {
                ok = false;
                break;
            }
            j += 1;
        }
        if (ok) return candidate;
        bits &= (uint16_t)~(1u << bit);
    }
    return SENTINEL;
}

static size_t RevTeddySearch_teddy_rev(const RevTeddyInner *t, size_t N,
                                        const uint8_t *haystack, size_t end)
{
    const uint8_t *ptr = haystack;
    v128_t nib = wasm_u8x16_splat(0x0F);
    v128_t masks_lo[3] = {
        wasm_v128_load((const v128_t *)t->masks->lo[0]),
        wasm_v128_load((const v128_t *)t->masks->lo[1]),
        wasm_v128_load((const v128_t *)t->masks->lo[2]),
    };
    v128_t masks_hi[3] = {
        wasm_v128_load((const v128_t *)t->masks->hi[0]),
        wasm_v128_load((const v128_t *)t->masks->hi[1]),
        wasm_v128_load((const v128_t *)t->masks->hi[2]),
    };
    const TSet *sets_ptr = t->sets;
    size_t len = t->len;
    size_t min_pos = len - 1;

    if (end < 15 + min_pos) {
        return RevTeddySearch_verify_tail(t, haystack, end);
    }
    size_t chunk_pos = end - 15;

    for (;;) {
        v128_t r = teddy_chunk_rev(N, ptr, chunk_pos, masks_lo, masks_hi, nib);
        uint16_t mask = (uint16_t)wasm_i8x16_bitmask(r);
        if (mask != 0) {
            size_t m = RevTeddySearch_verify_rev_inline(ptr, chunk_pos, mask, sets_ptr, len);
            if (m != SENTINEL) return m;
        }
        if (chunk_pos < 16 + min_pos) break;
        chunk_pos -= 16;
    }
    size_t fallback = (chunk_pos == 0) ? 0 : (chunk_pos - 1);
    if (fallback > end) fallback = end;
    return RevTeddySearch_verify_tail(t, haystack, fallback);
}
#endif

size_t rev_prefix_search_find_rev(const RevTeddySearch *self,
                                const uint8_t *haystack, size_t hlen,
                                size_t end)
{
    if (self->kind == REV_SEARCH_KIND_LITERAL) {
        const RevLiteralInner *l = &self->u.literal;
        size_t hsat = (hlen == 0) ? 0 : (hlen - 1);
        if (end > hsat) end = hsat;
        if (end < l->tail_offset) return SENTINEL;
        end -= l->tail_offset;
#ifdef __wasm_simd128__
        size_t r = rli_find_rev_wasm(l, haystack, end);
        if (r == SENTINEL) return SENTINEL;
        return r + l->tail_offset;
#else
        (void)haystack;
        return SENTINEL;
#endif
    }

    const RevTeddyInner *t = &self->u.teddy;
    size_t hsat = (hlen == 0) ? 0 : (hlen - 1);
    if (end > hsat) end = hsat;
    if (end < t->tail_offset) return SENTINEL;
    end -= t->tail_offset;

#ifdef __wasm_simd128__
    size_t r;
    switch (t->num_simd) {
        case 1:  r = RevTeddySearch_teddy_rev(t, 1, haystack, end); break;
        case 2:  r = RevTeddySearch_teddy_rev(t, 2, haystack, end); break;
        default: r = RevTeddySearch_teddy_rev(t, 3, haystack, end); break;
    }
    if (r == SENTINEL) return SENTINEL;
    return r + t->tail_offset;
#else
    (void)haystack;
    return SENTINEL;
#endif
}

// ============================================================
// FwdPrefixSearch
// ============================================================

size_t FwdPrefixSearch_len(const FwdPrefixSearch *self) { return self->len; }

FwdPrefixSearch FwdPrefixSearch_new(size_t len,
                                    const size_t *freq_order, size_t freq_order_len,
                                    const ByteVec *byte_sets_raw,
                                    TSet *all_sets, size_t all_sets_len)
{
    (void)all_sets_len;
    size_t num_simd = len < 3 ? len : 3;
    size_t simd_offsets[3] = {0, 0, 0};
    TeddyMasks *masks = teddy_masks_new_zero();
    for (size_t i = 0; i < num_simd; ++i) {
        size_t pos = freq_order[i];
        simd_offsets[i] = pos;
        uint8_t lo[16] = {0};
        uint8_t hi[16] = {0};
        const ByteVec *bv = &byte_sets_raw[pos];
        for (size_t k = 0; k < bv->len; ++k) {
            uint8_t b = bv->data[k];
            lo[b & 0xF] |= 0x80;
            hi[(b >> 4) & 0xF] |= 0x80;
        }
        memcpy(&masks->lo[i][0], lo, 16);
        memcpy(&masks->lo[i][16], lo, 16);
        memcpy(&masks->hi[i][0], hi, 16);
        memcpy(&masks->hi[i][16], hi, 16);
    }
    uint8_t verify_order[16] = {0};
    size_t vi = 0;
    for (size_t k = 0; k < freq_order_len; ++k) {
        size_t pos = freq_order[k];
        if (pos >= num_simd && pos < len) {
            verify_order[vi++] = (uint8_t)pos;
        }
    }
    for (size_t k = 0; k < freq_order_len; ++k) {
        size_t pos = freq_order[k];
        if (pos < num_simd) {
            verify_order[vi++] = (uint8_t)pos;
        }
    }

    FwdPrefixSearch r = {
        .len = len,
        .num_simd = num_simd,
        .masks = masks,
        .sets = all_sets,
        .sets_len = len,
    };
    memcpy(r.simd_offsets, simd_offsets, sizeof(simd_offsets));
    memcpy(r.verify_order, verify_order, sizeof(verify_order));
    return r;
}

void fwd_prefix_search_free(FwdPrefixSearch *self) {
    xfree(self->masks);
    xfree(self->sets);
    *self = (FwdPrefixSearch){0};
}

static size_t FwdPrefixSearch_verify_tail_fwd(const FwdPrefixSearch *self,
                                              const uint8_t *haystack, size_t hlen,
                                              size_t start)
{
    if (hlen < self->len) return SENTINEL;
    size_t end = hlen - self->len;
    size_t pos = start;
    while (pos <= end) {
        bool ok = true;
        for (size_t i = 0; i < self->len; ++i) {
            if (!TSet_contains_byte(&self->sets[i], haystack[pos + i])) {
                ok = false;
                break;
            }
        }
        if (ok) return pos;
        pos += 1;
    }
    return SENTINEL;
}

#ifdef __wasm_simd128__
[[gnu::always_inline]] static inline size_t FwdPrefixSearch_verify_inline(
    const uint8_t *ptr, size_t pos, uint16_t bits,
    const TSet *sets_ptr, size_t len, const uint8_t *verify_order)
{
    while (bits != 0) {
        unsigned bit = ctz_u16(bits);
        size_t candidate = pos + (size_t)bit;
        const uint8_t *base = ptr + candidate;
        bool ok = true;
        size_t j = 0;
        while (j < len) {
            size_t idx = (size_t)verify_order[j];
            if (!TSet_contains_byte(&sets_ptr[idx], base[idx])) {
                ok = false;
                break;
            }
            j += 1;
        }
        if (ok) return candidate;
        bits &= (uint16_t)(bits - 1u);
    }
    return SENTINEL;
}

static size_t FwdPrefixSearch_teddy_fwd(const FwdPrefixSearch *self, size_t N,
                                        const uint8_t *haystack, size_t hlen,
                                        size_t start)
{
    const uint8_t *ptr = haystack;
    v128_t nib = wasm_u8x16_splat(0x0F);
    v128_t masks_lo[3] = {
        wasm_v128_load((const v128_t *)self->masks->lo[0]),
        wasm_v128_load((const v128_t *)self->masks->lo[1]),
        wasm_v128_load((const v128_t *)self->masks->lo[2]),
    };
    v128_t masks_hi[3] = {
        wasm_v128_load((const v128_t *)self->masks->hi[0]),
        wasm_v128_load((const v128_t *)self->masks->hi[1]),
        wasm_v128_load((const v128_t *)self->masks->hi[2]),
    };
    const TSet *sets_ptr = self->sets;
    size_t len = self->len;

    size_t needed = 15 + self->len - 1;
    size_t simd_end = (hlen > needed) ? (hlen - needed) : 0;
    size_t pos = start;

    while (pos < simd_end) {
        v128_t r = teddy_chunk(N, ptr, pos, self->simd_offsets, masks_lo, masks_hi, nib);
        uint16_t mask = (uint16_t)wasm_i8x16_bitmask(r);
        if (mask != 0) {
            size_t m = FwdPrefixSearch_verify_inline(ptr, pos, mask, sets_ptr, len,
                                                     self->verify_order);
            if (m != SENTINEL) return m;
        }
        pos += 16;
    }
    return FwdPrefixSearch_verify_tail_fwd(self, haystack, hlen, pos);
}
#endif

size_t FwdPrefixSearch_find_fwd(const FwdPrefixSearch *self,
                                const uint8_t *haystack, size_t hlen,
                                size_t start)
{
#ifdef __wasm_simd128__
    switch (self->num_simd) {
        case 1:  return FwdPrefixSearch_teddy_fwd(self, 1, haystack, hlen, start);
        case 2:  return FwdPrefixSearch_teddy_fwd(self, 2, haystack, hlen, start);
        default: return FwdPrefixSearch_teddy_fwd(self, 3, haystack, hlen, start);
    }
#else
    return FwdPrefixSearch_verify_tail_fwd(self, haystack, hlen, start);
#endif
}

#endif // __wasm_simd128__
