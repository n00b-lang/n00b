// Phase 11 typed translation of resharp-c/tests/simd_neon_unit_test.c.
//
// Exercises the SIMD primitives (neon_movemask, RevSearchBytes,
// FwdLiteralSearch, FwdPrefixSearch Teddy 1/2/3, RevTeddySearch) directly,
// independent of the engine.  Phase 10 renamed every external SIMD symbol
// to the `n00b_simd_` prefix and the constructor / accessor names were
// reshaped (`*_new` -> `*_new_value`, `*_find_fwd` -> `*_find_fwd_raw`,
// `*_find_rev` -> `*_find_rev_raw`).  The data byte sequences and
// expected match offsets are byte-identical oracles and must not be
// paraphrased.
//
// Test runner: § 7.5 — `static void test_*(void)` driven from `main`,
// bare `assert()`, `[PASS]` printf per case (matches test_regex_seek.c).

#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__aarch64__)
int
main(void)
{
    return 0;
}
#else

#include <arm_neon.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "internal/regex/solver.h"
#include "util/simd/neon.h"

// MatchPair (start, end) lives behind MatchVec.data — for find_all_fixed
// assertions we read it back as plain (start, end) pairs.
typedef struct {
    size_t start;
    size_t end;
} pair_t;

static void
match_vec_init_local(n00b_simd_MatchVec *v)
{
    v->data = nullptr;
    v->len = 0;
    v->cap = 0;
}

static void
match_vec_free_local(n00b_simd_MatchVec *v)
{
    if (v->data) {
        n00b_free(v->data);
    }
    v->data = nullptr;
    v->len = 0;
    v->cap = 0;
}

// Single-byte TSet helpers via algebra/solver tset_from_bytes.
static TSet
ts(uint8_t b)
{
    return tset_from_bytes(&b, 1);
}

static TSet
ts_n(const uint8_t *bs, size_t n)
{
    return tset_from_bytes(bs, n);
}

// ============================================================================
// movemask
// ============================================================================

static void
test_movemask_all_zero(void)
{
    uint8x16_t v = vdupq_n_u8(0);
    assert(n00b_simd_neon_movemask(v) == 0u);
    printf("  [PASS] movemask_all_zero\n");
}

static void
test_movemask_all_ones(void)
{
    uint8x16_t v = vdupq_n_u8(0xFF);
    assert(n00b_simd_neon_movemask(v) == 0xFFFFu);
    printf("  [PASS] movemask_all_ones\n");
}

static void
test_movemask_single_bits(void)
{
    for (uint8_t i = 0; i < 16; ++i) {
        uint8_t arr[16] = {};
        arr[i] = 0xFF;
        uint8x16_t v = vld1q_u8(arr);
        uint16_t m = n00b_simd_neon_movemask(v);
        assert(m == (uint16_t)(1u << i));
    }
    printf("  [PASS] movemask_single_bits\n");
}

static void
test_movemask_0x80(void)
{
    uint8_t arr[16] = {};
    arr[6] = 0x80;
    arr[14] = 0x80;
    uint8x16_t v = vld1q_u8(arr);
    uint16_t m = n00b_simd_neon_movemask(v);
    assert(m == ((1u << 6) | (1u << 14)));
    printf("  [PASS] movemask_0x80\n");
}

static void
test_movemask_compare(void)
{
    static const uint8_t hay[16] = {
        'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o',
        'r', 'l', 'd', '1', '2', '3', '4', '5',
    };
    uint8x16_t v = vld1q_u8(hay);
    uint8x16_t target = vdupq_n_u8('l');
    uint8x16_t cmp = vceqq_u8(v, target);
    uint16_t m = n00b_simd_neon_movemask(cmp);
    // 'l' at positions 2, 3, 9
    assert(m == ((1u << 2) | (1u << 3) | (1u << 9)));
    printf("  [PASS] movemask_compare\n");
}

static void
test_movemask_mixed_values(void)
{
    const uint8_t arr[16] = {
        0x80, 0, 0xFF, 0, 0x80, 0, 0, 0xFE,
        0,    0, 0x80, 0, 0,    0, 0xC0, 0,
    };
    uint8x16_t v = vld1q_u8(arr);
    uint16_t m = n00b_simd_neon_movemask(v);
    // bits 0, 2, 4, 7, 10, 14
    assert(m == ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 7)
                 | (1u << 10) | (1u << 14)));
    printf("  [PASS] movemask_mixed_values\n");
}

// ============================================================================
// RevSearchBytes
// ============================================================================

static void
test_rev_search_bytes_short(void)
{
    uint8_t b = 'c';
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(&b, 1);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, (const uint8_t *)"", 0);
    assert(!r.found);
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, (const uint8_t *)"c", 1);
    assert(r.found && r.value == 0);
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, (const uint8_t *)"abc", 3);
    assert(r.found && r.value == 2);
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, (const uint8_t *)"xxxxx", 5);
    assert(!r.found);
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, (const uint8_t *)"cxxc", 4);
    assert(r.found && r.value == 3);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_bytes_short\n");
}

static void
test_rev_search_bytes_16(void)
{
    uint8_t b = 'Z';
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(&b, 1);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_search_bytes_find_rev_raw(
        &s, (const uint8_t *)"0123456789abcdeZ", 16);
    assert(r.found && r.value == 15);
    r = n00b_simd_rev_search_bytes_find_rev_raw(
        &s, (const uint8_t *)"Z123456789abcdef", 16);
    assert(r.found && r.value == 0);
    r = n00b_simd_rev_search_bytes_find_rev_raw(
        &s, (const uint8_t *)"01234Z6789abcdef", 16);
    assert(r.found && r.value == 5);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_bytes_16\n");
}

static void
test_rev_search_bytes_32(void)
{
    uint8_t b = 'Z';
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(&b, 1);
    static const uint8_t hay[32] = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'Z',
    };
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 32);
    assert(r.found && r.value == 31);
    static const uint8_t hay2[32] = {
        'Z', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', '0', '1', '2', '3', '4', '5',
        '6', '7', '8', '9', '0', '1', '2', '3',
        '4', '5', '6', '7', '8', '9', '0', '1',
    };
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay2, 32);
    assert(r.found && r.value == 0);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_bytes_32\n");
}

static void
test_rev_search_bytes_multi(void)
{
    uint8_t bs[3] = {'a', 'b', 'c'};
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(bs, 3);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_search_bytes_find_rev_raw(
        &s, (const uint8_t *)"xxxxxxxxxxxxxc", 14);
    assert(r.found && r.value == 13);
    r = n00b_simd_rev_search_bytes_find_rev_raw(
        &s, (const uint8_t *)"xxxxxxxxxxxxxxx", 15);
    assert(!r.found);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_bytes_multi\n");
}

static void
test_rev_search_at_chunk_boundaries(void)
{
    uint8_t b = 'Z';
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(&b, 1);
    uint8_t hay[32];
    memset(hay, '.', 32);
    hay[15] = 'Z';
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 32);
    assert(r.found && r.value == 15);
    hay[15] = '.';
    hay[16] = 'Z';
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 32);
    assert(r.found && r.value == 16);
    hay[5] = 'Z'; // two matches — should return the LAST
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 32);
    assert(r.found && r.value == 16);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_at_chunk_boundaries\n");
}

static void
test_rev_search_size_sweep(void)
{
    uint8_t b = 'Z';
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(&b, 1);
    for (size_t size = 1; size <= 80; ++size) {
        uint8_t hay[80];
        memset(hay, '.', size);
        hay[0] = 'Z';
        n00b_simd_OptUsize r =
            n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, size);
        assert(r.found && r.value == 0);
        hay[0] = '.';
        hay[size - 1] = 'Z';
        r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, size);
        assert(r.found && r.value == size - 1);
    }
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_size_sweep\n");
}

static void
test_rev_search_2bytes_long(void)
{
    uint8_t bs[2] = {'X', 'Y'};
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(bs, 2);
    uint8_t hay[64];
    memset(hay, '.', 64);
    hay[50] = 'Y';
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 64);
    assert(r.found && r.value == 50);
    hay[50] = '.';
    hay[10] = 'X';
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 64);
    assert(r.found && r.value == 10);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_2bytes_long\n");
}

static void
test_rev_search_3bytes_long(void)
{
    uint8_t bs[3] = {'X', 'Y', 'Z'};
    n00b_simd_RevSearchBytes s = n00b_simd_rev_search_bytes_new_value(bs, 3);
    uint8_t hay[64];
    memset(hay, '.', 64);
    hay[60] = 'Z';
    n00b_simd_OptUsize r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 64);
    assert(r.found && r.value == 60);
    hay[60] = '.';
    hay[5] = 'X';
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 64);
    assert(r.found && r.value == 5);
    hay[5] = '.';
    hay[30] = 'Y';
    r = n00b_simd_rev_search_bytes_find_rev_raw(&s, hay, 64);
    assert(r.found && r.value == 30);
    n00b_simd_rev_search_bytes_free(&s);
    printf("  [PASS] rev_search_3bytes_long\n");
}

// ============================================================================
// FwdLiteralSearch
// ============================================================================

static void
test_fwd_literal_basic(void)
{
    n00b_simd_FwdLiteralSearch s =
        n00b_simd_fwd_literal_search_new_value((const uint8_t *)"abc", 3);
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_literal_search_find_fwd_raw(
        &s, (const uint8_t *)"xxxabcxxx", 9);
    assert(r.found && r.value == 3);
    r = n00b_simd_fwd_literal_search_find_fwd_raw(&s, (const uint8_t *)"abc", 3);
    assert(r.found && r.value == 0);
    r = n00b_simd_fwd_literal_search_find_fwd_raw(&s, (const uint8_t *)"ab", 2);
    assert(!r.found);
    r = n00b_simd_fwd_literal_search_find_fwd_raw(&s, (const uint8_t *)"", 0);
    assert(!r.found);
    n00b_simd_fwd_literal_search_free_inner(&s);
    printf("  [PASS] fwd_literal_basic\n");
}

static void
test_fwd_literal_long(void)
{
    n00b_simd_FwdLiteralSearch s =
        n00b_simd_fwd_literal_search_new_value((const uint8_t *)"XY", 2);
    uint8_t hay[100];
    memset(hay, '.', 100);
    hay[50] = 'X';
    hay[51] = 'Y';
    n00b_simd_OptUsize r =
        n00b_simd_fwd_literal_search_find_fwd_raw(&s, hay, 100);
    assert(r.found && r.value == 50);
    n00b_simd_fwd_literal_search_free_inner(&s);
    printf("  [PASS] fwd_literal_long\n");
}

static void
test_fwd_literal_all_fixed(void)
{
    n00b_simd_FwdLiteralSearch s =
        n00b_simd_fwd_literal_search_new_value((const uint8_t *)"ab", 2);
    n00b_simd_MatchVec m;
    match_vec_init_local(&m);
    n00b_simd_fwd_literal_search_find_all_fixed_raw(
        &s, (const uint8_t *)"xabxabxab", 9, &m);
    pair_t expected[] = {{1, 3}, {4, 6}, {7, 9}};
    assert(m.len == 3);
    for (size_t i = 0; i < 3; ++i) {
        assert(m.data[i].start == expected[i].start);
        assert(m.data[i].end == expected[i].end);
    }
    match_vec_free_local(&m);
    n00b_simd_fwd_literal_search_free_inner(&s);
    printf("  [PASS] fwd_literal_all_fixed\n");
}

static void
test_fwd_literal_all_fixed_long(void)
{
    n00b_simd_FwdLiteralSearch s =
        n00b_simd_fwd_literal_search_new_value((const uint8_t *)"ab", 2);
    uint8_t hay[60];
    memset(hay, '.', 60);
    hay[14] = 'a';
    hay[15] = 'b';
    hay[30] = 'a';
    hay[31] = 'b';
    hay[46] = 'a';
    hay[47] = 'b';
    n00b_simd_MatchVec m;
    match_vec_init_local(&m);
    n00b_simd_fwd_literal_search_find_all_fixed_raw(&s, hay, 60, &m);
    pair_t expected[] = {{14, 16}, {30, 32}, {46, 48}};
    assert(m.len == 3);
    for (size_t i = 0; i < 3; ++i) {
        assert(m.data[i].start == expected[i].start);
        assert(m.data[i].end == expected[i].end);
    }
    match_vec_free_local(&m);
    n00b_simd_fwd_literal_search_free_inner(&s);
    printf("  [PASS] fwd_literal_all_fixed_long\n");
}

static void
test_fwd_literal_size_sweep(void)
{
    n00b_simd_FwdLiteralSearch s =
        n00b_simd_fwd_literal_search_new_value((const uint8_t *)"XY", 2);
    for (size_t size = 2; size <= 80; ++size) {
        uint8_t hay[80];
        memset(hay, '.', size);
        hay[size - 2] = 'X';
        hay[size - 1] = 'Y';
        n00b_simd_OptUsize r =
            n00b_simd_fwd_literal_search_find_fwd_raw(&s, hay, size);
        assert(r.found && r.value == size - 2);
    }
    n00b_simd_fwd_literal_search_free_inner(&s);
    printf("  [PASS] fwd_literal_size_sweep\n");
}

// ============================================================================
// FwdPrefixSearch — Teddy 1/2/3
// ============================================================================

// Helper: build a Teddy with single-byte sets at each position.
static n00b_simd_FwdPrefixSearchSimd
fwd_prefix_singletons(const uint8_t *needle, size_t len)
{
    const uint8_t *byte_sets_raw[3];
    size_t byte_sets_lens[3];
    TSet *all_sets = n00b_alloc_array(TSet, len);
    for (size_t i = 0; i < len; ++i) {
        byte_sets_raw[i] = &needle[i];
        byte_sets_lens[i] = 1;
        all_sets[i] = ts(needle[i]);
    }
    size_t freq_order[3] = {0, 1, 2};
    return n00b_simd_fwd_prefix_search_new_value(
        len, freq_order, len, byte_sets_raw, byte_sets_lens, len,
        all_sets, len);
}

static n00b_simd_RevTeddySearch
rev_prefix_singletons(const uint8_t *bytes, size_t len)
{
    const uint8_t *byte_sets_raw[3];
    size_t byte_sets_lens[3];
    TSet *all_sets = n00b_alloc_array(TSet, len);
    for (size_t i = 0; i < len; ++i) {
        byte_sets_raw[i] = &bytes[i];
        byte_sets_lens[i] = 1;
        all_sets[i] = ts(bytes[i]);
    }
    return n00b_simd_rev_prefix_search_new_value(
        len, byte_sets_raw, byte_sets_lens, len, all_sets, len, 0);
}

static void
test_fwd_prefix_teddy1(void)
{
    uint8_t a = 'a';
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(&a, 1);
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxaxx", 5, 0);
    assert(r.found && r.value == 2);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxxxx", 5, 0);
    assert(!r.found);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[30] = 'a';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 30);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_teddy1\n");
}

static void
test_fwd_prefix_teddy2(void)
{
    uint8_t ab[2] = {'a', 'b'};
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(ab, 2);
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxabxx", 6, 0);
    assert(r.found && r.value == 2);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxbaxx", 6, 0);
    assert(!r.found);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[30] = 'a';
    hay[31] = 'b';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 30);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_teddy2\n");
}

static void
test_fwd_prefix_teddy3(void)
{
    uint8_t abc[3] = {'a', 'b', 'c'};
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(abc, 3);
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxabcxx", 7, 0);
    assert(r.found && r.value == 2);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(
        &s, (const uint8_t *)"xxacbxx", 7, 0);
    assert(!r.found);
    uint8_t hay50[50];
    memset(hay50, '.', 50);
    hay50[30] = 'a';
    hay50[31] = 'b';
    hay50[32] = 'c';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay50, 50, 0);
    assert(r.found && r.value == 30);
    uint8_t hay100[100];
    memset(hay100, '.', 100);
    hay100[70] = 'a';
    hay100[71] = 'b';
    hay100[72] = 'c';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay100, 100, 0);
    assert(r.found && r.value == 70);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_teddy3\n");
}

static void
test_rev_prefix_teddy1(void)
{
    uint8_t c = 'c';
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(&c, 1);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_prefix_search_find_rev_raw(
        &s, (const uint8_t *)"xxcxx", 5, 4);
    assert(r.found && r.value == 2);
    r = n00b_simd_rev_prefix_search_find_rev_raw(
        &s, (const uint8_t *)"xxxxx", 5, 4);
    assert(!r.found);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[30] = 'c';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 30);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_teddy1\n");
}

static void
test_rev_prefix_teddy2(void)
{
    uint8_t cb[2] = {'c', 'b'}; // rev order: needle is "bc"
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(cb, 2);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_prefix_search_find_rev_raw(
        &s, (const uint8_t *)"xxbcxx", 6, 5);
    assert(r.found && r.value == 3);
    r = n00b_simd_rev_prefix_search_find_rev_raw(
        &s, (const uint8_t *)"xxxcxx", 6, 5);
    assert(!r.found);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[29] = 'b';
    hay[30] = 'c';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 30);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_teddy2\n");
}

static void
test_rev_prefix_teddy3(void)
{
    uint8_t cba[3] = {'c', 'b', 'a'}; // rev order: needle is "abc"
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(cba, 3);
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_prefix_search_find_rev_raw(
        &s, (const uint8_t *)"xxabcxx", 7, 6);
    assert(r.found && r.value == 4);
    uint8_t hay50[50];
    memset(hay50, '.', 50);
    hay50[28] = 'a';
    hay50[29] = 'b';
    hay50[30] = 'c';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay50, 50, 49);
    assert(r.found && r.value == 30);
    uint8_t hay100[100];
    memset(hay100, '.', 100);
    hay100[68] = 'a';
    hay100[69] = 'b';
    hay100[70] = 'c';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay100, 100, 99);
    assert(r.found && r.value == 70);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_teddy3\n");
}

static void
test_fwd_prefix_char_class(void)
{
    uint8_t digits[10];
    for (int i = 0; i < 10; ++i) {
        digits[i] = (uint8_t)('0' + i);
    }
    const uint8_t *byte_sets_raw[1] = {digits};
    size_t byte_sets_lens[1] = {10};
    TSet *all_sets = n00b_alloc_array(TSet, 1);
    all_sets[0] = ts_n(digits, 10);
    size_t freq_order[1] = {0};
    n00b_simd_FwdPrefixSearchSimd s = n00b_simd_fwd_prefix_search_new_value(
        1, freq_order, 1, byte_sets_raw, byte_sets_lens, 1, all_sets, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[25] = '5';
    n00b_simd_OptUsize r =
        n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 25);
    for (uint8_t d = '0'; d <= '9'; ++d) {
        hay[25] = d;
        r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
        assert(r.found && r.value == 25);
    }
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_char_class\n");
}

static void
test_rev_prefix_char_class(void)
{
    uint8_t vowels[5] = {'a', 'e', 'i', 'o', 'u'};
    const uint8_t *byte_sets_raw[1] = {vowels};
    size_t byte_sets_lens[1] = {5};
    TSet *all_sets = n00b_alloc_array(TSet, 1);
    all_sets[0] = ts_n(vowels, 5);
    n00b_simd_RevTeddySearch s = n00b_simd_rev_prefix_search_new_value(
        1, byte_sets_raw, byte_sets_lens, 1, all_sets, 1, 0);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[35] = 'o';
    n00b_simd_OptUsize r =
        n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 35);
    for (size_t k = 0; k < 5; ++k) {
        hay[35] = vowels[k];
        r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
        assert(r.found && r.value == 35);
    }
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_char_class\n");
}

static void
test_fwd_prefix_at_chunk_boundaries(void)
{
    uint8_t X = 'X';
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(&X, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[15] = 'X';
    n00b_simd_OptUsize r =
        n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 15);
    hay[15] = '.';
    hay[16] = 'X';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 16);
    hay[16] = '.';
    hay[31] = 'X';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 31);
    hay[31] = '.';
    hay[32] = 'X';
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 32);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_at_chunk_boundaries\n");
}

static void
test_rev_prefix_at_chunk_boundaries(void)
{
    uint8_t X = 'X';
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(&X, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[15] = 'X';
    n00b_simd_OptUsize r =
        n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 15);
    hay[15] = '.';
    hay[16] = 'X';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 16);
    hay[16] = '.';
    hay[31] = 'X';
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 31);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_at_chunk_boundaries\n");
}

static void
test_fwd_prefix_size_sweep(void)
{
    uint8_t ab[2] = {'a', 'b'};
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(ab, 2);
    for (size_t size = 3; size <= 80; ++size) {
        uint8_t hay[80];
        memset(hay, '.', size);
        hay[size - 3] = 'a';
        hay[size - 2] = 'b';
        n00b_simd_OptUsize r =
            n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, size, 0);
        assert(r.found && r.value == size - 3);
    }
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_size_sweep\n");
}

static void
test_rev_prefix_size_sweep(void)
{
    uint8_t cb[2] = {'c', 'b'};
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(cb, 2);
    for (size_t size = 3; size <= 80; ++size) {
        uint8_t hay[80];
        memset(hay, '.', size);
        hay[1] = 'b';
        hay[2] = 'c';
        n00b_simd_OptUsize r = n00b_simd_rev_prefix_search_find_rev_raw(
            &s, hay, size, size - 1);
        assert(r.found && r.value == 2);
    }
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_size_sweep\n");
}

static void
test_fwd_prefix_with_start_offset(void)
{
    uint8_t a = 'a';
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(&a, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[10] = 'a';
    hay[30] = 'a';
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 10);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 11);
    assert(r.found && r.value == 30);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 31);
    assert(!r.found);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_with_start_offset\n");
}

static void
test_fwd_prefix_no_nibble_collision(void)
{
    // 'a' = 0x61; 'q' = 0x71, '1' = 0x31, 'A' = 0x41 share low nibble (1)
    uint8_t a = 'a';
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(&a, 1);
    uint8_t hay[50];
    memset(hay, 'q', 50);
    n00b_simd_OptUsize r =
        n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(!r.found);
    memset(hay, '1', 50);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(!r.found);
    memset(hay, 'A', 50);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(!r.found);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_no_nibble_collision\n");
}

static void
test_rev_prefix_no_nibble_collision(void)
{
    uint8_t c = 'c'; // 0x63 vs 's' = 0x73 (same low nibble)
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(&c, 1);
    uint8_t hay[50];
    memset(hay, 's', 50);
    n00b_simd_OptUsize r =
        n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(!r.found);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_no_nibble_collision\n");
}

static void
test_rev_prefix_finds_last(void)
{
    uint8_t X = 'X';
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(&X, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[10] = 'X';
    hay[20] = 'X';
    hay[40] = 'X';
    n00b_simd_OptUsize r;
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 49);
    assert(r.found && r.value == 40);
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 39);
    assert(r.found && r.value == 20);
    r = n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 50, 19);
    assert(r.found && r.value == 10);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_finds_last\n");
}

static void
test_fwd_prefix_finds_first(void)
{
    uint8_t X = 'X';
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(&X, 1);
    uint8_t hay[50];
    memset(hay, '.', 50);
    hay[10] = 'X';
    hay[20] = 'X';
    hay[40] = 'X';
    n00b_simd_OptUsize r;
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 0);
    assert(r.found && r.value == 10);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 11);
    assert(r.found && r.value == 20);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 21);
    assert(r.found && r.value == 40);
    r = n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 50, 41);
    assert(!r.found);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_finds_first\n");
}

static void
test_fwd_prefix_teddy3_second_chunk(void)
{
    uint8_t abc[3] = {'a', 'b', 'c'};
    n00b_simd_FwdPrefixSearchSimd s = fwd_prefix_singletons(abc, 3);
    uint8_t hay[48];
    memset(hay, '.', 48);
    hay[20] = 'a';
    hay[21] = 'b';
    hay[22] = 'c';
    n00b_simd_OptUsize r =
        n00b_simd_fwd_prefix_search_find_fwd_raw(&s, hay, 48, 0);
    assert(r.found && r.value == 20);
    n00b_simd_fwd_prefix_search_free_inner(&s);
    printf("  [PASS] fwd_prefix_teddy3_second_chunk\n");
}

static void
test_rev_prefix_teddy3_second_chunk(void)
{
    uint8_t cba[3] = {'c', 'b', 'a'};
    n00b_simd_RevTeddySearch s = rev_prefix_singletons(cba, 3);
    uint8_t hay[80];
    memset(hay, '.', 80);
    hay[20] = 'a';
    hay[21] = 'b';
    hay[22] = 'c';
    n00b_simd_OptUsize r =
        n00b_simd_rev_prefix_search_find_rev_raw(&s, hay, 80, 79);
    assert(r.found && r.value == 22);
    n00b_simd_rev_prefix_search_free(&s);
    printf("  [PASS] rev_prefix_teddy3_second_chunk\n");
}

// ============================================================================
// runner
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running SIMD/NEON unit tests...\n");

    test_movemask_all_zero();
    test_movemask_all_ones();
    test_movemask_single_bits();
    test_movemask_0x80();
    test_movemask_compare();
    test_movemask_mixed_values();

    test_rev_search_bytes_short();
    test_rev_search_bytes_16();
    test_rev_search_bytes_32();
    test_rev_search_bytes_multi();
    test_rev_search_at_chunk_boundaries();
    test_rev_search_size_sweep();
    test_rev_search_2bytes_long();
    test_rev_search_3bytes_long();

    test_fwd_literal_basic();
    test_fwd_literal_long();
    test_fwd_literal_all_fixed();
    test_fwd_literal_all_fixed_long();
    test_fwd_literal_size_sweep();

    test_fwd_prefix_teddy1();
    test_fwd_prefix_teddy2();
    test_fwd_prefix_teddy3();
    test_rev_prefix_teddy1();
    test_rev_prefix_teddy2();
    test_rev_prefix_teddy3();
    test_fwd_prefix_char_class();
    test_rev_prefix_char_class();
    test_fwd_prefix_at_chunk_boundaries();
    test_rev_prefix_at_chunk_boundaries();
    test_fwd_prefix_size_sweep();
    test_rev_prefix_size_sweep();
    test_fwd_prefix_with_start_offset();
    test_fwd_prefix_no_nibble_collision();
    test_rev_prefix_no_nibble_collision();
    test_rev_prefix_finds_last();
    test_fwd_prefix_finds_first();
    test_fwd_prefix_teddy3_second_chunk();
    test_rev_prefix_teddy3_second_chunk();

    printf("All SIMD/NEON unit tests passed.\n");
    n00b_shutdown();
    return 0;
}

#endif // __aarch64__
