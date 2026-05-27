// SIMD prefilter primitives — phase 10 port from resharp-c.
//
// On aarch64 this header declares the FwdRangeSearch struct (the only
// SIMD type defined in mod.c on this arch) plus the cross-arch runtime
// detection.  The Teddy-style FwdPrefixSearch / FwdLiteralSearch /
// RevTeddySearch / RevSearchBytes / RevSearchRanges live in neon.h on
// aarch64.
//
// External symbols are prefixed with `n00b_simd_` per § 19a; internal
// `static` helpers keep their resharp-c names.  Callers (regex engine
// TUs) reference these via `internal/regex/accel.h` aliases.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// `TSet` comes from the algebra side; declared opaquely here so the SIMD
// public surface compiles without dragging in algebra.h transitively.
struct TSet;
typedef struct TSet TSet;

// Externally-visible: runtime SIMD detection.
bool n00b_simd_has_simd(void);

// Externally-visible: per-byte frequency table (definition in byte_freq.c).
extern const uint16_t n00b_simd_BYTE_FREQ[256];

// ---- u8 dynamic byte vector (mirrors Rust Vec<u8>) ---------------------
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} n00b_simd_resharp_bytes_t;

// ---- u64 dynamic vector ------------------------------------------------
typedef struct {
    uint64_t *data;
    size_t    len;
    size_t    cap;
} n00b_simd_resharp_u64vec_t;

// ---- (u8,u8) range vector ----------------------------------------------
typedef struct {
    uint8_t lo;
    uint8_t hi;
} n00b_simd_resharp_range_t;

typedef struct {
    n00b_simd_resharp_range_t *data;
    size_t                     len;
    size_t                     cap;
} n00b_simd_resharp_ranges_t;

// ---- TSet vector --------------------------------------------------------
typedef struct {
    TSet  *data;
    size_t len;
    size_t cap;
} n00b_simd_resharp_tset_vec_t;

// ---- (usize,usize) match-pair vector -----------------------------------
typedef struct {
    size_t start;
    size_t end;
} n00b_simd_resharp_match_t;

typedef struct {
    n00b_simd_resharp_match_t *data;
    size_t                     len;
    size_t                     cap;
} n00b_simd_resharp_matches_t;

// ---- byte_sets_raw analogue: slice of Vec<u8> --------------------------
typedef struct {
    const n00b_simd_resharp_bytes_t *data;
    size_t                           len;
} n00b_simd_resharp_bytes_slice_t;

#if defined(__aarch64__)

// FwdRangeSearch lives in mod.c on aarch64 (the Teddy-style structs live
// in neon.h).  Layout mirrors resharp-c's aarch64 path verbatim.
typedef struct n00b_simd_FwdRangeSearch {
    size_t                       len;
    size_t                       anchor_pos;
    n00b_simd_resharp_ranges_t   ranges;
    n00b_simd_resharp_tset_vec_t sets;
} n00b_simd_FwdRangeSearch;

n00b_simd_FwdRangeSearch n00b_simd_FwdRangeSearch_new_value(
    size_t                       len,
    size_t                       anchor_pos,
    n00b_simd_resharp_ranges_t   ranges,
    n00b_simd_resharp_tset_vec_t sets);

size_t n00b_simd_FwdRangeSearch_len(const n00b_simd_FwdRangeSearch *self);
size_t n00b_simd_FwdRangeSearch_find_fwd(
    const n00b_simd_FwdRangeSearch *self,
    const uint8_t                  *haystack,
    size_t                          hlen,
    size_t                          start);

// neon-side helper re-exported from neon.h.
extern uint16_t n00b_simd_neon_movemask(uint8x16_t v);

#endif // __aarch64__
