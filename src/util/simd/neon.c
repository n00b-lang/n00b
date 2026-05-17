// SIMD prefilter primitives — aarch64 / NEON backend.
// Phase 10 port from resharp-c/src/simd/neon.c.
//
// External symbols carry the `n00b_simd_` prefix per § 19a.  Internal
// `static` helpers keep their resharp-c names (file-locality is
// preserved per § 5 style-change B).  Allocations route through n00b's
// `n00b_alloc(T)` / `n00b_alloc_array(T, N)` / `n00b_free` per D13.
// libc memcpy / memset / memcmp / memmove are used directly per D13.
//
// NOTE: Rust's const generics (`<const FWD: bool>`, `<const COLLECT_ALL:
// bool>`) are realized here by emitting specialized internal helpers per
// generic, each taking the const flag as a `bool` parameter only at the
// call site.

#include "util/simd/neon.h"
#include "util/simd/mod.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdckdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "util/assert.h"
#include "util/panic.h"

#include "internal/regex/ids.h"  // TSet real definition.

// ============================================================================
// Local checked-arithmetic + panic helpers.
// ============================================================================

[[noreturn]] static inline void neon_capacity_overflow(void) {
    n00b_panic("simd/neon: capacity overflow");
}

static inline size_t neon_mul_sz(size_t a, size_t b) {
    size_t r;
    if (ckd_mul(&r, a, b)) neon_capacity_overflow();
    return r;
}

// ============================================================================
// MatchVec
// ============================================================================

static void neon_match_vec_init(n00b_simd_MatchVec *v) {
    v->data = nullptr;
    v->len = 0;
    v->cap = 0;
}

static void neon_match_vec_push(n00b_simd_MatchVec *v, size_t start, size_t end) {
    if (v->len == v->cap) {
        size_t new_cap = v->cap == 0 ? 8 : neon_mul_sz(v->cap, 2);
        n00b_simd_MatchPair *new_buf = n00b_alloc_array(n00b_simd_MatchPair, new_cap);
        if (v->len > 0 && v->data != nullptr) {
            memcpy(new_buf, v->data, v->len * sizeof(n00b_simd_MatchPair));
        }
        if (v->data) n00b_free(v->data);
        v->data = new_buf;
        v->cap = new_cap;
    }
    v->data[v->len].start = start;
    v->data[v->len].end = end;
    v->len += 1;
}

// ============================================================================
// Small helpers
// ============================================================================

[[gnu::always_inline]] static inline n00b_simd_OptUsize opt_some(size_t v) {
    return (n00b_simd_OptUsize){.found = true, .value = v};
}

[[gnu::always_inline]] static inline n00b_simd_OptUsize opt_none(void) {
    return (n00b_simd_OptUsize){.found = false, .value = 0};
}

[[gnu::always_inline]] static inline unsigned trailing_zeros_u16(uint16_t x) {
    // Matches Rust's `u16::trailing_zeros` which returns 16 for 0.
    if (x == 0) return 16;
    return (unsigned)__builtin_ctz((unsigned)x);
}

[[gnu::always_inline]] static inline unsigned leading_zeros_u16(uint16_t x) {
    // Rust's `u16::leading_zeros` returns 16 for 0.
    if (x == 0) return 16;
    return (unsigned)__builtin_clz((unsigned)x) - 16u;
}

[[gnu::always_inline]] static inline size_t saturating_sub_size(size_t a, size_t b) {
    return a > b ? a - b : 0;
}

// ============================================================================
// neon_movemask — 128-bit signed compare result -> 16-bit mask.
// ============================================================================

uint16_t n00b_simd_neon_movemask(uint8x16_t v) {
    uint8x16_t signs = vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(v), 7));
    static const uint8_t MASK_BITS[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    uint8x8_t mask = vld1_u8(MASK_BITS);
    uint8x8_t lo = vand_u8(vget_low_u8(signs), mask);
    uint8x8_t hi = vand_u8(vget_high_u8(signs), mask);
    return (uint16_t)vaddv_u8(lo) | ((uint16_t)vaddv_u8(hi) << 8);
}

// ============================================================================
// RevSearchBytes
// ============================================================================

n00b_simd_RevSearchBytes n00b_simd_rev_search_bytes_new_value(const uint8_t *bytes, size_t len) {
    n00b_require(len != 0 && len <= 3,
                 "n00b_simd_rev_search_bytes_new: len must be in 1..=3");
    n00b_simd_RevSearchBytes s;
    s.bytes = n00b_alloc_array(uint8_t, len);
    memcpy(s.bytes, bytes, len);
    s.len = len;
    return s;
}

void n00b_simd_rev_search_bytes_free(n00b_simd_RevSearchBytes *s) {
    if (s->bytes) n00b_free(s->bytes);
    s->bytes = nullptr;
    s->len = 0;
}

const uint8_t *n00b_simd_rev_search_bytes_bytes(const n00b_simd_RevSearchBytes *s,
                                                size_t                         *out_len) {
    if (out_len) *out_len = s->len;
    return s->bytes;
}

[[gnu::always_inline]] static inline uint8x16_t
rsb_compute_combined(uint8x16_t chunk, size_t n,
                     uint8x16_t v0, uint8x16_t v1, uint8x16_t v2) {
    uint8x16_t cmp0 = vceqq_u8(chunk, v0);
    if (n >= 3) {
        return vorrq_u8(cmp0, vorrq_u8(vceqq_u8(chunk, v1), vceqq_u8(chunk, v2)));
    } else if (n >= 2) {
        return vorrq_u8(cmp0, vceqq_u8(chunk, v1));
    } else {
        return cmp0;
    }
}

static n00b_simd_OptUsize rsb_search_neon(const n00b_simd_RevSearchBytes *self,
                                          const uint8_t *haystack, size_t len,
                                          bool fwd) {
    if (len == 0) return opt_none();
    const uint8_t *ptr = haystack;
    uint8x16_t v0 = vdupq_n_u8(self->bytes[0]);
    size_t n = self->len;
    uint8x16_t v1 = (n >= 2) ? vdupq_n_u8(self->bytes[1]) : v0;
    uint8x16_t v2 = (n >= 3) ? vdupq_n_u8(self->bytes[2]) : v0;

    if (fwd) {
        size_t pos = 0;
        while (pos + 16 <= len) {
            uint8x16_t combined = rsb_compute_combined(vld1q_u8(ptr + pos), n, v0, v1, v2);
            if (vmaxvq_u8(combined) != 0) {
                uint16_t mask = n00b_simd_neon_movemask(combined);
                return opt_some(pos + (size_t)trailing_zeros_u16(mask));
            }
            pos += 16;
        }
        if (pos < len) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack + pos, len - pos);
            uint8x16_t combined = rsb_compute_combined(vld1q_u8(buf), n, v0, v1, v2);
            uint16_t mask = n00b_simd_neon_movemask(combined);
            mask &= (uint16_t)((1u << (len - pos)) - 1u);
            if (mask != 0) {
                return opt_some(pos + (size_t)trailing_zeros_u16(mask));
            }
        }
    } else {
        if (len >= 16) {
            size_t pos = len - 16;
            for (;;) {
                uint8x16_t combined = rsb_compute_combined(vld1q_u8(ptr + pos), n, v0, v1, v2);
                if (vmaxvq_u8(combined) != 0) {
                    uint16_t mask = n00b_simd_neon_movemask(combined);
                    return opt_some(pos + 15u - (size_t)leading_zeros_u16(mask));
                }
                if (pos < 16) break;
                pos -= 16;
            }
        }
        size_t gap = (len >= 16) ? (len % 16) : len;
        if (gap > 0) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack, gap);
            uint8x16_t combined = rsb_compute_combined(vld1q_u8(buf), n, v0, v1, v2);
            uint16_t mask = n00b_simd_neon_movemask(combined);
            mask &= (uint16_t)((1u << gap) - 1u);
            if (mask != 0) {
                return opt_some(15u - (size_t)leading_zeros_u16(mask));
            }
        }
    }
    return opt_none();
}

n00b_simd_OptUsize n00b_simd_rev_search_bytes_find_fwd_raw(
    const n00b_simd_RevSearchBytes *s,
    const uint8_t                  *haystack,
    size_t                          hlen) {
    return rsb_search_neon(s, haystack, hlen, true);
}

n00b_simd_OptUsize n00b_simd_rev_search_bytes_find_rev_raw(
    const n00b_simd_RevSearchBytes *s,
    const uint8_t                  *haystack,
    size_t                          hlen) {
    return rsb_search_neon(s, haystack, hlen, false);
}

// ============================================================================
// RevSearchRanges
// ============================================================================

n00b_simd_RevSearchRanges n00b_simd_rev_search_ranges_new_value(
    const n00b_simd_ByteRange *ranges, size_t len) {
    n00b_require(len != 0 && len <= 4,
                 "n00b_simd_rev_search_ranges_new: len must be in 1..=4");
    n00b_simd_RevSearchRanges s;
    s.ranges = n00b_alloc_array(n00b_simd_ByteRange, len);
    memcpy(s.ranges, ranges, len * sizeof(n00b_simd_ByteRange));
    s.len = len;
    return s;
}

void n00b_simd_rev_search_ranges_free(n00b_simd_RevSearchRanges *s) {
    if (s->ranges) n00b_free(s->ranges);
    s->ranges = nullptr;
    s->len = 0;
}

const n00b_simd_ByteRange *n00b_simd_rev_search_ranges_ranges(
    const n00b_simd_RevSearchRanges *s, size_t *out_len) {
    if (out_len) *out_len = s->len;
    return s->ranges;
}

[[gnu::always_inline]] static inline uint8x16_t
rsr_compute_combined(uint8x16_t chunk, size_t n,
                     uint8x16_t lo0, uint8x16_t hi0,
                     uint8x16_t lo1, uint8x16_t hi1,
                     uint8x16_t lo2, uint8x16_t hi2,
                     uint8x16_t lo3, uint8x16_t hi3) {
    uint8x16_t in0 = vandq_u8(vcgeq_u8(chunk, lo0), vcleq_u8(chunk, hi0));
    if (n >= 4) {
        uint8x16_t in1 = vandq_u8(vcgeq_u8(chunk, lo1), vcleq_u8(chunk, hi1));
        uint8x16_t in2 = vandq_u8(vcgeq_u8(chunk, lo2), vcleq_u8(chunk, hi2));
        uint8x16_t in3 = vandq_u8(vcgeq_u8(chunk, lo3), vcleq_u8(chunk, hi3));
        return vorrq_u8(vorrq_u8(in0, in1), vorrq_u8(in2, in3));
    } else if (n >= 3) {
        uint8x16_t in1 = vandq_u8(vcgeq_u8(chunk, lo1), vcleq_u8(chunk, hi1));
        uint8x16_t in2 = vandq_u8(vcgeq_u8(chunk, lo2), vcleq_u8(chunk, hi2));
        return vorrq_u8(in0, vorrq_u8(in1, in2));
    } else if (n >= 2) {
        uint8x16_t in1 = vandq_u8(vcgeq_u8(chunk, lo1), vcleq_u8(chunk, hi1));
        return vorrq_u8(in0, in1);
    } else {
        return in0;
    }
}

static n00b_simd_OptUsize rsr_search_neon(const n00b_simd_RevSearchRanges *self,
                                          const uint8_t *haystack, size_t len,
                                          bool fwd) {
    if (len == 0) return opt_none();
    const uint8_t *ptr = haystack;
    size_t n = self->len;
    uint8x16_t lo0 = vdupq_n_u8(self->ranges[0].lo);
    uint8x16_t hi0 = vdupq_n_u8(self->ranges[0].hi);
    uint8x16_t lo1 = (n >= 2) ? vdupq_n_u8(self->ranges[1].lo) : lo0;
    uint8x16_t hi1 = (n >= 2) ? vdupq_n_u8(self->ranges[1].hi) : hi0;
    uint8x16_t lo2 = (n >= 3) ? vdupq_n_u8(self->ranges[2].lo) : lo0;
    uint8x16_t hi2 = (n >= 3) ? vdupq_n_u8(self->ranges[2].hi) : hi0;
    uint8x16_t lo3 = (n >= 4) ? vdupq_n_u8(self->ranges[3].lo) : lo0;
    uint8x16_t hi3 = (n >= 4) ? vdupq_n_u8(self->ranges[3].hi) : hi0;

    if (fwd) {
        size_t pos = 0;
        while (pos + 16 <= len) {
            uint8x16_t combined = rsr_compute_combined(vld1q_u8(ptr + pos), n,
                                                       lo0, hi0, lo1, hi1,
                                                       lo2, hi2, lo3, hi3);
            if (vmaxvq_u8(combined) != 0) {
                uint16_t mask = n00b_simd_neon_movemask(combined);
                return opt_some(pos + (size_t)trailing_zeros_u16(mask));
            }
            pos += 16;
        }
        if (pos < len) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack + pos, len - pos);
            uint8x16_t combined = rsr_compute_combined(vld1q_u8(buf), n,
                                                       lo0, hi0, lo1, hi1,
                                                       lo2, hi2, lo3, hi3);
            uint16_t mask = n00b_simd_neon_movemask(combined);
            mask &= (uint16_t)((1u << (len - pos)) - 1u);
            if (mask != 0) {
                return opt_some(pos + (size_t)trailing_zeros_u16(mask));
            }
        }
    } else {
        if (len >= 16) {
            size_t pos = len - 16;
            for (;;) {
                uint8x16_t combined = rsr_compute_combined(vld1q_u8(ptr + pos), n,
                                                           lo0, hi0, lo1, hi1,
                                                           lo2, hi2, lo3, hi3);
                if (vmaxvq_u8(combined) != 0) {
                    uint16_t mask = n00b_simd_neon_movemask(combined);
                    return opt_some(pos + 15u - (size_t)leading_zeros_u16(mask));
                }
                if (pos < 16) break;
                pos -= 16;
            }
        }
        size_t gap = (len >= 16) ? (len % 16) : len;
        if (gap > 0) {
            uint8_t buf[16] = {0};
            memcpy(buf, haystack, gap);
            uint8x16_t combined = rsr_compute_combined(vld1q_u8(buf), n,
                                                       lo0, hi0, lo1, hi1,
                                                       lo2, hi2, lo3, hi3);
            uint16_t mask = n00b_simd_neon_movemask(combined);
            mask &= (uint16_t)((1u << gap) - 1u);
            if (mask != 0) {
                return opt_some(15u - (size_t)leading_zeros_u16(mask));
            }
        }
    }
    return opt_none();
}

n00b_simd_OptUsize n00b_simd_rev_search_ranges_find_fwd_raw(
    const n00b_simd_RevSearchRanges *s,
    const uint8_t                   *haystack,
    size_t                           hlen) {
    return rsr_search_neon(s, haystack, hlen, true);
}

n00b_simd_OptUsize n00b_simd_rev_search_ranges_find_rev_raw(
    const n00b_simd_RevSearchRanges *s,
    const uint8_t                   *haystack,
    size_t                           hlen) {
    return rsr_search_neon(s, haystack, hlen, false);
}

// ============================================================================
// FwdLiteralSearch
// ============================================================================

n00b_simd_FwdLiteralSearch n00b_simd_fwd_literal_search_new_value(
    const uint8_t *needle, size_t needle_len) {
    n00b_require(needle_len != 0,
                 "n00b_simd_fwd_literal_search_new: needle_len must be non-zero");
    n00b_simd_FwdLiteralSearch s;

    size_t rare_idx = 0;
    uint32_t rare_freq = n00b_simd_BYTE_FREQ[needle[0]];
    for (size_t i = 1; i < needle_len; ++i) {
        uint32_t f = n00b_simd_BYTE_FREQ[needle[i]];
        if (f < rare_freq) {
            rare_freq = f;
            rare_idx = i;
        }
    }

    size_t confirm_idx;
    if (needle_len > 1) {
        size_t ci = (rare_idx == 0) ? 1 : 0;
        uint32_t cf = n00b_simd_BYTE_FREQ[needle[ci]];
        for (size_t i = 0; i < needle_len; ++i) {
            if (i == rare_idx) continue;
            uint32_t f = n00b_simd_BYTE_FREQ[needle[i]];
            if (f < cf) {
                cf = f;
                ci = i;
            }
        }
        confirm_idx = ci;
    } else {
        confirm_idx = 0;
    }

    size_t chunks_cap = (needle_len + 7) / 8;
    uint64_t *chunks = n00b_alloc_array(uint64_t, chunks_cap);
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

    s.needle = n00b_alloc_array(uint8_t, needle_len);
    memcpy(s.needle, needle, needle_len);
    s.needle_len = needle_len;
    s.chunks = chunks;
    s.chunks_len = chunks_len;
    s.rare_idx = rare_idx;
    s.rare_byte = needle[rare_idx];
    s.confirm_idx = confirm_idx;
    s.confirm_byte = needle[confirm_idx];
    s.confirm_offset = (ptrdiff_t)confirm_idx - (ptrdiff_t)rare_idx;
    return s;
}

void n00b_simd_fwd_literal_search_free_inner(n00b_simd_FwdLiteralSearch *s) {
    if (s->needle) n00b_free(s->needle);
    if (s->chunks) n00b_free(s->chunks);
    s->needle = nullptr;
    s->chunks = nullptr;
    s->needle_len = 0;
    s->chunks_len = 0;
}

size_t n00b_simd_fwd_literal_search_len(const n00b_simd_FwdLiteralSearch *s) {
    return s->needle_len;
}

uint8_t n00b_simd_fwd_literal_search_rare_byte(const n00b_simd_FwdLiteralSearch *s) {
    return s->rare_byte;
}

[[gnu::always_inline]] static inline bool
fls_verify(const n00b_simd_FwdLiteralSearch *self, const uint8_t *haystack, size_t start) {
    size_t n = self->needle_len;
    const uint8_t *hp = haystack + start;
    size_t ci = 0;
    size_t off = 0;
    while (off + 8 <= n) {
        uint64_t h;
        memcpy(&h, hp + off, 8);
        if (h != self->chunks[ci]) {
            return false;
        }
        ci += 1;
        off += 8;
    }
    if (off < n) {
        uint8_t buf[8] = {0};
        memcpy(buf, hp + off, n - off);
        uint64_t h;
        memcpy(&h, buf, 8);
        uint64_t mask = ((uint64_t)1 << ((n - off) * 8)) - 1;
        if (((h ^ self->chunks[ci]) & mask) != 0) {
            return false;
        }
    }
    return true;
}

[[gnu::always_inline]] static inline n00b_simd_OptUsize
fls_scan(const n00b_simd_FwdLiteralSearch *self,
         const uint8_t *haystack, size_t hlen,
         n00b_simd_MatchVec *matches, bool collect_all) {
    size_t nlen = self->needle_len;
    if (hlen < nlen) return opt_none();
    const uint8_t *ptr = haystack;
    size_t rare_idx = self->rare_idx;
    uint8_t rare_byte = self->rare_byte;
    uint8_t confirm_byte = self->confirm_byte;
    ptrdiff_t confirm_offset = self->confirm_offset;
    size_t end = hlen - nlen + rare_idx;
    uint8x16_t vrare = vdupq_n_u8(rare_byte);
    uint8x16_t vconfirm = vdupq_n_u8(confirm_byte);
    size_t last_end = 0;

    #define HANDLE(start_expr)                                              \
        do {                                                                \
            size_t _start = (start_expr);                                   \
            if (collect_all && _start < last_end) break;                    \
            if (!fls_verify(self, haystack, _start)) break;                 \
            if (collect_all) {                                              \
                size_t _m_end = _start + nlen;                              \
                neon_match_vec_push(matches, _start, _m_end);               \
                last_end = _m_end;                                          \
            } else {                                                        \
                return opt_some(_start);                                    \
            }                                                               \
        } while (0)

    size_t pos = rare_idx;
    while (pos + 32 <= end + 1) {
        uint8x16_t r0 = vceqq_u8(vld1q_u8(ptr + pos), vrare);
        uint8x16_t r1 = vceqq_u8(vld1q_u8(ptr + pos + 16), vrare);
        uint8x16_t c0 = vceqq_u8(vld1q_u8(ptr + (ptrdiff_t)pos + confirm_offset), vconfirm);
        uint8x16_t c1 = vceqq_u8(vld1q_u8(ptr + (ptrdiff_t)pos + 16 + confirm_offset), vconfirm);
        uint8x16_t and0 = vandq_u8(r0, c0);
        uint8x16_t and1 = vandq_u8(r1, c1);
        if (vmaxvq_u8(vorrq_u8(and0, and1)) == 0) {
            pos += 32;
            continue;
        }
        uint16_t mask = n00b_simd_neon_movemask(and0);
        while (mask != 0) {
            size_t bit = (size_t)trailing_zeros_u16(mask);
            HANDLE(pos + bit - rare_idx);
            mask &= (uint16_t)(mask - 1);
        }
        mask = n00b_simd_neon_movemask(and1);
        while (mask != 0) {
            size_t bit = (size_t)trailing_zeros_u16(mask);
            HANDLE(pos + 16 + bit - rare_idx);
            mask &= (uint16_t)(mask - 1);
        }
        pos += 32;
    }
    while (pos + 16 <= end + 1) {
        uint8x16_t r = vceqq_u8(vld1q_u8(ptr + pos), vrare);
        uint8x16_t c = vceqq_u8(vld1q_u8(ptr + (ptrdiff_t)pos + confirm_offset), vconfirm);
        uint8x16_t andv = vandq_u8(r, c);
        if (vmaxvq_u8(andv) == 0) {
            pos += 16;
            continue;
        }
        uint16_t mask = n00b_simd_neon_movemask(andv);
        while (mask != 0) {
            size_t bit = (size_t)trailing_zeros_u16(mask);
            HANDLE(pos + bit - rare_idx);
            mask &= (uint16_t)(mask - 1);
        }
        pos += 16;
    }
    while (pos <= end) {
        if (ptr[pos] == rare_byte
            && ptr[(ptrdiff_t)pos + confirm_offset] == confirm_byte) {
            HANDLE(pos - rare_idx);
        }
        pos += 1;
    }
    #undef HANDLE
    return opt_none();
}

n00b_simd_OptUsize n00b_simd_fwd_literal_search_find_fwd_raw(
    const n00b_simd_FwdLiteralSearch *s,
    const uint8_t                    *haystack,
    size_t                            hlen) {
    n00b_simd_MatchVec sink;
    neon_match_vec_init(&sink);
    n00b_simd_OptUsize r = fls_scan(s, haystack, hlen, &sink, false);
    if (sink.data) n00b_free(sink.data);
    return r;
}

void n00b_simd_fwd_literal_search_find_all_fixed_raw(
    const n00b_simd_FwdLiteralSearch *s,
    const uint8_t                    *haystack,
    size_t                            hlen,
    n00b_simd_MatchVec               *matches) {
    (void)fls_scan(s, haystack, hlen, matches, true);
}

// ============================================================================
// RevLiteralInner — literal-needle reverse search (rare-byte broadcast)
// ============================================================================

static n00b_simd_RevLiteralInner rev_literal_inner_new(uint8_t *needle, size_t needle_len,
                                                       size_t tail_offset) {
    n00b_require(needle_len != 0, "rev_literal_inner_new: needle_len must be non-zero");

    size_t rare_idx = 0;
    uint32_t rare_freq = n00b_simd_BYTE_FREQ[needle[0]];
    for (size_t i = 1; i < needle_len; ++i) {
        uint32_t f = n00b_simd_BYTE_FREQ[needle[i]];
        if (f < rare_freq) {
            rare_freq = f;
            rare_idx = i;
        }
    }

    size_t confirm_idx;
    if (needle_len > 1) {
        size_t ci = (rare_idx == 0) ? 1 : 0;
        uint32_t cf = n00b_simd_BYTE_FREQ[needle[ci]];
        for (size_t i = 0; i < needle_len; ++i) {
            if (i == rare_idx) continue;
            uint32_t f = n00b_simd_BYTE_FREQ[needle[i]];
            if (f < cf) {
                cf = f;
                ci = i;
            }
        }
        confirm_idx = ci;
    } else {
        confirm_idx = 0;
    }

    size_t chunks_cap = (needle_len + 7) / 8;
    uint64_t *chunks = n00b_alloc_array(uint64_t, chunks_cap);
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

    return (n00b_simd_RevLiteralInner){
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
rli_verify(const n00b_simd_RevLiteralInner *self, const uint8_t *haystack, size_t start) {
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
        uint64_t h = 0;
        memcpy(&h, hp + off, n - off);
        uint64_t mask = ((uint64_t)1 << ((n - off) * 8)) - 1;
        if (((h ^ self->chunks[ci]) & mask) != 0) return false;
    }
    return true;
}

static n00b_simd_OptUsize rli_find_rev_neon(const n00b_simd_RevLiteralInner *self,
                                            const uint8_t *haystack, size_t end) {
    size_t nlen = self->needle_len;
    if (end + 1 < nlen) return opt_none();
    const uint8_t *ptr = haystack;
    size_t rare_idx = self->rare_idx;
    uint8_t rare_byte = self->rare_byte;
    size_t confirm_idx = self->confirm_idx;
    uint8_t confirm_byte = self->confirm_byte;
    uint8x16_t vrare = vdupq_n_u8(rare_byte);
    size_t min_rare_pos = rare_idx;
    size_t pos = end - (nlen - 1) + rare_idx;

    while (pos >= min_rare_pos + 16) {
        uint8x16_t chunk = vld1q_u8(ptr + pos - 15);
        uint16_t mask = n00b_simd_neon_movemask(vceqq_u8(chunk, vrare));
        while (mask != 0) {
            size_t bit = 15u - (size_t)leading_zeros_u16(mask);
            size_t start = pos - 15 + bit - rare_idx;
            if (ptr[start + confirm_idx] == confirm_byte
                && rli_verify(self, haystack, start)) {
                return opt_some(start + nlen - 1);
            }
            mask &= (uint16_t)~((uint16_t)1u << bit);
        }
        pos -= 16;
    }
    for (;;) {
        if (ptr[pos] == rare_byte) {
            size_t start = pos - rare_idx;
            if (ptr[start + confirm_idx] == confirm_byte
                && rli_verify(self, haystack, start)) {
                return opt_some(start + nlen - 1);
            }
        }
        if (pos == min_rare_pos) break;
        pos -= 1;
    }
    return opt_none();
}

// ============================================================================
// RevTeddySearch (tagged Teddy/Literal)
// ============================================================================

n00b_simd_RevTeddySearch n00b_simd_rev_prefix_search_new_value(
    size_t                len,
    const uint8_t *const *byte_sets_raw,
    const size_t         *byte_sets_lens,
    size_t                byte_sets_count,
    TSet                 *all_sets,
    size_t                all_sets_len,
    size_t                tail_offset) {
    n00b_require(all_sets_len == len,
                 "n00b_simd_rev_prefix_search_new: all_sets_len must equal len");
    n00b_require(byte_sets_count == len,
                 "n00b_simd_rev_prefix_search_new: byte_sets_count must equal len");

    bool is_literal = true;
    for (size_t i = 0; i < len; ++i) {
        if (byte_sets_lens[i] != 1) {
            is_literal = false;
            break;
        }
    }

    if (is_literal) {
        uint8_t *needle = n00b_alloc_array(uint8_t, len);
        for (size_t i = 0; i < len; ++i) {
            needle[i] = byte_sets_raw[len - 1 - i][0];
        }
        // Literal path doesn't use the Teddy sets; release the borrow.
        n00b_free(all_sets);
        return (n00b_simd_RevTeddySearch){
            .kind = N00B_SIMD_REV_SEARCH_KIND_LITERAL,
            .u = { .literal = rev_literal_inner_new(needle, len, tail_offset) },
        };
    }

    size_t num_simd = len < 3 ? len : 3;
    n00b_simd_TeddyMasks *masks = n00b_alloc(n00b_simd_TeddyMasks);

    for (size_t i = 0; i < num_simd; ++i) {
        uint8_t lo[16] = {0};
        uint8_t hi[16] = {0};
        for (size_t k = 0; k < byte_sets_lens[i]; ++k) {
            uint8_t b = byte_sets_raw[i][k];
            lo[b & 0xF] |= 0x80;
            hi[b >> 4] |= 0x80;
        }
        memcpy(&masks->lo[i][0], lo, 16);
        memcpy(&masks->lo[i][16], lo, 16);
        memcpy(&masks->hi[i][0], hi, 16);
        memcpy(&masks->hi[i][16], hi, 16);
    }

    return (n00b_simd_RevTeddySearch){
        .kind = N00B_SIMD_REV_SEARCH_KIND_TEDDY,
        .u = { .teddy = (n00b_simd_RevTeddyInner){
            .len = len,
            .num_simd = num_simd,
            .masks = masks,
            .sets = all_sets,
            .sets_len = all_sets_len,
            .tail_offset = tail_offset,
        } },
    };
}

void n00b_simd_rev_prefix_search_free(n00b_simd_RevTeddySearch *s) {
    if (s->kind == N00B_SIMD_REV_SEARCH_KIND_LITERAL) {
        if (s->u.literal.needle) n00b_free(s->u.literal.needle);
        if (s->u.literal.chunks) n00b_free(s->u.literal.chunks);
        s->u.literal.needle = nullptr;
        s->u.literal.chunks = nullptr;
    } else {
        if (s->u.teddy.masks) n00b_free(s->u.teddy.masks);
        if (s->u.teddy.sets) n00b_free(s->u.teddy.sets);
        s->u.teddy.masks = nullptr;
        s->u.teddy.sets = nullptr;
    }
}

n00b_simd_RevTeddySearch n00b_simd_rev_prefix_search_add_tail_offset(
    n00b_simd_RevTeddySearch s, uint32_t extra) {
    if (s.kind == N00B_SIMD_REV_SEARCH_KIND_LITERAL) {
        s.u.literal.tail_offset += (size_t)extra;
    } else {
        s.u.teddy.tail_offset += (size_t)extra;
    }
    return s;
}

size_t n00b_simd_rev_prefix_search_len_value(const n00b_simd_RevTeddySearch *s) {
    if (s->kind == N00B_SIMD_REV_SEARCH_KIND_LITERAL) return s->u.literal.needle_len;
    return s->u.teddy.len;
}

static n00b_simd_OptUsize rps_verify_tail(const n00b_simd_RevTeddyInner *t,
                                          const uint8_t *haystack, size_t end) {
    size_t min_pos = t->len - 1;
    size_t pos = end;
    for (;;) {
        if (pos < min_pos) return opt_none();
        bool restart = false;
        for (size_t i = 0; i < t->len; ++i) {
            if (!tset_contains_byte(&t->sets[i], haystack[pos - i])) {
                if (pos == min_pos) return opt_none();
                pos -= 1;
                restart = true;
                break;
            }
        }
        if (!restart) return opt_some(pos);
    }
}

[[gnu::always_inline]] static inline n00b_simd_OptUsize
rps_verify_rev_inline(const uint8_t *ptr, size_t chunk_pos, uint16_t bits,
                      const TSet *sets_ptr, size_t len) {
    while (bits != 0) {
        size_t bit = 15u - (size_t)leading_zeros_u16(bits);
        size_t candidate = chunk_pos + bit;
        if (candidate + 1 < len) {
            bits &= (uint16_t)~((uint16_t)1u << bit);
            continue;
        }
        bool ok = true;
        size_t j = 0;
        while (j < len) {
            if (!tset_contains_byte(&sets_ptr[j], ptr[candidate - j])) {
                ok = false;
                break;
            }
            j += 1;
        }
        if (ok) return opt_some(candidate);
        bits &= (uint16_t)~((uint16_t)1u << bit);
    }
    return opt_none();
}

static n00b_simd_OptUsize teddy_rev_1(const n00b_simd_RevTeddyInner *t,
                                      const uint8_t *haystack, size_t hlen, size_t end) {
    (void)hlen;
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(t->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(t->masks->hi[0]);
    const TSet *sets_ptr = t->sets;
    size_t len = t->len;
    size_t min_pos = len - 1;

    if (end < 15 + min_pos) {
        return rps_verify_tail(t, haystack, end);
    }

    size_t chunk_pos = end - 15;

    for (;;) {
        uint8x16_t c0 = vld1q_u8(ptr + chunk_pos);
        uint8x16_t r0 = vandq_u8(
            vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
            vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4)));
        if (vmaxvq_u8(r0) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(r0);
            n00b_simd_OptUsize m = rps_verify_rev_inline(ptr, chunk_pos, mask, sets_ptr, len);
            if (m.found) return m;
        }
        if (chunk_pos < 16 + min_pos) break;
        chunk_pos -= 16;
    }
    size_t tail_end = saturating_sub_size(chunk_pos, 1);
    if (tail_end > end) tail_end = end;
    return rps_verify_tail(t, haystack, tail_end);
}

static n00b_simd_OptUsize teddy_rev_2(const n00b_simd_RevTeddyInner *t,
                                      const uint8_t *haystack, size_t hlen, size_t end) {
    (void)hlen;
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(t->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(t->masks->hi[0]);
    uint8x16_t vlo1 = vld1q_u8(t->masks->lo[1]);
    uint8x16_t vhi1 = vld1q_u8(t->masks->hi[1]);
    const TSet *sets_ptr = t->sets;
    size_t len = t->len;
    size_t min_pos = len - 1;

    if (end < 15 + min_pos) {
        return rps_verify_tail(t, haystack, end);
    }

    size_t chunk_pos = end - 15;

    for (;;) {
        uint8x16_t c0 = vld1q_u8(ptr + chunk_pos);
        uint8x16_t c1 = vld1q_u8(ptr + chunk_pos - 1);
        uint8x16_t r0 = vandq_u8(
            vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
            vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4)));
        uint8x16_t r1 = vandq_u8(
            vqtbl1q_u8(vlo1, vandq_u8(c1, nib)),
            vqtbl1q_u8(vhi1, vshrq_n_u8(c1, 4)));
        uint8x16_t combined = vandq_u8(r0, r1);
        if (vmaxvq_u8(combined) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(combined);
            n00b_simd_OptUsize m = rps_verify_rev_inline(ptr, chunk_pos, mask, sets_ptr, len);
            if (m.found) return m;
        }
        if (chunk_pos < 16 + min_pos) break;
        chunk_pos -= 16;
    }
    size_t tail_end = saturating_sub_size(chunk_pos, 1);
    if (tail_end > end) tail_end = end;
    return rps_verify_tail(t, haystack, tail_end);
}

static n00b_simd_OptUsize teddy_rev_3(const n00b_simd_RevTeddyInner *t,
                                      const uint8_t *haystack, size_t hlen, size_t end) {
    (void)hlen;
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(t->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(t->masks->hi[0]);
    uint8x16_t vlo1 = vld1q_u8(t->masks->lo[1]);
    uint8x16_t vhi1 = vld1q_u8(t->masks->hi[1]);
    uint8x16_t vlo2 = vld1q_u8(t->masks->lo[2]);
    uint8x16_t vhi2 = vld1q_u8(t->masks->hi[2]);
    const TSet *sets_ptr = t->sets;
    size_t len = t->len;
    size_t min_pos = len - 1;

    if (end < 15 + min_pos) {
        return rps_verify_tail(t, haystack, end);
    }

    size_t chunk_pos = end - 15;

    while (chunk_pos >= 32 + min_pos) {
        uint8x16_t c0a = vld1q_u8(ptr + chunk_pos);
        uint8x16_t c1a = vld1q_u8(ptr + chunk_pos - 1);
        uint8x16_t c2a = vld1q_u8(ptr + chunk_pos - 2);
        uint8x16_t ra = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0a, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0a, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1a, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1a, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2a, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2a, 4))));

        uint8x16_t c0b = vld1q_u8(ptr + chunk_pos - 16);
        uint8x16_t c1b = vld1q_u8(ptr + chunk_pos - 17);
        uint8x16_t c2b = vld1q_u8(ptr + chunk_pos - 18);
        uint8x16_t rb = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0b, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0b, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1b, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1b, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2b, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2b, 4))));

        if (vmaxvq_u8(vorrq_u8(ra, rb)) != 0) {
            if (vmaxvq_u8(ra) != 0) {
                uint16_t mask_a = n00b_simd_neon_movemask(ra);
                n00b_simd_OptUsize m = rps_verify_rev_inline(ptr, chunk_pos, mask_a, sets_ptr, len);
                if (m.found) return m;
            }
            if (vmaxvq_u8(rb) != 0) {
                uint16_t mask_b = n00b_simd_neon_movemask(rb);
                n00b_simd_OptUsize m = rps_verify_rev_inline(ptr, chunk_pos - 16, mask_b, sets_ptr, len);
                if (m.found) return m;
            }
        }
        chunk_pos -= 32;
    }

    for (;;) {
        uint8x16_t c0 = vld1q_u8(ptr + chunk_pos);
        uint8x16_t c1 = vld1q_u8(ptr + chunk_pos - 1);
        uint8x16_t c2 = vld1q_u8(ptr + chunk_pos - 2);
        uint8x16_t combined = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2, 4))));
        if (vmaxvq_u8(combined) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(combined);
            n00b_simd_OptUsize m = rps_verify_rev_inline(ptr, chunk_pos, mask, sets_ptr, len);
            if (m.found) return m;
        }
        if (chunk_pos < 16 + min_pos) break;
        chunk_pos -= 16;
    }
    size_t tail_end = saturating_sub_size(chunk_pos, 1);
    if (tail_end > end) tail_end = end;
    return rps_verify_tail(t, haystack, tail_end);
}

n00b_simd_OptUsize n00b_simd_rev_prefix_search_find_rev_raw(
    const n00b_simd_RevTeddySearch *s,
    const uint8_t                  *haystack,
    size_t                          hlen,
    size_t                          end) {
    if (s->kind == N00B_SIMD_REV_SEARCH_KIND_LITERAL) {
        const n00b_simd_RevLiteralInner *l = &s->u.literal;
        size_t e = end;
        size_t cap = saturating_sub_size(hlen, 1);
        if (e > cap) e = cap;
        if (e < l->tail_offset) return opt_none();
        e -= l->tail_offset;
        n00b_simd_OptUsize r = rli_find_rev_neon(l, haystack, e);
        if (r.found) return opt_some(r.value + l->tail_offset);
        return opt_none();
    }

    const n00b_simd_RevTeddyInner *t = &s->u.teddy;
    size_t e = end;
    size_t cap = saturating_sub_size(hlen, 1);
    if (e > cap) e = cap;
    if (e < t->tail_offset) return opt_none();
    e -= t->tail_offset;

    n00b_simd_OptUsize r;
    switch (t->num_simd) {
        case 1: r = teddy_rev_1(t, haystack, hlen, e); break;
        case 2: r = teddy_rev_2(t, haystack, hlen, e); break;
        default: r = teddy_rev_3(t, haystack, hlen, e); break;
    }
    if (r.found) {
        return opt_some(r.value + t->tail_offset);
    }
    return opt_none();
}

// ============================================================================
// FwdPrefixSearch (SIMD-internal Teddy)
// ============================================================================

n00b_simd_FwdPrefixSearchSimd n00b_simd_fwd_prefix_search_new_value(
    size_t                len,
    const size_t         *freq_order,
    size_t                freq_order_len,
    const uint8_t *const *byte_sets_raw,
    const size_t         *byte_sets_lens,
    size_t                byte_sets_count,
    TSet                 *all_sets,
    size_t                all_sets_len) {
    n00b_require(all_sets_len == len,
                 "n00b_simd_fwd_prefix_search_new: all_sets_len must equal len");
    n00b_require(byte_sets_count == len,
                 "n00b_simd_fwd_prefix_search_new: byte_sets_count must equal len");

    n00b_simd_FwdPrefixSearchSimd s;
    size_t num_simd = len < 3 ? len : 3;
    size_t simd_offsets[3] = {0, 0, 0};
    n00b_simd_TeddyMasks *masks = n00b_alloc(n00b_simd_TeddyMasks);

    for (size_t i = 0; i < num_simd; ++i) {
        size_t pos = freq_order[i];
        simd_offsets[i] = pos;
        uint8_t lo[16] = {0};
        uint8_t hi[16] = {0};
        for (size_t k = 0; k < byte_sets_lens[pos]; ++k) {
            uint8_t b = byte_sets_raw[pos][k];
            lo[b & 0xF] |= 0x80;
            hi[b >> 4] |= 0x80;
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

    s.len = len;
    s.num_simd = num_simd;
    memcpy(s.simd_offsets, simd_offsets, sizeof(simd_offsets));
    s.masks = masks;
    s.sets = all_sets;
    s.sets_len = all_sets_len;
    memcpy(s.verify_order, verify_order, sizeof(verify_order));
    return s;
}

void n00b_simd_fwd_prefix_search_free_inner(n00b_simd_FwdPrefixSearchSimd *s) {
    if (s->masks) n00b_free(s->masks);
    if (s->sets) n00b_free(s->sets);
    s->masks = nullptr;
    s->sets = nullptr;
}

size_t n00b_simd_fwd_prefix_search_len_value(const n00b_simd_FwdPrefixSearchSimd *s) {
    return s->len;
}

static n00b_simd_OptUsize fps_verify_tail_fwd(const n00b_simd_FwdPrefixSearchSimd *self,
                                              const uint8_t *haystack, size_t hlen,
                                              size_t start) {
    if (hlen < self->len) return opt_none();
    size_t end = hlen - self->len;
    size_t pos = start;
    while (pos <= end) {
        bool restart = false;
        for (size_t i = 0; i < self->len; ++i) {
            if (!tset_contains_byte(&self->sets[i], haystack[pos + i])) {
                pos += 1;
                restart = true;
                break;
            }
        }
        if (!restart) return opt_some(pos);
    }
    return opt_none();
}

[[gnu::always_inline]] static inline n00b_simd_OptUsize
fps_verify_inline(const uint8_t *ptr, size_t pos, uint16_t bits,
                  const TSet *sets_ptr, size_t len,
                  const uint8_t *verify_order) {
    while (bits != 0) {
        size_t bit = (size_t)trailing_zeros_u16(bits);
        size_t candidate = pos + bit;
        const uint8_t *base = ptr + candidate;
        bool ok = true;
        size_t j = 0;
        while (j < len) {
            size_t idx = (size_t)verify_order[j];
            if (!tset_contains_byte(&sets_ptr[idx], base[idx])) {
                ok = false;
                break;
            }
            j += 1;
        }
        if (ok) return opt_some(candidate);
        bits &= (uint16_t)(bits - 1);
    }
    return opt_none();
}

static n00b_simd_OptUsize teddy_1(const n00b_simd_FwdPrefixSearchSimd *self,
                                  const uint8_t *haystack, size_t hlen, size_t start) {
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(self->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(self->masks->hi[0]);
    size_t off0 = self->simd_offsets[0];
    const TSet *sets_ptr = self->sets;
    size_t len = self->len;

    size_t simd_end = saturating_sub_size(hlen, 15 + self->len - 1);
    size_t pos = start;

    while (pos < simd_end) {
        uint8x16_t c0 = vld1q_u8(ptr + pos + off0);
        uint8x16_t r0 = vandq_u8(
            vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
            vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4)));
        if (vmaxvq_u8(r0) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(r0);
            n00b_simd_OptUsize m = fps_verify_inline(ptr, pos, mask, sets_ptr, len, self->verify_order);
            if (m.found) return m;
        }
        pos += 16;
    }
    return fps_verify_tail_fwd(self, haystack, hlen, pos);
}

static n00b_simd_OptUsize teddy_2(const n00b_simd_FwdPrefixSearchSimd *self,
                                  const uint8_t *haystack, size_t hlen, size_t start) {
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(self->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(self->masks->hi[0]);
    uint8x16_t vlo1 = vld1q_u8(self->masks->lo[1]);
    uint8x16_t vhi1 = vld1q_u8(self->masks->hi[1]);
    size_t off0 = self->simd_offsets[0];
    size_t off1 = self->simd_offsets[1];
    const TSet *sets_ptr = self->sets;
    size_t len = self->len;

    size_t simd_end = saturating_sub_size(hlen, 15 + self->len - 1);
    size_t pos = start;

    while (pos < simd_end) {
        uint8x16_t c0 = vld1q_u8(ptr + pos + off0);
        uint8x16_t c1 = vld1q_u8(ptr + pos + off1);
        uint8x16_t r0 = vandq_u8(
            vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
            vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4)));
        uint8x16_t r1 = vandq_u8(
            vqtbl1q_u8(vlo1, vandq_u8(c1, nib)),
            vqtbl1q_u8(vhi1, vshrq_n_u8(c1, 4)));
        uint8x16_t combined = vandq_u8(r0, r1);
        if (vmaxvq_u8(combined) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(combined);
            n00b_simd_OptUsize m = fps_verify_inline(ptr, pos, mask, sets_ptr, len, self->verify_order);
            if (m.found) return m;
        }
        pos += 16;
    }
    return fps_verify_tail_fwd(self, haystack, hlen, pos);
}

static n00b_simd_OptUsize teddy_3(const n00b_simd_FwdPrefixSearchSimd *self,
                                  const uint8_t *haystack, size_t hlen, size_t start) {
    const uint8_t *ptr = haystack;
    uint8x16_t nib = vdupq_n_u8(0x0F);
    uint8x16_t vlo0 = vld1q_u8(self->masks->lo[0]);
    uint8x16_t vhi0 = vld1q_u8(self->masks->hi[0]);
    uint8x16_t vlo1 = vld1q_u8(self->masks->lo[1]);
    uint8x16_t vhi1 = vld1q_u8(self->masks->hi[1]);
    uint8x16_t vlo2 = vld1q_u8(self->masks->lo[2]);
    uint8x16_t vhi2 = vld1q_u8(self->masks->hi[2]);
    size_t off0 = self->simd_offsets[0];
    size_t off1 = self->simd_offsets[1];
    size_t off2 = self->simd_offsets[2];

    size_t simd_end = saturating_sub_size(hlen, 15 + self->len - 1);
    const TSet *sets_ptr = self->sets;
    size_t len = self->len;
    size_t pos = start;

    while (pos + 16 < simd_end) {
        uint8x16_t c0a = vld1q_u8(ptr + pos + off0);
        uint8x16_t c1a = vld1q_u8(ptr + pos + off1);
        uint8x16_t c2a = vld1q_u8(ptr + pos + off2);
        uint8x16_t ra = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0a, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0a, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1a, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1a, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2a, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2a, 4))));

        uint8x16_t c0b = vld1q_u8(ptr + pos + 16 + off0);
        uint8x16_t c1b = vld1q_u8(ptr + pos + 16 + off1);
        uint8x16_t c2b = vld1q_u8(ptr + pos + 16 + off2);
        uint8x16_t rb = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0b, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0b, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1b, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1b, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2b, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2b, 4))));

        if (vmaxvq_u8(vorrq_u8(ra, rb)) != 0) {
            if (vmaxvq_u8(ra) != 0) {
                uint16_t mask_a = n00b_simd_neon_movemask(ra);
                n00b_simd_OptUsize m = fps_verify_inline(ptr, pos, mask_a, sets_ptr, len, self->verify_order);
                if (m.found) return m;
            }
            if (vmaxvq_u8(rb) != 0) {
                uint16_t mask_b = n00b_simd_neon_movemask(rb);
                n00b_simd_OptUsize m = fps_verify_inline(ptr, pos + 16, mask_b, sets_ptr, len, self->verify_order);
                if (m.found) return m;
            }
        }
        pos += 32;
    }

    while (pos < simd_end) {
        uint8x16_t c0 = vld1q_u8(ptr + pos + off0);
        uint8x16_t c1 = vld1q_u8(ptr + pos + off1);
        uint8x16_t c2 = vld1q_u8(ptr + pos + off2);
        uint8x16_t combined = vandq_u8(
            vandq_u8(
                vandq_u8(
                    vqtbl1q_u8(vlo0, vandq_u8(c0, nib)),
                    vqtbl1q_u8(vhi0, vshrq_n_u8(c0, 4))),
                vandq_u8(
                    vqtbl1q_u8(vlo1, vandq_u8(c1, nib)),
                    vqtbl1q_u8(vhi1, vshrq_n_u8(c1, 4)))),
            vandq_u8(
                vqtbl1q_u8(vlo2, vandq_u8(c2, nib)),
                vqtbl1q_u8(vhi2, vshrq_n_u8(c2, 4))));
        if (vmaxvq_u8(combined) != 0) {
            uint16_t mask = n00b_simd_neon_movemask(combined);
            n00b_simd_OptUsize m = fps_verify_inline(ptr, pos, mask, sets_ptr, len, self->verify_order);
            if (m.found) return m;
        }
        pos += 16;
    }
    return fps_verify_tail_fwd(self, haystack, hlen, pos);
}

n00b_simd_OptUsize n00b_simd_fwd_prefix_search_find_fwd_raw(
    const n00b_simd_FwdPrefixSearchSimd *s,
    const uint8_t                       *haystack,
    size_t                               hlen,
    size_t                               start) {
    switch (s->num_simd) {
        case 1: return teddy_1(s, haystack, hlen, start);
        case 2: return teddy_2(s, haystack, hlen, start);
        default: return teddy_3(s, haystack, hlen, start);
    }
}
