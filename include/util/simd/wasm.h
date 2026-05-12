// WebAssembly SIMD (simd128) backend — phase 10 port.
//
// This file is carried into the n00b tree for completeness but is not
// compiled by the default meson configuration (wasm.c stays commented
// out in src/util/simd/meson.build).  External symbols are prefixed
// with `n00b_simd_` per § 19a.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#else
// Outside wasm32-simd targets `v128_t` is opaque; this file only compiles
// meaningfully under a wasm32 target with simd128 enabled.
typedef struct v128_t v128_t;
#endif

struct TSet;
typedef struct TSet TSet;

typedef struct n00b_simd_TeddyMasks_wasm {
    uint8_t lo[3][32];
    uint8_t hi[3][32];
} n00b_simd_TeddyMasks_wasm;

extern const uint32_t n00b_simd_BYTE_FREQ_wasm[256];
extern bool           TSet_contains_byte(const TSet *self, uint8_t b);

// ---------- FwdLiteralSearch ----------
typedef struct n00b_simd_FwdLiteralSearch_wasm {
    uint8_t  *needle;
    size_t    needle_len;
    uint64_t *chunks;
    size_t    chunks_len;
    size_t    rare_idx;
    uint8_t   rare_byte;
    size_t    confirm_idx;
    uint8_t   confirm_byte;
} n00b_simd_FwdLiteralSearch_wasm;

typedef struct n00b_simd_MatchPair_wasm {
    size_t start;
    size_t end;
} n00b_simd_MatchPair_wasm;

typedef struct n00b_simd_MatchVec_wasm {
    n00b_simd_MatchPair_wasm *data;
    size_t                    len;
    size_t                    cap;
} n00b_simd_MatchVec_wasm;

n00b_simd_FwdLiteralSearch_wasm n00b_simd_FwdLiteralSearch_wasm_new(const uint8_t *needle, size_t needle_len);
void                            n00b_simd_FwdLiteralSearch_wasm_free(n00b_simd_FwdLiteralSearch_wasm *self);
size_t                          n00b_simd_FwdLiteralSearch_wasm_len(const n00b_simd_FwdLiteralSearch_wasm *self);
uint8_t                         n00b_simd_FwdLiteralSearch_wasm_rare_byte(const n00b_simd_FwdLiteralSearch_wasm *self);
size_t                          n00b_simd_FwdLiteralSearch_wasm_find_fwd(const n00b_simd_FwdLiteralSearch_wasm *self,
                                                                         const uint8_t *haystack, size_t hlen);
void                            n00b_simd_FwdLiteralSearch_wasm_find_all_fixed(const n00b_simd_FwdLiteralSearch_wasm *self,
                                                                               const uint8_t *haystack, size_t hlen,
                                                                               n00b_simd_MatchVec_wasm *matches);

// ---------- RevSearchBytes ----------
typedef struct n00b_simd_RevSearchBytes_wasm {
    uint8_t *bytes;
    size_t   bytes_len;
} n00b_simd_RevSearchBytes_wasm;

n00b_simd_RevSearchBytes_wasm n00b_simd_RevSearchBytes_wasm_new(const uint8_t *bytes, size_t bytes_len);
void                          n00b_simd_RevSearchBytes_wasm_free(n00b_simd_RevSearchBytes_wasm *self);
const uint8_t                *n00b_simd_RevSearchBytes_wasm_bytes(const n00b_simd_RevSearchBytes_wasm *self, size_t *out_len);
size_t                        n00b_simd_RevSearchBytes_wasm_find_fwd(const n00b_simd_RevSearchBytes_wasm *self,
                                                                     const uint8_t *haystack, size_t hlen);
size_t                        n00b_simd_RevSearchBytes_wasm_find_rev(const n00b_simd_RevSearchBytes_wasm *self,
                                                                     const uint8_t *haystack, size_t hlen);

// ---------- RevSearchRanges ----------
typedef struct n00b_simd_U8Pair_wasm { uint8_t lo; uint8_t hi; } n00b_simd_U8Pair_wasm;

typedef struct n00b_simd_RevSearchRanges_wasm {
    n00b_simd_U8Pair_wasm *ranges;
    size_t                 ranges_len;
} n00b_simd_RevSearchRanges_wasm;

n00b_simd_RevSearchRanges_wasm n00b_simd_RevSearchRanges_wasm_new(const n00b_simd_U8Pair_wasm *ranges, size_t ranges_len);
void                           n00b_simd_RevSearchRanges_wasm_free(n00b_simd_RevSearchRanges_wasm *self);
const n00b_simd_U8Pair_wasm   *n00b_simd_RevSearchRanges_wasm_ranges(const n00b_simd_RevSearchRanges_wasm *self, size_t *out_len);
size_t                         n00b_simd_RevSearchRanges_wasm_find_fwd(const n00b_simd_RevSearchRanges_wasm *self,
                                                                       const uint8_t *haystack, size_t hlen);
size_t                         n00b_simd_RevSearchRanges_wasm_find_rev(const n00b_simd_RevSearchRanges_wasm *self,
                                                                       const uint8_t *haystack, size_t hlen);

// ---------- FwdRangeSearch ----------
typedef struct n00b_simd_FwdRangeSearch_wasm {
    size_t                 len;
    size_t                 anchor_pos;
    n00b_simd_U8Pair_wasm *ranges;
    size_t                 ranges_len;
    TSet                  *sets;
    size_t                 sets_len;
} n00b_simd_FwdRangeSearch_wasm;

n00b_simd_FwdRangeSearch_wasm n00b_simd_FwdRangeSearch_wasm_new(size_t len, size_t anchor_pos,
                                                                n00b_simd_U8Pair_wasm *ranges, size_t ranges_len,
                                                                TSet *sets, size_t sets_len);
void                          n00b_simd_FwdRangeSearch_wasm_free(n00b_simd_FwdRangeSearch_wasm *self);
size_t                        n00b_simd_FwdRangeSearch_wasm_len(const n00b_simd_FwdRangeSearch_wasm *self);
size_t                        n00b_simd_FwdRangeSearch_wasm_find_fwd(const n00b_simd_FwdRangeSearch_wasm *self,
                                                                     const uint8_t *haystack, size_t hlen,
                                                                     size_t start);

// ---------- RevLiteralInner / RevTeddyInner / RevTeddySearch ----------
typedef struct n00b_simd_RevLiteralInner_wasm {
    uint8_t  *needle;
    size_t    needle_len;
    uint64_t *chunks;
    size_t    chunks_len;
    size_t    rare_idx;
    uint8_t   rare_byte;
    size_t    confirm_idx;
    uint8_t   confirm_byte;
    size_t    tail_offset;
} n00b_simd_RevLiteralInner_wasm;

typedef struct n00b_simd_RevTeddyInner_wasm {
    size_t                     len;
    size_t                     num_simd;
    n00b_simd_TeddyMasks_wasm *masks;
    TSet                      *sets;
    size_t                     sets_len;
    size_t                     tail_offset;
} n00b_simd_RevTeddyInner_wasm;

typedef enum {
    N00B_SIMD_REV_SEARCH_KIND_TEDDY_WASM   = 0,
    N00B_SIMD_REV_SEARCH_KIND_LITERAL_WASM = 1,
} n00b_simd_RevSearchKind_wasm;

typedef struct n00b_simd_RevTeddySearch_wasm {
    n00b_simd_RevSearchKind_wasm kind;
    union {
        n00b_simd_RevTeddyInner_wasm   teddy;
        n00b_simd_RevLiteralInner_wasm literal;
    } u;
} n00b_simd_RevTeddySearch_wasm;

typedef struct n00b_simd_ByteVec_wasm {
    uint8_t *data;
    size_t   len;
} n00b_simd_ByteVec_wasm;

n00b_simd_RevTeddySearch_wasm n00b_simd_rev_prefix_search_wasm_new(size_t len,
                                                                   const n00b_simd_ByteVec_wasm *byte_sets_raw,
                                                                   TSet *all_sets, size_t all_sets_len,
                                                                   size_t tail_offset);
void                          n00b_simd_RevTeddySearch_wasm_free(n00b_simd_RevTeddySearch_wasm *self);
n00b_simd_RevTeddySearch_wasm n00b_simd_rev_prefix_search_wasm_add_tail_offset(n00b_simd_RevTeddySearch_wasm self,
                                                                               uint32_t extra);
size_t                        n00b_simd_rev_prefix_search_wasm_len(const n00b_simd_RevTeddySearch_wasm *self);
size_t                        n00b_simd_rev_prefix_search_wasm_find_rev(const n00b_simd_RevTeddySearch_wasm *self,
                                                                        const uint8_t *haystack, size_t hlen,
                                                                        size_t end);

// ---------- FwdPrefixSearch ----------
typedef struct n00b_simd_FwdPrefixSearch_wasm {
    size_t                     len;
    size_t                     num_simd;
    size_t                     simd_offsets[3];
    n00b_simd_TeddyMasks_wasm *masks;
    TSet                      *sets;
    size_t                     sets_len;
    uint8_t                    verify_order[16];
} n00b_simd_FwdPrefixSearch_wasm;

n00b_simd_FwdPrefixSearch_wasm n00b_simd_FwdPrefixSearch_wasm_new(size_t len,
                                                                  const size_t *freq_order, size_t freq_order_len,
                                                                  const n00b_simd_ByteVec_wasm *byte_sets_raw,
                                                                  TSet *all_sets, size_t all_sets_len);
void                           n00b_simd_fwd_prefix_search_wasm_free(n00b_simd_FwdPrefixSearch_wasm *self);
size_t                         n00b_simd_FwdPrefixSearch_wasm_len(const n00b_simd_FwdPrefixSearch_wasm *self);
size_t                         n00b_simd_FwdPrefixSearch_wasm_find_fwd(const n00b_simd_FwdPrefixSearch_wasm *self,
                                                                       const uint8_t *haystack, size_t hlen,
                                                                       size_t start);
