// SIMD prefilter primitives — phase 10 port from resharp-c/src/simd/mod.c.
//
// On aarch64 this TU owns:
//   * `n00b_simd_has_simd`  — runtime detection.
//   * `n00b_simd_FwdRangeSearch_*` — neon-only range searcher.
//   * Engine-facing accel-layer wrappers: `n00b_simd_RevSearchBytes_new`,
//     `n00b_simd_RevSearchRanges_new`, `n00b_simd_FwdLiteralSearch_new`,
//     `n00b_simd_FwdPrefixSearch_new`, `n00b_simd_FwdRangeSearch_new`,
//     `n00b_simd_RevTeddySearch_new`, plus the `find_*` / `_len` accessors
//     the engine's `accel.h` declares.  These wrappers heap-allocate via
//     `n00b_alloc(T)` and forward to the by-value NEON entry points
//     declared in `util/simd/neon.h`.
//
// External symbols carry the `n00b_simd_` prefix per § 19a.  Internal
// `static` helpers keep their resharp-c names.  Libc string ops
// (memcpy/memset) are used directly per D13.

#include "util/simd/mod.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdckdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "adt/option.h"
#include "adt/list.h"
#include "util/assert.h"
#include "util/panic.h"

#include "internal/regex/ids.h"      // TSet (real definition).
#include "internal/regex/solver.h"   // TSet_contains_byte.
#include "internal/regex/accel.h"    // Match (n00b_list_t(Match) wire type).
#include "internal/regex/engine.h"   // U8Pair, ByteRange typedef alignment.
#include "internal/regex/prefix.h"   // ByteVec (engine-side growable byte buffer).

#if defined(__aarch64__)
#include <arm_neon.h>
#include "util/simd/neon.h"
#endif

// ============================================================================
// Local checked-arithmetic helper (mirrors engine.c's safe_mul_sz).
// ============================================================================

[[noreturn]] static inline void simd_capacity_overflow(void) {
    n00b_panic("simd: capacity overflow");
}

static inline size_t simd_mul_sz(size_t a, size_t b) {
    size_t r;
    if (ckd_mul(&r, a, b)) simd_capacity_overflow();
    return r;
}

// ============================================================================
// n00b_simd_has_simd — runtime detection
// ============================================================================

bool n00b_simd_has_simd(void) {
#if defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

// Engine-facing scalar accessor: `simd_BYTE_FREQ(b)`.
uint16_t n00b_simd_byte_freq(uint8_t b) {
    return n00b_simd_BYTE_FREQ[b];
}

#if defined(__aarch64__)

// ============================================================================
// FwdRangeSearch (aarch64) — neon-only range searcher.
// ============================================================================

n00b_simd_FwdRangeSearch n00b_simd_FwdRangeSearch_new_value(
    size_t                       len,
    size_t                       anchor_pos,
    n00b_simd_resharp_ranges_t   ranges,
    n00b_simd_resharp_tset_vec_t sets) {
    n00b_require(ranges.len != 0 && ranges.len <= 3,
                 "n00b_simd_FwdRangeSearch_new: ranges.len must be in 1..=3");
    n00b_require(anchor_pos < len,
                 "n00b_simd_FwdRangeSearch_new: anchor_pos must be < len");
    return (n00b_simd_FwdRangeSearch){
        .len        = len,
        .anchor_pos = anchor_pos,
        .ranges     = ranges,
        .sets       = sets,
    };
}

size_t n00b_simd_FwdRangeSearch_len(const n00b_simd_FwdRangeSearch *self) {
    return self->len;
}

static size_t FwdRangeSearch_verify_tail_fwd(const n00b_simd_FwdRangeSearch *self,
                                             const uint8_t *haystack,
                                             size_t hlen, size_t start) {
    if (hlen < self->len) return SIZE_MAX;
    size_t end = hlen - self->len;
    size_t pos = start;
    while (pos <= end) {
        bool ok = true;
        for (size_t i = 0; i < self->len; ++i) {
            if (!TSet_contains_byte(&self->sets.data[i], haystack[pos + i])) {
                ok = false;
                break;
            }
        }
        if (ok) return pos;
        pos += 1;
    }
    return SIZE_MAX;
}

static size_t FwdRangeSearch_find_fwd_neon(const n00b_simd_FwdRangeSearch *self,
                                           const uint8_t *haystack,
                                           size_t hlen, size_t start) {
    const uint8_t *ptr = haystack;
    size_t n = self->ranges.len;
    size_t anchor = self->anchor_pos;
    uint8x16_t lo0 = vdupq_n_u8(self->ranges.data[0].lo);
    uint8x16_t hi0 = vdupq_n_u8(self->ranges.data[0].hi);

    size_t need = 15u + self->len - 1u;
    size_t simd_end = (hlen >= need) ? (hlen - need) : 0;
    size_t pos = start;

    while (pos < simd_end) {
        uint8x16_t chunk = vld1q_u8(ptr + pos + anchor);
        uint8x16_t in0 = vandq_u8(vcgeq_u8(chunk, lo0), vcleq_u8(chunk, hi0));
        uint8x16_t combined;
        if (n >= 3) {
            uint8x16_t lo1 = vdupq_n_u8(self->ranges.data[1].lo);
            uint8x16_t hi1 = vdupq_n_u8(self->ranges.data[1].hi);
            uint8x16_t lo2 = vdupq_n_u8(self->ranges.data[2].lo);
            uint8x16_t hi2 = vdupq_n_u8(self->ranges.data[2].hi);
            uint8x16_t in1 = vandq_u8(vcgeq_u8(chunk, lo1),
                                      vcleq_u8(chunk, hi1));
            uint8x16_t in2 = vandq_u8(vcgeq_u8(chunk, lo2),
                                      vcleq_u8(chunk, hi2));
            combined = vorrq_u8(in0, vorrq_u8(in1, in2));
        } else if (n >= 2) {
            uint8x16_t lo1 = vdupq_n_u8(self->ranges.data[1].lo);
            uint8x16_t hi1 = vdupq_n_u8(self->ranges.data[1].hi);
            uint8x16_t in1 = vandq_u8(vcgeq_u8(chunk, lo1),
                                      vcleq_u8(chunk, hi1));
            combined = vorrq_u8(in0, in1);
        } else {
            combined = in0;
        }
        uint32_t mask = n00b_simd_neon_movemask(combined);
        while (mask != 0) {
            size_t bit = (size_t)__builtin_ctz(mask);
            size_t candidate = pos + bit;
            bool ok = true;
            for (size_t i = 0; i < self->len; ++i) {
                if (!TSet_contains_byte(&self->sets.data[i],
                                        ptr[candidate + i])) {
                    ok = false;
                    break;
                }
            }
            if (ok) return candidate;
            mask &= mask - 1;
        }
        pos += 16;
    }
    return FwdRangeSearch_verify_tail_fwd(self, haystack, hlen, pos);
}

size_t n00b_simd_FwdRangeSearch_find_fwd(const n00b_simd_FwdRangeSearch *self,
                                         const uint8_t *haystack, size_t hlen,
                                         size_t start) {
    if (start > hlen) return SIZE_MAX;
    return FwdRangeSearch_find_fwd_neon(self, haystack, hlen, start);
}

// ============================================================================
// Engine-facing accel-layer wrappers.  These match the externs in the
// regex engine's `accel.h`, `engine.c`, `bdfa.c`, and `prefix.c`.
// Signatures mirror those externs; OptUsize → n00b_option_t(size_t) is
// done at this boundary.
// ============================================================================

// --- ByteRange aliasing ------------------------------------------------------
// The engine's `ByteRange` (algebra/solver.h: `{uint8_t start; uint8_t end}`)
// is layout-compatible with the SIMD `n00b_simd_ByteRange` (`{uint8_t lo;
// uint8_t hi}`) and with the engine-side `U8Pair`.  We reinterpret across the
// boundary; the engine guarantees the wire layout via static asserts in
// engine.h.

static_assert(sizeof(n00b_simd_ByteRange) == sizeof(U8Pair),
              "n00b_simd_ByteRange must match U8Pair layout");

// === RevSearchBytes ==========================================================

n00b_simd_RevSearchBytes *n00b_simd_RevSearchBytes_new(n00b_list_t(uint8_t) bytes) {
    n00b_simd_RevSearchBytes *p = n00b_alloc(n00b_simd_RevSearchBytes);
    *p = n00b_simd_rev_search_bytes_new_value(bytes.data, bytes.len);
    return p;
}

n00b_option_t(size_t)
n00b_simd_rev_search_bytes_find_fwd(const n00b_simd_RevSearchBytes *s,
                                    const uint8_t *haystack, size_t hlen) {
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_fwd_raw(s, haystack, hlen);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

n00b_option_t(size_t)
n00b_simd_rev_search_bytes_find_rev(const n00b_simd_RevSearchBytes *s,
                                    const uint8_t *haystack, size_t hlen) {
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_rev_raw(s, haystack, hlen);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

// === RevSearchRanges =========================================================

n00b_simd_RevSearchRanges *n00b_simd_RevSearchRanges_new(n00b_list_t(U8Pair) ranges) {
    n00b_simd_RevSearchRanges *p = n00b_alloc(n00b_simd_RevSearchRanges);
    *p = n00b_simd_rev_search_ranges_new_value(
        (const n00b_simd_ByteRange *)ranges.data, ranges.len);
    return p;
}

n00b_option_t(size_t)
n00b_simd_rev_search_ranges_find_fwd(const n00b_simd_RevSearchRanges *s,
                                     const uint8_t *haystack, size_t hlen) {
    n00b_simd_OptUsize r = n00b_simd_rev_search_ranges_find_fwd_raw(s, haystack, hlen);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

n00b_option_t(size_t)
n00b_simd_rev_search_ranges_find_rev(const n00b_simd_RevSearchRanges *s,
                                     const uint8_t *haystack, size_t hlen) {
    n00b_simd_OptUsize r = n00b_simd_rev_search_ranges_find_rev_raw(s, haystack, hlen);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

// === FwdLiteralSearch ========================================================

n00b_simd_FwdLiteralSearch *n00b_simd_FwdLiteralSearch_new(const uint8_t *needle,
                                                            size_t         nlen) {
    n00b_simd_FwdLiteralSearch *p = n00b_alloc(n00b_simd_FwdLiteralSearch);
    *p = n00b_simd_fwd_literal_search_new_value(needle, nlen);
    return p;
}

uint8_t n00b_simd_FwdLiteralSearch_rare_byte(const n00b_simd_FwdLiteralSearch *self) {
    return self->rare_byte;
}

void n00b_simd_FwdLiteralSearch_free(n00b_simd_FwdLiteralSearch *p) {
    if (!p) return;
    n00b_simd_fwd_literal_search_free_inner(p);
    n00b_free(p);
}

n00b_option_t(size_t)
n00b_simd_fwd_literal_search_find_fwd(const n00b_simd_FwdLiteralSearch *s,
                                      const uint8_t *haystack, size_t hlen) {
    n00b_simd_OptUsize r = n00b_simd_fwd_literal_search_find_fwd_raw(s, haystack, hlen);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

// `n00b_simd_fwd_literal_search_find_all_fixed` writes into an
// engine-supplied `n00b_list_t(Match)`.  `Match` (accel.h) is layout-
// compatible with `n00b_simd_MatchPair` (`{size_t start; size_t end}`),
// so we pull matches into a temporary MatchVec then push to the n00b list.

void n00b_simd_fwd_literal_search_find_all_fixed(
    const n00b_simd_FwdLiteralSearch *s,
    const uint8_t                    *haystack,
    size_t                            hlen,
    n00b_list_t(Match)               *out_matches) {
    n00b_simd_MatchVec mv = (n00b_simd_MatchVec){};
    n00b_simd_fwd_literal_search_find_all_fixed_raw(s, haystack, hlen, &mv);
    for (size_t i = 0; i < mv.len; ++i) {
        n00b_list_push(*out_matches,
                       ((Match){.start = mv.data[i].start, .end = mv.data[i].end}));
    }
    if (mv.data) n00b_free(mv.data);
}

// === FwdPrefixSearchSimd (Teddy) =============================================

n00b_simd_FwdPrefixSearchSimd *n00b_simd_FwdPrefixSearch_new(
    size_t                total_len,
    const size_t         *freq_order,
    size_t                freq_order_len,
    const ByteVec        *byte_sets,
    size_t                bs_len,
    const TSet           *all_sets,
    size_t                as_len) {
    const uint8_t **raw = n00b_alloc_array(const uint8_t *, bs_len);
    size_t *lens = n00b_alloc_array(size_t, bs_len);
    for (size_t i = 0; i < bs_len; ++i) {
        raw[i]  = byte_sets[i].data;
        lens[i] = byte_sets[i].len;
    }
    TSet *owned = n00b_alloc_array(TSet, as_len);
    memcpy(owned, all_sets, simd_mul_sz(as_len, sizeof *owned));

    n00b_simd_FwdPrefixSearchSimd *p = n00b_alloc(n00b_simd_FwdPrefixSearchSimd);
    *p = n00b_simd_fwd_prefix_search_new_value(total_len, freq_order,
                                                freq_order_len, raw, lens,
                                                bs_len, owned, as_len);
    n00b_free(raw);
    n00b_free(lens);
    return p;
}

size_t n00b_simd_fwd_prefix_search_simd_len(const n00b_simd_FwdPrefixSearchSimd *p) {
    return n00b_simd_fwd_prefix_search_len_value(p);
}

n00b_option_t(size_t)
n00b_simd_fwd_prefix_search_simd_find_fwd(const n00b_simd_FwdPrefixSearchSimd *p,
                                          const uint8_t *haystack, size_t hlen,
                                          size_t start) {
    n00b_simd_OptUsize r = n00b_simd_fwd_prefix_search_find_fwd_raw(p, haystack, hlen, start);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

// === RevTeddySearch ==========================================================

n00b_simd_RevTeddySearch *n00b_simd_RevTeddySearch_new(
    size_t                num_simd,
    const ByteVec        *window,
    size_t                window_len,
    const TSet           *all_sets,
    size_t                as_len,
    size_t                tail_offset) {
    (void)num_simd;
    const uint8_t **raw = n00b_alloc_array(const uint8_t *, window_len);
    size_t *lens = n00b_alloc_array(size_t, window_len);
    for (size_t i = 0; i < window_len; ++i) {
        raw[i]  = window[i].data;
        lens[i] = window[i].len;
    }
    TSet *owned = n00b_alloc_array(TSet, as_len);
    memcpy(owned, all_sets, simd_mul_sz(as_len, sizeof *owned));

    n00b_simd_RevTeddySearch *p = n00b_alloc(n00b_simd_RevTeddySearch);
    *p = n00b_simd_rev_prefix_search_new_value(window_len, raw, lens,
                                                window_len, owned, as_len,
                                                tail_offset);
    n00b_free(raw);
    n00b_free(lens);
    return p;
}

n00b_option_t(size_t)
n00b_simd_rev_prefix_search_find_rev(const n00b_simd_RevTeddySearch *s,
                                     const uint8_t *haystack, size_t hlen,
                                     size_t end) {
    n00b_simd_OptUsize r = n00b_simd_rev_prefix_search_find_rev_raw(s, haystack, hlen, end);
    if (r.found) return n00b_option_set(size_t, r.value);
    return n00b_option_none(size_t);
}

// === FwdRangeSearch wrappers =================================================

n00b_simd_FwdRangeSearch *n00b_simd_FwdRangeSearch_new(
    size_t          total_len,
    size_t          anchor_pos,
    const uint8_t  *lo,
    const uint8_t  *hi,
    size_t          ranges_len,
    const TSet     *all_sets,
    size_t          all_sets_len) {
    n00b_require(ranges_len != 0 && ranges_len <= 3,
                 "n00b_simd_FwdRangeSearch_new: ranges_len must be in 1..=3");

    n00b_simd_resharp_ranges_t ranges = {};
    ranges.data = n00b_alloc_array(n00b_simd_resharp_range_t, ranges_len);
    ranges.len = ranges.cap = ranges_len;
    for (size_t i = 0; i < ranges_len; ++i) {
        ranges.data[i].lo = lo[i];
        ranges.data[i].hi = hi[i];
    }

    n00b_simd_resharp_tset_vec_t sets = {};
    sets.data = n00b_alloc_array(TSet, all_sets_len);
    sets.len = sets.cap = all_sets_len;
    memcpy(sets.data, all_sets, simd_mul_sz(all_sets_len, sizeof *sets.data));

    n00b_simd_FwdRangeSearch *p = n00b_alloc(n00b_simd_FwdRangeSearch);
    *p = n00b_simd_FwdRangeSearch_new_value(total_len, anchor_pos, ranges, sets);
    return p;
}

size_t n00b_simd_fwd_range_search_len(const n00b_simd_FwdRangeSearch *p) {
    return n00b_simd_FwdRangeSearch_len(p);
}

n00b_option_t(size_t)
n00b_simd_fwd_range_search_find_fwd(const n00b_simd_FwdRangeSearch *p,
                                    const uint8_t *haystack,
                                    size_t haystack_len, size_t start) {
    size_t r = n00b_simd_FwdRangeSearch_find_fwd(p, haystack, haystack_len, start);
    if (r == SIZE_MAX) return n00b_option_none(size_t);
    return n00b_option_set(size_t, r);
}

#else

// Backendless targets still need these symbols because regex object code
// contains calls guarded by n00b_simd_has_simd().  Reverse prefix search is
// also used as a scalar fallback so boundary-stripped reverse accelerators
// continue to work without hardware SIMD.

struct n00b_simd_ByteRange;

struct n00b_simd_RevTeddySearch {
    size_t len;
    size_t tail_offset;
    TSet   sets[3];
};

n00b_simd_RevSearchBytes *n00b_simd_RevSearchBytes_new(n00b_list_t(uint8_t) bytes) {
    (void)bytes;
    return nullptr;
}

n00b_simd_RevSearchRanges *n00b_simd_RevSearchRanges_new(n00b_list_t(U8Pair) ranges) {
    (void)ranges;
    return nullptr;
}

const uint8_t *n00b_simd_rev_search_bytes_bytes(const n00b_simd_RevSearchBytes *s,
                                                size_t *out_len) {
    (void)s;
    if (out_len) *out_len = 0;
    return nullptr;
}

const struct n00b_simd_ByteRange *
n00b_simd_rev_search_ranges_ranges(const n00b_simd_RevSearchRanges *s,
                                   size_t *out_len) {
    (void)s;
    if (out_len) *out_len = 0;
    return nullptr;
}

n00b_option_t(size_t)
n00b_simd_rev_search_bytes_find_fwd(const n00b_simd_RevSearchBytes *s,
                                    const uint8_t *haystack,
                                    size_t hlen) {
    (void)s;
    (void)haystack;
    (void)hlen;
    return n00b_option_none(size_t);
}

n00b_option_t(size_t)
n00b_simd_rev_search_bytes_find_rev(const n00b_simd_RevSearchBytes *s,
                                    const uint8_t *haystack,
                                    size_t hlen) {
    (void)s;
    (void)haystack;
    (void)hlen;
    return n00b_option_none(size_t);
}

n00b_option_t(size_t)
n00b_simd_rev_search_ranges_find_fwd(const n00b_simd_RevSearchRanges *s,
                                     const uint8_t *haystack,
                                     size_t hlen) {
    (void)s;
    (void)haystack;
    (void)hlen;
    return n00b_option_none(size_t);
}

n00b_option_t(size_t)
n00b_simd_rev_search_ranges_find_rev(const n00b_simd_RevSearchRanges *s,
                                     const uint8_t *haystack,
                                     size_t hlen) {
    (void)s;
    (void)haystack;
    (void)hlen;
    return n00b_option_none(size_t);
}

n00b_simd_FwdLiteralSearch *n00b_simd_FwdLiteralSearch_new(const uint8_t *needle,
                                                           size_t nlen) {
    (void)needle;
    (void)nlen;
    return nullptr;
}

uint8_t n00b_simd_FwdLiteralSearch_rare_byte(const n00b_simd_FwdLiteralSearch *self) {
    (void)self;
    return 0;
}

void n00b_simd_FwdLiteralSearch_free(n00b_simd_FwdLiteralSearch *p) {
    (void)p;
}

n00b_option_t(size_t)
n00b_simd_fwd_literal_search_find_fwd(const n00b_simd_FwdLiteralSearch *s,
                                      const uint8_t *haystack,
                                      size_t hlen) {
    (void)s;
    (void)haystack;
    (void)hlen;
    return n00b_option_none(size_t);
}

size_t n00b_simd_fwd_literal_search_len(const n00b_simd_FwdLiteralSearch *s) {
    (void)s;
    return 0;
}

void n00b_simd_fwd_literal_search_find_all_fixed(const n00b_simd_FwdLiteralSearch *s,
                                                 const uint8_t *haystack,
                                                 size_t hlen,
                                                 n00b_list_t(Match) *out_matches) {
    (void)s;
    (void)haystack;
    (void)hlen;
    (void)out_matches;
}

n00b_simd_FwdPrefixSearchSimd *
n00b_simd_FwdPrefixSearch_new(size_t total_len,
                              const size_t *freq_order,
                              size_t freq_order_len,
                              const ByteVec *byte_sets,
                              size_t bs_len,
                              const TSet *all_sets,
                              size_t as_len) {
    (void)total_len;
    (void)freq_order;
    (void)freq_order_len;
    (void)byte_sets;
    (void)bs_len;
    (void)all_sets;
    (void)as_len;
    return nullptr;
}

n00b_option_t(size_t)
n00b_simd_fwd_prefix_search_simd_find_fwd(const n00b_simd_FwdPrefixSearchSimd *p,
                                          const uint8_t *haystack,
                                          size_t hlen,
                                          size_t start) {
    (void)p;
    (void)haystack;
    (void)hlen;
    (void)start;
    return n00b_option_none(size_t);
}

size_t n00b_simd_fwd_prefix_search_simd_len(const n00b_simd_FwdPrefixSearchSimd *p) {
    (void)p;
    return 0;
}

n00b_simd_RevTeddySearch *n00b_simd_RevTeddySearch_new(size_t num_simd,
                                                       const ByteVec *window,
                                                       size_t window_len,
                                                       const TSet *all_sets,
                                                       size_t as_len,
                                                       size_t tail_offset) {
    (void)num_simd;
    (void)window;
    (void)window_len;
    if (!all_sets || as_len < 1 || as_len > 3) return nullptr;

    n00b_simd_RevTeddySearch *s = n00b_alloc(n00b_simd_RevTeddySearch);
    s->len         = as_len;
    s->tail_offset = tail_offset;
    for (size_t i = 0; i < as_len; ++i) {
        s->sets[i] = all_sets[i];
    }
    return s;
}

n00b_option_t(size_t)
n00b_simd_rev_prefix_search_find_rev(const n00b_simd_RevTeddySearch *s,
                                     const uint8_t *haystack,
                                     size_t hlen,
                                     size_t end) {
    if (!s || !haystack || hlen == 0 || s->len == 0) {
        return n00b_option_none(size_t);
    }

    size_t hsat = hlen - 1;
    if (end > hsat) end = hsat;
    if (end < s->tail_offset) return n00b_option_none(size_t);

    size_t pos     = end - s->tail_offset;
    size_t min_pos = s->len - 1;
    if (pos < min_pos) return n00b_option_none(size_t);

    for (;;) {
        bool ok = true;
        for (size_t i = 0; i < s->len; ++i) {
            if (!TSet_contains_byte(&s->sets[i], haystack[pos - i])) {
                ok = false;
                break;
            }
        }
        if (ok) return n00b_option_set(size_t, pos + s->tail_offset);
        if (pos == min_pos) break;
        --pos;
    }

    return n00b_option_none(size_t);
}

n00b_simd_FwdRangeSearch *n00b_simd_FwdRangeSearch_new(size_t total_len,
                                                       size_t anchor_pos,
                                                       const uint8_t *lo,
                                                       const uint8_t *hi,
                                                       size_t ranges_len,
                                                       const TSet *all_sets,
                                                       size_t all_sets_len) {
    (void)total_len;
    (void)anchor_pos;
    (void)lo;
    (void)hi;
    (void)ranges_len;
    (void)all_sets;
    (void)all_sets_len;
    return nullptr;
}

n00b_option_t(size_t)
n00b_simd_fwd_range_search_find_fwd(const n00b_simd_FwdRangeSearch *p,
                                    const uint8_t *haystack,
                                    size_t haystack_len,
                                    size_t start) {
    (void)p;
    (void)haystack;
    (void)haystack_len;
    (void)start;
    return n00b_option_none(size_t);
}

size_t n00b_simd_fwd_range_search_len(const n00b_simd_FwdRangeSearch *p) {
    (void)p;
    return 0;
}

#endif // __aarch64__
