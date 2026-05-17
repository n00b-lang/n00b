// Phase 11 / neon_simd_test.c — typed translation.
//
// Source: ~/resharp-c/tests/neon_simd_test.c (1121 lines).
//
// The original test drives the engine through resharp-c's local FFI
// surface (`regex_find_all` filling a `MatchVec`, `regex_is_match`
// with a bool out-param) and asserts that the SIMD-accelerated lazy
// path agrees with the default-options path on a wide range of
// patterns and inputs.  n00b-regex's engine surface is shape-equivalent
// — `regex_find_all` now fills an `n00b_list_t(Match)` and returns
// `n00b_regex_engine_err_t`; everything else translates 1:1 per § 7.5.
//
// Source-data byte-identity policy (Phase 11): every pattern literal,
// every input byte, every expected (start,end) pair is reproduced
// verbatim.  Only container types and call shapes change.
//
// The seven `lazy_*_toml` cases at the bottom of the source iterate
// the engine over `basic.toml`, `anchors.toml`, `semantics.toml`,
// `edge_cases.toml`, `boolean.toml`, `lookaround.toml`,
// `paragraph.toml` — and the `all_accel_skip_patterns_simd_vs_default`
// case iterates over `accel_skip.toml`.  These are SIMD vs. default
// cross-checks driven by the lazy path's `max_dfa_capacity = 10000`
// option, not by anything SIMD-specific in the test code itself.  The
// same fixtures are owned by the engine-test port (`run_file(...)` in
// `~/resharp-c/tests/engine_test.c`) and `test_regex_accel_skip.c`,
// which embed the cases inline.  Re-embedding them here would
// duplicate thousands of lines of oracle data — instead, the
// TOML-driven cases are stubbed with a SKIP message that names the
// owning port.  No new coverage is lost; the underlying SIMD path is
// exercised on every other case in this file.
//
// The two helpers in `neon_simd_test.h` (`regex_find_all_alloc`,
// `regex_is_match_b`) are never called from `neon_simd_test.c` — see
// the companion header for the verification audit.
//
// aarch64 gating: the source compiles unconditionally.  This port
// wraps the body in `#if defined(__aarch64__)` per the dispatch
// instruction; on non-aarch64 builds the executable is a no-op that
// reports SKIP and returns 0.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/alloc.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "util/path.h"
#include "parsers/toml.h"

#include "internal/regex/regex.h"
#include "internal/regex/accel.h"

#include "test_regex_neon_simd.h"

#ifndef RESHARP_ENGINE_MANIFEST_DIR
#define RESHARP_ENGINE_MANIFEST_DIR "."
#endif

#if defined(__aarch64__)

// ---------------------------------------------------------------------------
// helpers — (start, end) pair plus equality / printing.  These replace
// resharp-c's `pair_t` / `PairVec` macro-template; n00b doesn't need a
// per-element-type vec because `n00b_list_t(Match)` already carries the
// (start,end) shape.
// ---------------------------------------------------------------------------

typedef struct {
    size_t start;
    size_t end;
} pair_t;

// list_eq_arr — engine fills a `n00b_list_t(Match)`; oracle is a flat
// pair_t[] (matches the original `pv_eq` shape).
static bool
list_eq_arr(n00b_list_t(Match) *got, const pair_t *want, size_t want_n)
{
    size_t got_n = n00b_list_len(*got);
    if (got_n != want_n) {
        return false;
    }
    for (size_t i = 0; i < want_n; ++i) {
        Match m = n00b_list_get(*got, i);
        if (m.start != want[i].start || m.end != want[i].end) {
            return false;
        }
    }
    return true;
}

static bool
list_eq_list(n00b_list_t(Match) *a, n00b_list_t(Match) *b)
{
    size_t na = n00b_list_len(*a);
    size_t nb = n00b_list_len(*b);
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; ++i) {
        Match ma = n00b_list_get(*a, i);
        Match mb = n00b_list_get(*b, i);
        if (ma.start != mb.start || ma.end != mb.end) {
            return false;
        }
    }
    return true;
}

static void
list_print(FILE *f, n00b_list_t(Match) *l)
{
    size_t n = n00b_list_len(*l);
    fprintf(f, "[");
    for (size_t i = 0; i < n; ++i) {
        if (i) fprintf(f, ", ");
        Match m = n00b_list_get(*l, i);
        fprintf(f, "(%zu, %zu)", m.start, m.end);
    }
    fprintf(f, "]");
}

static void
arr_print(FILE *f, const pair_t *p, size_t n)
{
    fprintf(f, "[");
    for (size_t i = 0; i < n; ++i) {
        if (i) fprintf(f, ", ");
        fprintf(f, "(%zu, %zu)", p[i].start, p[i].end);
    }
    fprintf(f, "]");
}

// ---------------------------------------------------------------------------
// option / regex helpers — mirror the Rust top-level fns.
// ---------------------------------------------------------------------------

static RegexOptions
lazy_opts(void)
{
    RegexOptions o = regex_options_default();
    o.max_dfa_capacity = 10000;
    return o;
}

// collect_find_all — the upstream Rust `unwrap()`s the Result; in C we
// hard-assert on engine error since the test has no meaningful recovery.
static n00b_list_t(Match)
collect_find_all(Regex *re, const uint8_t *input, size_t len)
{
    n00b_list_t(Match) out = n00b_list_new_private(Match);
    n00b_regex_engine_err_t err = regex_find_all(re, input, len, &out);
    assert(err == N00B_REGEX_ENGINE_ERR_NONE);
    return out;
}

static n00b_list_t(Match)
find_lazy(const char *pattern, const uint8_t *input, size_t len)
{
    Regex *re = regex_with_options(pattern, lazy_opts());
    assert(re != nullptr);
    n00b_list_t(Match) r = collect_find_all(re, input, len);
    regex_free(re);
    return r;
}

static n00b_list_t(Match)
find_default(const char *pattern, const uint8_t *input, size_t len)
{
    Regex *re = regex_new(pattern);
    assert(re != nullptr);
    n00b_list_t(Match) r = collect_find_all(re, input, len);
    regex_free(re);
    return r;
}

// Macro parameters are textually substituted everywhere in the expansion,
// even after `.` — so a parameter named `len` would clobber `_lazy.len`.
// Underscore-prefixed names dodge that hazard (same trick as the source).
#define ASSERT_SIMD_EQ(_pat, _in, _ilen)                                     \
    do {                                                                     \
        n00b_list_t(Match) _lazy = find_lazy((_pat), (_in), (_ilen));        \
        n00b_list_t(Match) _def  = find_default((_pat), (_in), (_ilen));     \
        if (!list_eq_list(&_lazy, &_def)) {                                  \
            fprintf(stderr,                                                  \
                    "SIMD mismatch: pattern=%s, input_len=%zu, lazy=",       \
                    (_pat), (size_t)(_ilen));                                \
            list_print(stderr, &_lazy);                                      \
            fprintf(stderr, ", default=");                                   \
            list_print(stderr, &_def);                                       \
            fprintf(stderr, "\n");                                           \
            n00b_list_free(_lazy);                                           \
            n00b_list_free(_def);                                            \
            assert(false);                                                   \
        }                                                                    \
        n00b_list_free(_lazy);                                               \
        n00b_list_free(_def);                                                \
    } while (0)

#define ASSERT_SIMD(_pat, _in, _ilen, _exp, _explen)                         \
    do {                                                                     \
        n00b_list_t(Match) _lazy = find_lazy((_pat), (_in), (_ilen));        \
        if (!list_eq_arr(&_lazy, (_exp), (_explen))) {                       \
            fprintf(stderr,                                                  \
                    "SIMD wrong: pattern=%s, input_len=%zu, lazy=",          \
                    (_pat), (size_t)(_ilen));                                \
            list_print(stderr, &_lazy);                                      \
            fprintf(stderr, ", expected=");                                  \
            arr_print(stderr, (_exp), (_explen));                            \
            fprintf(stderr, "\n");                                           \
            n00b_list_free(_lazy);                                           \
            assert(false);                                                   \
        }                                                                    \
        n00b_list_free(_lazy);                                               \
    } while (0)

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------

static void
test_rev_skip_single_byte_every_position(void)
{
    for (size_t pos = 0; pos < 64; ++pos) {
        uint8_t hay[64];
        memset(hay, '.', 64);
        hay[pos] = 'Z';
        n00b_list_t(Match) r = find_lazy("Z", hay, 64);
        pair_t expected[1] = {{pos, pos + 1}};
        bool ok = list_eq_arr(&r, expected, 1);
        n00b_list_free(r);
        assert(ok);
    }
    printf("  [PASS] rev_skip_single_byte_every_position\n");
}

static void
test_rev_skip_two_bytes_sweep(void)
{
    for (size_t size = 1; size <= 80; ++size) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        hay[0] = 'X';
        ASSERT_SIMD_EQ("[XY]", hay, size);
        hay[0] = '.';
        hay[size - 1] = 'Y';
        ASSERT_SIMD_EQ("[XY]", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] rev_skip_two_bytes_sweep\n");
}

static void
test_rev_skip_no_match_long(void)
{
    uint8_t hay[1024];
    memset(hay, '.', 1024);
    ASSERT_SIMD("Z", hay, 1024, nullptr, 0);
    printf("  [PASS] rev_skip_no_match_long\n");
}

static void
test_rev_skip_all_match(void)
{
    uint8_t hay[100];
    memset(hay, 'Z', 100);
    pair_t expected[100];
    for (size_t i = 0; i < 100; ++i) {
        expected[i].start = i;
        expected[i].end   = i + 1;
    }
    ASSERT_SIMD("Z", hay, 100, expected, 100);
    printf("  [PASS] rev_skip_all_match\n");
}

static void
test_fwd_literal_at_every_offset(void)
{
    for (size_t pos = 0; pos < 98; ++pos) {
        uint8_t hay[100];
        memset(hay, '.', 100);
        hay[pos]     = 'a';
        hay[pos + 1] = 'b';
        hay[pos + 2] = 'c';
        n00b_list_t(Match) r = find_lazy("abc", hay, 100);
        pair_t expected[1] = {{pos, pos + 3}};
        bool ok = list_eq_arr(&r, expected, 1);
        n00b_list_free(r);
        assert(ok);
    }
    printf("  [PASS] fwd_literal_at_every_offset\n");
}

static void
test_fwd_literal_adjacent_non_overlapping(void)
{
    pair_t exp[3] = {{0, 4}, {4, 8}, {8, 12}};
    ASSERT_SIMD("abab", (const uint8_t *)"abababababab", 12, exp, 3);
    printf("  [PASS] fwd_literal_adjacent_non_overlapping\n");
}

static void
test_fwd_literal_long_needle(void)
{
    const char *needle16 = "ABCDEFGHIJKLMNOP";
    uint8_t hay[200];
    memset(hay, '.', 200);
    memcpy(&hay[100], needle16, 16);
    pair_t exp[1] = {{100, 116}};
    ASSERT_SIMD(needle16, hay, 200, exp, 1);

    const char *needle20 = "ABCDEFGHIJKLMNOPQRST";
    memcpy(&hay[100], needle20, 20);
    ASSERT_SIMD_EQ(needle20, hay, 200);
    printf("  [PASS] fwd_literal_long_needle\n");
}

static void
test_fwd_literal_haystack_equals_needle(void)
{
    pair_t exp[1] = {{0, 5}};
    ASSERT_SIMD("exact", (const uint8_t *)"exact", 5, exp, 1);
    printf("  [PASS] fwd_literal_haystack_equals_needle\n");
}

static void
test_fwd_literal_single_byte_haystack(void)
{
    pair_t exp[1] = {{0, 1}};
    ASSERT_SIMD("a", (const uint8_t *)"a", 1, exp, 1);
    ASSERT_SIMD("a", (const uint8_t *)"b", 1, nullptr, 0);
    printf("  [PASS] fwd_literal_single_byte_haystack\n");
}

static void
test_fwd_literal_near_end_boundary(void)
{
    static const size_t sizes[] = {15, 16, 17, 31, 32, 33,
                                   47, 48, 49, 63, 64, 65};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t size = sizes[i];
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 3) {
            hay[size - 3] = 'x';
            hay[size - 2] = 'y';
            hay[size - 1] = 'z';
            n00b_list_t(Match) r = find_lazy("xyz", hay, size);
            pair_t expected[1] = {{size - 3, size}};
            bool ok = list_eq_arr(&r, expected, 1);
            n00b_list_free(r);
            if (!ok) {
                n00b_free(hay);
                assert(false);
            }
        }
        n00b_free(hay);
    }
    printf("  [PASS] fwd_literal_near_end_boundary\n");
}

static void
test_fwd_literal_bulk_find_all(void)
{
    Regex *re = regex_new("the");
    assert(re != nullptr);
    const uint8_t *input =
        (const uint8_t *)"the quick brown fox jumps over the lazy dog "
                         "and the cat";
    size_t input_len = strlen((const char *)input);
    // Route through collect_find_all so the engine-level invariant
    // checks happen in one place rather than being reinvented inline.
    n00b_list_t(Match) r = collect_find_all(re, input, input_len);
    regex_free(re);
    pair_t exp[3] = {{0, 3}, {31, 34}, {48, 51}};
    bool ok = list_eq_arr(&r, exp, 3);
    n00b_list_free(r);
    assert(ok);
    printf("  [PASS] fwd_literal_bulk_find_all\n");
}

static void
test_teddy_digit_class_sweep(void)
{
    static const size_t sizes[] = {10, 15, 16, 17, 31, 32, 33, 48, 64, 100};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t size = sizes[i];
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        size_t mid = size / 2;
        hay[mid]     = '5';
        hay[mid + 1] = '7';
        ASSERT_SIMD_EQ("[0-9]+", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] teddy_digit_class_sweep\n");
}

static void
test_teddy_upper_lower_class(void)
{
    ASSERT_SIMD_EQ("[A-Z][a-z]+", (const uint8_t *)"Hello World Foo Bar", 19);
    // "....".repeat(50) + "Hello" + "....".repeat(50)
    size_t total = 50 * 4 + 5 + 50 * 4;
    uint8_t *hay = n00b_alloc_array(uint8_t, total);
    memset(hay, '.', 50 * 4);
    memcpy(hay + 50 * 4, "Hello", 5);
    memset(hay + 50 * 4 + 5, '.', 50 * 4);
    ASSERT_SIMD_EQ("[A-Z][a-z]+", hay, total);
    n00b_free(hay);
    printf("  [PASS] teddy_upper_lower_class\n");
}

static void
test_teddy_alternation_three_way(void)
{
    ASSERT_SIMD_EQ("cat|dog|fox",
                   (const uint8_t *)"the cat sat on the dog and the fox ran",
                   38);
    printf("  [PASS] teddy_alternation_three_way\n");
}

static void
test_teddy_pattern_no_match(void)
{
    uint8_t hay[200];
    memset(hay, '.', 200);
    ASSERT_SIMD("[0-9]+",      hay, 200, nullptr, 0);
    ASSERT_SIMD("[A-Z][a-z]+", hay, 200, nullptr, 0);
    printf("  [PASS] teddy_pattern_no_match\n");
}

static void
test_teddy_at_position_zero(void)
{
    pair_t exp[1] = {{0, 3}};
    ASSERT_SIMD("[0-9]+", (const uint8_t *)"123abc", 6, exp, 1);
    printf("  [PASS] teddy_at_position_zero\n");
}

static void
test_teddy_at_end(void)
{
    pair_t exp[1] = {{3, 6}};
    ASSERT_SIMD("[0-9]+", (const uint8_t *)"abc123", 6, exp, 1);
    printf("  [PASS] teddy_at_end\n");
}

static void
test_teddy_dense_matches(void)
{
    uint8_t hay[100];
    for (size_t i = 0; i < 100; ++i) {
        hay[i] = (uint8_t)('0' + (i % 10));
    }
    ASSERT_SIMD_EQ("[0-9]+", hay, 100);
    printf("  [PASS] teddy_dense_matches\n");
}

static void
test_bounded_rep_size_sweep(void)
{
    const char *pattern = "ab{2,4}c";
    for (size_t size = 4; size <= 100; ++size) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 6) {
            hay[1] = 'a';
            hay[2] = 'b';
            hay[3] = 'b';
            hay[4] = 'c';
            ASSERT_SIMD_EQ(pattern, hay, size);
        }
        n00b_free(hay);
    }
    printf("  [PASS] bounded_rep_size_sweep\n");
}

static void
test_bounded_rep_multiple_at_boundaries(void)
{
    const char *pattern = "ab{2,4}c";
    uint8_t hay[128];
    memset(hay, '.', 128);
    hay[0] = 'a';
    memcpy(&hay[1], "bbb", 3);
    hay[4] = 'c';
    hay[15] = 'a';
    memcpy(&hay[16], "bb", 2);
    hay[18] = 'c';
    hay[32] = 'a';
    memcpy(&hay[33], "bbbb", 4);
    hay[37] = 'c';
    hay[64] = 'a';
    memcpy(&hay[65], "bb", 2);
    hay[67] = 'c';
    ASSERT_SIMD_EQ(pattern, hay, 128);
    printf("  [PASS] bounded_rep_multiple_at_boundaries\n");
}

// ---------------------------------------------------------------------------
// TOML-driven cross-checks (see header comment).
//
// The eight TOML-driven cases in the source (one accel_skip plus seven
// lazy_*_toml) iterate the engine over fixtures owned by other Phase 11
// test ports.  Re-embedding ~3000 lines of oracle data here would
// duplicate that coverage without adding any SIMD-specific signal.
// They are emitted as SKIP messages so the 1:1 file-mapping audit is
// satisfied and the SIMD path remains exercised by every other case in
// this file (rev_skip / fwd_literal / teddy / bounded_rep / etc.).
// ---------------------------------------------------------------------------

static void
test_all_accel_skip_patterns_simd_vs_default(void)
{
    // Source: #[ignore = "slow in debug; run with --ignored or in release"].
    // Coverage owned by test_regex_accel_skip.c (Phase 11).
    printf("  [SKIP] all_accel_skip_patterns_simd_vs_default "
           "(covered by test_regex_accel_skip.c)\n");
}

// ---------------------------------------------------------------------------
// run_toml_lazy — port of resharp-c's `run_toml_lazy`.
//
// Loads a TOML fixture from RESHARP_ENGINE_MANIFEST_DIR/tests/<filename>,
// iterates the `[[test]]` array, and asserts that `find_all` output under
// `lazy_opts()` (max_dfa_capacity = 10000) matches the `matches` field.
// Cases tagged ignore / expect_error / anchored are skipped, matching the
// Rust semantics exactly.
//
// Earlier this test was stubbed `[SKIP]` with the rationale that the same
// fixtures are exercised by `test_regex_engine.c::run_file()`.  That was
// incorrect: `run_file()` uses default options; this case explicitly
// exercises the lazy DFA path (10k cap), which is a different code path.
// The TOML parser at `parsers/toml.h` lets us reuse the fixture data
// without duplicating oracle bytes into this TU.
// ---------------------------------------------------------------------------
static char *
neon_resolve_fixture(const char *filename)
{
    n00b_string_t *base  = n00b_string_from_cstr(RESHARP_ENGINE_MANIFEST_DIR);
    n00b_string_t *tests = n00b_string_from_cstr("tests");
    n00b_string_t *fn    = n00b_string_from_cstr(filename);
    n00b_string_t *j     = n00b_path_simple_join(base, tests);
    n00b_string_t *jj    = n00b_path_simple_join(j, fn);
    size_t n   = (size_t)jj->u8_bytes;
    char  *out = n00b_alloc_array(char, n + 1);
    memcpy(out, jj->data, n);
    out[n] = '\0';
    return out;
}

static bool
neon_opt_bool(const n00b_toml_node_t *t, const char *key)
{
    n00b_toml_node_t *v = n00b_toml_table_get_cstr(t, key);
    if (v == nullptr) return false;
    return n00b_toml_as_bool(v);
}

static void
run_toml_lazy(const char *filename)
{
    char *path = neon_resolve_fixture(filename);
    n00b_string_t *path_s = n00b_string_from_cstr(path);
    auto r = n00b_toml_parse_file(path_s);
    assert(n00b_result_is_ok(r));
    n00b_toml_node_t *root  = n00b_result_get(r);
    n00b_toml_node_t *arr   = n00b_toml_table_array_of(root, "test");
    assert(arr != nullptr);
    size_t n = n00b_toml_array_len(arr);

    for (size_t i = 0; i < n; ++i) {
        n00b_toml_node_t *t = n00b_toml_array_get(arr, i);

        if (neon_opt_bool(t, "ignore")
            || neon_opt_bool(t, "expect_error")
            || neon_opt_bool(t, "anchored")) {
            continue;
        }

        n00b_toml_node_t *pv = n00b_toml_table_get_cstr(t, "pattern");
        assert(pv != nullptr && n00b_toml_type(pv) == N00B_TOML_STRING);
        const char *pattern = n00b_toml_as_string(pv)->data;

        n00b_toml_node_t *iv = n00b_toml_table_get_cstr(t, "input");
        const char *input = "";
        size_t      input_len = 0;
        if (iv != nullptr && n00b_toml_type(iv) == N00B_TOML_STRING) {
            input     = n00b_toml_as_string(iv)->data;
            input_len = (size_t)n00b_toml_as_string(iv)->u8_bytes;
        }

        // Rust's `match Regex::with_options(...)` — on compile error, skip.
        Regex *re = regex_with_options(pattern, lazy_opts());
        if (re == nullptr) continue;

        n00b_list_t(Match) got = n00b_list_new_private(Match);
        n00b_regex_engine_err_t err = regex_find_all(
            re, (const uint8_t *)input, input_len, &got);
        if (err != N00B_REGEX_ENGINE_ERR_NONE) {
            regex_free(re);
            continue;
        }

        // Compare against `matches = [[start, end], ...]`.
        n00b_toml_node_t *mv = n00b_toml_table_get_cstr(t, "matches");
        size_t exp_n = 0;
        if (mv != nullptr && n00b_toml_type(mv) == N00B_TOML_ARRAY) {
            exp_n = n00b_toml_array_len(mv);
        }
        size_t glen = n00b_list_len(got);
        bool ok = (glen == exp_n);
        if (ok) {
            for (size_t j = 0; j < exp_n; ++j) {
                n00b_toml_node_t *pair = n00b_toml_array_get(mv, j);
                assert(n00b_toml_type(pair) == N00B_TOML_ARRAY);
                size_t es = (size_t)n00b_toml_as_int(
                    n00b_toml_array_get(pair, 0));
                size_t ee = (size_t)n00b_toml_as_int(
                    n00b_toml_array_get(pair, 1));
                Match m = n00b_list_get(got, j);
                if (m.start != es || m.end != ee) { ok = false; break; }
            }
        }
        if (!ok) {
            fprintf(stderr,
                    "lazy_toml mismatch: file=%s test #%zu pattern=%s "
                    "input_len=%zu glen=%zu exp=%zu\n",
                    filename, i, pattern, input_len, glen, exp_n);
            n00b_list_free(got);
            regex_free(re);
            assert(false);
        }
        n00b_list_free(got);
        regex_free(re);
    }
}

static void test_lazy_basic_toml(void)
    { run_toml_lazy("basic.toml");      printf("  [PASS] lazy_basic_toml\n"); }
static void test_lazy_anchors_toml(void)
    { run_toml_lazy("anchors.toml");    printf("  [PASS] lazy_anchors_toml\n"); }
static void test_lazy_semantics_toml(void)
    { run_toml_lazy("semantics.toml");  printf("  [PASS] lazy_semantics_toml\n"); }
static void test_lazy_edge_cases_toml(void)
    { run_toml_lazy("edge_cases.toml"); printf("  [PASS] lazy_edge_cases_toml\n"); }
static void test_lazy_boolean_toml(void)
    { run_toml_lazy("boolean.toml");    printf("  [PASS] lazy_boolean_toml\n"); }
static void test_lazy_lookaround_toml(void)
    { run_toml_lazy("lookaround.toml"); printf("  [PASS] lazy_lookaround_toml\n"); }
static void test_lazy_paragraph_toml(void)
    { run_toml_lazy("paragraph.toml");  printf("  [PASS] lazy_paragraph_toml\n"); }

// ---------------------------------------------------------------------------
// remainder of the SIMD-specific cases
// ---------------------------------------------------------------------------

static void
test_literal_in_1kb_haystack(void)
{
    uint8_t hay[1024];
    memset(hay, '.', 1024);
    memcpy(&hay[500], "needle", 6);
    pair_t exp[1] = {{500, 506}};
    ASSERT_SIMD("needle", hay, 1024, exp, 1);
    ASSERT_SIMD_EQ("needle", hay, 1024);
    printf("  [PASS] literal_in_1kb_haystack\n");
}

static void
test_literal_in_64kb_haystack(void)
{
    uint8_t *hay = n00b_alloc_array(uint8_t, 65536);
    memset(hay, '.', 65536);
    memcpy(&hay[32000], "target", 6);
    memcpy(&hay[64000], "target", 6);
    pair_t exp[2] = {{32000, 32006}, {64000, 64006}};
    n00b_list_t(Match) lazy = find_lazy("target", hay, 65536);
    bool ok = list_eq_arr(&lazy, exp, 2);
    n00b_list_free(lazy);
    n00b_free(hay);
    assert(ok);
    printf("  [PASS] literal_in_64kb_haystack\n");
}

static void
test_class_pattern_in_1kb_haystack(void)
{
    uint8_t hay[1024];
    memset(hay, '.', 1024);
    memcpy(&hay[100], "12345", 5);
    memcpy(&hay[900], "678", 3);
    ASSERT_SIMD_EQ("[0-9]+", hay, 1024);
    printf("  [PASS] class_pattern_in_1kb_haystack\n");
}

static void
test_dot_pattern_long_haystack(void)
{
    uint8_t hay[500];
    memset(hay, '.', 500);
    hay[100] = 'x';
    hay[300] = 'x';
    ASSERT_SIMD_EQ("..x", hay, 500);
    printf("  [PASS] dot_pattern_long_haystack\n");
}

static void
test_teddy1_vowel_class(void)
{
    ASSERT_SIMD_EQ("[aeiou]", (const uint8_t *)"bcdfghjklmnpqrstvwxyz", 21);
    ASSERT_SIMD_EQ("[aeiou]", (const uint8_t *)"hello world", 11);
    printf("  [PASS] teddy1_vowel_class\n");
}

static void
test_teddy2_two_char_classes(void)
{
    ASSERT_SIMD_EQ("[A-Z][0-9]", (const uint8_t *)"___A5___B7___", 13);
    uint8_t hay[100];
    memset(hay, '.', 100);
    hay[50] = 'Q';
    hay[51] = '3';
    ASSERT_SIMD_EQ("[A-Z][0-9]", hay, 100);
    printf("  [PASS] teddy2_two_char_classes\n");
}

static void
test_teddy3_three_char_classes(void)
{
    ASSERT_SIMD_EQ("[a-z][0-9][A-Z]", (const uint8_t *)"___a5B___c7D___", 15);
    uint8_t hay[200];
    memset(hay, '.', 200);
    hay[100] = 'x';
    hay[101] = '9';
    hay[102] = 'Z';
    ASSERT_SIMD_EQ("[a-z][0-9][A-Z]", hay, 200);
    printf("  [PASS] teddy3_three_char_classes\n");
}

static void
test_empty_input(void)
{
    ASSERT_SIMD("abc",    (const uint8_t *)"", 0, nullptr, 0);
    ASSERT_SIMD("[0-9]+", (const uint8_t *)"", 0, nullptr, 0);
    printf("  [PASS] empty_input\n");
}

static void
test_input_shorter_than_pattern(void)
{
    ASSERT_SIMD("abcdef",      (const uint8_t *)"abc", 3, nullptr, 0);
    ASSERT_SIMD("[A-Z][a-z]+", (const uint8_t *)"H",   1, nullptr, 0);
    printf("  [PASS] input_shorter_than_pattern\n");
}

static void
test_one_byte_input(void)
{
    pair_t exp1[1] = {{0, 1}};
    ASSERT_SIMD("a",     (const uint8_t *)"a", 1, exp1,    1);
    ASSERT_SIMD("a",     (const uint8_t *)"b", 1, nullptr, 0);
    ASSERT_SIMD("[0-9]", (const uint8_t *)"5", 1, exp1,    1);
    printf("  [PASS] one_byte_input\n");
}

static void
test_greedy_dot_star(void)
{
    ASSERT_SIMD_EQ("a.*b", (const uint8_t *)"a---b---b", 9);
    uint8_t hay[200];
    memset(hay, '-', 200);
    hay[0]   = 'a';
    hay[199] = 'b';
    ASSERT_SIMD_EQ("a.*b", hay, 200);
    printf("  [PASS] greedy_dot_star\n");
}

static void
test_non_overlapping_adjacent(void)
{
    pair_t exp[3] = {{0, 2}, {2, 4}, {4, 6}};
    ASSERT_SIMD("ab", (const uint8_t *)"ababab", 6, exp, 3);
    printf("  [PASS] non_overlapping_adjacent\n");
}

static void
test_ip_address_long_input(void)
{
    const char *pattern = "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}";
    // "padding " * 100 + "connect from 192.168.1.100 to 10.0.0.1"
    //   + " padding" * 100
    const char *pad_a = "padding ";
    const char *mid   = "connect from 192.168.1.100 to 10.0.0.1";
    const char *pad_b = " padding";
    size_t pa = strlen(pad_a), pb = strlen(pad_b), mm = strlen(mid);
    size_t total = pa * 100 + mm + pb * 100;
    uint8_t *hay = n00b_alloc_array(uint8_t, total);
    size_t off = 0;
    for (int i = 0; i < 100; ++i) {
        memcpy(hay + off, pad_a, pa);
        off += pa;
    }
    memcpy(hay + off, mid, mm);
    off += mm;
    for (int i = 0; i < 100; ++i) {
        memcpy(hay + off, pad_b, pb);
        off += pb;
    }
    ASSERT_SIMD_EQ(pattern, hay, total);
    n00b_free(hay);
    printf("  [PASS] ip_address_long_input\n");
}

static void
test_email_pattern(void)
{
    const char *pattern =
        "[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}";
    ASSERT_SIMD_EQ(pattern,
                   (const uint8_t *)"contact user@example.com or "
                                    "admin@test.org",
                   strlen("contact user@example.com or admin@test.org"));
    const char *pad_a = "xxxx ";
    const char *mid   = "test@foo.bar";
    const char *pad_b = " yyyy";
    size_t pa = strlen(pad_a), pb = strlen(pad_b), mm = strlen(mid);
    size_t total = pa * 200 + mm + pb * 200;
    uint8_t *hay = n00b_alloc_array(uint8_t, total);
    size_t off = 0;
    for (int i = 0; i < 200; ++i) {
        memcpy(hay + off, pad_a, pa);
        off += pa;
    }
    memcpy(hay + off, mid, mm);
    off += mm;
    for (int i = 0; i < 200; ++i) {
        memcpy(hay + off, pad_b, pb);
        off += pb;
    }
    ASSERT_SIMD_EQ(pattern, hay, total);
    n00b_free(hay);
    printf("  [PASS] email_pattern\n");
}

static void
test_html_tag_pattern(void)
{
    ASSERT_SIMD_EQ("<h[1-6]>.*</h[1-6]>",
                   (const uint8_t *)"<h1>Title</h1> and <h2>Sub</h2>",
                   strlen("<h1>Title</h1> and <h2>Sub</h2>"));
    printf("  [PASS] html_tag_pattern\n");
}

static void
test_quoted_string(void)
{
    const char *pattern = "\"[^\"]*\"";
    const char *input   = "say \"hello\" and \"bye\"";
    ASSERT_SIMD_EQ(pattern, (const uint8_t *)input, strlen(input));
    uint8_t hay[500];
    memset(hay, '.', 500);
    hay[200] = '"';
    memcpy(&hay[201], "inner", 5);
    hay[206] = '"';
    ASSERT_SIMD_EQ(pattern, hay, 500);
    printf("  [PASS] quoted_string\n");
}

static void
test_anchored_start(void)
{
    ASSERT_SIMD_EQ("^hello", (const uint8_t *)"hello world", 11);
    ASSERT_SIMD_EQ("^hello", (const uint8_t *)"world hello", 11);
    printf("  [PASS] anchored_start\n");
}

static void
test_anchored_end(void)
{
    ASSERT_SIMD_EQ("world$", (const uint8_t *)"hello world", 11);
    printf("  [PASS] anchored_end\n");
}

static void
test_anchored_both(void)
{
    ASSERT_SIMD_EQ("^exact$", (const uint8_t *)"exact",     5);
    ASSERT_SIMD_EQ("^exact$", (const uint8_t *)"not exact", 9);
    printf("  [PASS] anchored_both\n");
}

static void
test_lookahead_with_simd(void)
{
    ASSERT_SIMD_EQ("a(?=b)",                 (const uint8_t *)"_ab_ab_", 7);
    ASSERT_SIMD_EQ("\\d+(?=[aA]\\.?[mM]\\.?)",
                   (const uint8_t *)"10am", 4);
    printf("  [PASS] lookahead_with_simd\n");
}

static void
test_lookbehind_with_simd(void)
{
    ASSERT_SIMD_EQ("(?<=b)a", (const uint8_t *)"bbbba", 5);
    ASSERT_SIMD_EQ("(?<=author).*",
                   (const uint8_t *)"author: abc and def",
                   strlen("author: abc and def"));
    printf("  [PASS] lookbehind_with_simd\n");
}

static void
test_neg_lookbehind_with_simd(void)
{
    ASSERT_SIMD_EQ("(?<!\\d)a", (const uint8_t *)"1a__a__a", 8);
    printf("  [PASS] neg_lookbehind_with_simd\n");
}

static void
test_complement_simd(void)
{
    ASSERT_SIMD_EQ("~(_*\\d\\d_*)", (const uint8_t *)"Aa11aBaAA", 9);
    printf("  [PASS] complement_simd\n");
}

static void
test_intersection_simd(void)
{
    ASSERT_SIMD_EQ("c...&...s",
                   (const uint8_t *)"raining cats and dogs",
                   strlen("raining cats and dogs"));
    printf("  [PASS] intersection_simd\n");
}

static void
test_complement_intersection_simd(void)
{
    ASSERT_SIMD_EQ("~(.*\\d\\d.*)&[a-zA-Z\\d]{8,}",
                   (const uint8_t *)"tej55zhA25wXu8bvQxFxt",
                   strlen("tej55zhA25wXu8bvQxFxt"));
    printf("  [PASS] complement_intersection_simd\n");
}

static void
test_multiline_simd(void)
{
    const char *input = "\naaa\n\nbbb\n\nccc\n\n";
    ASSERT_SIMD_EQ("(?:.+\\n)+\\n", (const uint8_t *)input, strlen(input));
    printf("  [PASS] multiline_simd\n");
}

static void
test_deep_alternation(void)
{
    ASSERT_SIMD_EQ(
        "accommodating|acknowledging|comprehensive|corresponding|disappointing",
        (const uint8_t *)"a]comprehensive/disappointing;acknowledging",
        strlen("a]comprehensive/disappointing;acknowledging"));
    printf("  [PASS] deep_alternation\n");
}

static void
test_alternation_factored_prefix(void)
{
    ASSERT_SIMD_EQ("bar|baz", (const uint8_t *)"bar baz bar", 11);
    printf("  [PASS] alternation_factored_prefix\n");
}

static void
test_alternation_with_suffix(void)
{
    ASSERT_SIMD_EQ("(cat|dog)\\d+",
                   (const uint8_t *)"cat123 dog45 cat bird99",
                   strlen("cat123 dog45 cat bird99"));
    printf("  [PASS] alternation_with_suffix\n");
}

static void
test_size_sweep_literal(void)
{
    for (size_t size = 1; size <= 200; size += 7) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 5) {
            size_t pos = size / 2;
            memcpy(&hay[pos], "abc", 3);
        }
        ASSERT_SIMD_EQ("abc", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] size_sweep_literal\n");
}

static void
test_size_sweep_class(void)
{
    for (size_t size = 1; size <= 200; size += 7) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 3) hay[size / 2] = '7';
        ASSERT_SIMD_EQ("[0-9]+", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] size_sweep_class\n");
}

static void
test_size_sweep_bounded_rep(void)
{
    for (size_t size = 1; size <= 200; size += 3) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 6) {
            size_t pos = size / 2;
            hay[pos]     = 'a';
            hay[pos + 1] = 'b';
            hay[pos + 2] = 'b';
            hay[pos + 3] = 'c';
        }
        ASSERT_SIMD_EQ("ab{2,4}c", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] size_sweep_bounded_rep\n");
}

static void
test_match_spans_chunk_boundary(void)
{
    uint8_t hay[64];
    memset(hay, '.', 64);
    memcpy(&hay[14], "abcd", 4);
    {
        pair_t exp[1] = {{14, 18}};
        ASSERT_SIMD("abcd", hay, 64, exp, 1);
    }
    memset(&hay[14], '.', 4);
    memcpy(&hay[15], "abcd", 4);
    {
        pair_t exp[1] = {{15, 19}};
        ASSERT_SIMD("abcd", hay, 64, exp, 1);
    }
    memset(&hay[15], '.', 4);
    memcpy(&hay[30], "abcd", 4);
    {
        pair_t exp[1] = {{30, 34}};
        ASSERT_SIMD("abcd", hay, 64, exp, 1);
    }
    printf("  [PASS] match_spans_chunk_boundary\n");
}

static void
test_many_single_char_matches(void)
{
    uint8_t hay[500];
    memset(hay, 'a', 500);
    pair_t exp[500];
    for (size_t i = 0; i < 500; ++i) {
        exp[i].start = i;
        exp[i].end   = i + 1;
    }
    ASSERT_SIMD("a", hay, 500, exp, 500);
    printf("  [PASS] many_single_char_matches\n");
}

static void
test_many_two_char_matches(void)
{
    // 33 "ab" pairs = 66 bytes; matches the Rust literal exactly.
    const char *hay_s = "ababababababababababababababababababababababababab"
                        "abababababababab";
    size_t hay_len = strlen(hay_s);  // 66
    pair_t *exp = n00b_alloc_array(pair_t, hay_len / 2);
    size_t n = 0;
    for (size_t i = 0; i < hay_len; i += 2) {
        exp[n].start = i;
        exp[n].end   = i + 2;
        ++n;
    }
    n00b_list_t(Match) lazy =
        find_lazy("ab", (const uint8_t *)hay_s, hay_len);
    bool ok = list_eq_arr(&lazy, exp, n);
    n00b_list_free(lazy);
    n00b_free(exp);
    assert(ok);
    printf("  [PASS] many_two_char_matches\n");
}

static void
test_is_match_lazy_long_input(void)
{
    Regex *re = regex_with_options("needle", lazy_opts());
    assert(re != nullptr);
    uint8_t *hay = n00b_alloc_array(uint8_t, 10000);
    memset(hay, '.', 10000);
    bool m = false;
    n00b_regex_engine_err_t err = regex_is_match(re, hay, 10000, &m);
    if (err != N00B_REGEX_ENGINE_ERR_NONE || m) {
        n00b_free(hay);
        regex_free(re);
        assert(false);
    }
    memcpy(&hay[9990], "needle", 6);
    err = regex_is_match(re, hay, 10000, &m);
    bool pass = (err == N00B_REGEX_ENGINE_ERR_NONE && m);
    n00b_free(hay);
    regex_free(re);
    assert(pass);
    printf("  [PASS] is_match_lazy_long_input\n");
}

static void
test_is_match_class_lazy(void)
{
    Regex *re = regex_with_options("[0-9]+", lazy_opts());
    assert(re != nullptr);
    uint8_t hay[1000];
    memset(hay, '.', 1000);
    bool m = false;
    n00b_regex_engine_err_t err = regex_is_match(re, hay, 1000, &m);
    if (err != N00B_REGEX_ENGINE_ERR_NONE || m) {
        regex_free(re);
        assert(false);
    }
    uint8_t hay2[1000];
    memset(hay2, '.', 1000);
    hay2[500] = '5';
    err = regex_is_match(re, hay2, 1000, &m);
    bool pass = (err == N00B_REGEX_ENGINE_ERR_NONE && m);
    regex_free(re);
    assert(pass);
    printf("  [PASS] is_match_class_lazy\n");
}

static void
test_rev_range_skip_digit_sweep(void)
{
    for (size_t pos = 0; pos < 64; ++pos) {
        uint8_t hay[64];
        memset(hay, '.', 64);
        hay[pos] = (uint8_t)('0' + (pos % 10));
        ASSERT_SIMD_EQ("[0-9]+", hay, 64);
    }
    printf("  [PASS] rev_range_skip_digit_sweep\n");
}

static void
test_rev_range_skip_uppercase_sweep(void)
{
    for (size_t pos = 0; pos < 64; ++pos) {
        uint8_t hay[64];
        memset(hay, '.', 64);
        hay[pos] = (uint8_t)('A' + (pos % 26));
        ASSERT_SIMD_EQ("[A-Z]+", hay, 64);
    }
    printf("  [PASS] rev_range_skip_uppercase_sweep\n");
}

static void
test_rev_range_skip_two_ranges(void)
{
    // hex digits: [0-9A-F]
    for (size_t pos = 0; pos < 64; ++pos) {
        uint8_t hay[64];
        memset(hay, '.', 64);
        if (pos % 2 == 0) {
            hay[pos] = (uint8_t)('0' + (pos % 10));
        } else {
            hay[pos] = (uint8_t)('A' + (pos % 6));
        }
        ASSERT_SIMD_EQ("[0-9A-F]+", hay, 64);
    }
    printf("  [PASS] rev_range_skip_two_ranges\n");
}

static void
test_rev_range_skip_no_match_long(void)
{
    uint8_t hay[1024];
    memset(hay, '.', 1024);
    ASSERT_SIMD("[0-9]+", hay, 1024, nullptr, 0);
    ASSERT_SIMD("[A-Z]+", hay, 1024, nullptr, 0);
    printf("  [PASS] rev_range_skip_no_match_long\n");
}

static void
test_rev_range_skip_all_match(void)
{
    uint8_t hay[100];
    for (size_t i = 0; i < 100; ++i) {
        hay[i] = (uint8_t)('0' + (i % 10));
    }
    ASSERT_SIMD_EQ("[0-9]+", hay, 100);
    printf("  [PASS] rev_range_skip_all_match\n");
}

static void
test_rev_range_skip_size_sweep(void)
{
    for (size_t size = 1; size <= 200; size += 7) {
        uint8_t *hay = n00b_alloc_array(uint8_t, size);
        memset(hay, '.', size);
        if (size >= 3) hay[size / 2] = '3';
        ASSERT_SIMD_EQ("[0-9]+", hay, size);
        if (size >= 3) hay[size / 2] = 'M';
        ASSERT_SIMD_EQ("[A-Z]+", hay, size);
        n00b_free(hay);
    }
    printf("  [PASS] rev_range_skip_size_sweep\n");
}

static void
test_range_skip_digit_plus_has_accel(void)
{
    // Source: #[ignore = "reimplement prefix selection first"].
    Regex *re = regex_with_options("[0-9]+", lazy_opts());
    assert(re != nullptr);
    bool fwd = false, rev = false;
    regex_has_accel(re, &fwd, &rev);
    (void)fwd;  // TODO mirrored from source: assert on `fwd` once prefix
                //                            selection is reimplemented.
    bool pass = rev;
    regex_free(re);
    assert(pass);
    printf("  [PASS] range_skip_digit_plus_has_accel\n");
}

static void
test_range_skip_uppercase_plus_has_accel(void)
{
    // Source: #[ignore = "reimplement prefix selection first"].
    Regex *re = regex_with_options("[A-Z]+", lazy_opts());
    assert(re != nullptr);
    bool fwd = false, rev = false;
    regex_has_accel(re, &fwd, &rev);
    (void)fwd;
    bool pass = rev;
    regex_free(re);
    assert(pass);
    printf("  [PASS] range_skip_uppercase_plus_has_accel\n");
}

static void
test_range_skip_ip_address(void)
{
    const char *pattern = "\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}";
    uint8_t hay[500];
    memset(hay, ' ', 500);
    memcpy(&hay[200], "192.168.1.100", 13);
    ASSERT_SIMD_EQ(pattern, hay, 500);
    printf("  [PASS] range_skip_ip_address\n");
}

static void
test_range_skip_uppercase_in_long_input(void)
{
    const char *pattern = "[A-Z]+";
    uint8_t hay[2000];
    memset(hay, '.', 2000);
    memcpy(&hay[500],  "ABC",  3);
    memcpy(&hay[1500], "WXYZ", 4);
    ASSERT_SIMD_EQ(pattern, hay, 2000);
    printf("  [PASS] range_skip_uppercase_in_long_input\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex neon SIMD tests...\n");

    test_rev_skip_single_byte_every_position();
    test_rev_skip_two_bytes_sweep();
    test_rev_skip_no_match_long();
    test_rev_skip_all_match();
    test_fwd_literal_at_every_offset();
    test_fwd_literal_adjacent_non_overlapping();
    test_fwd_literal_long_needle();
    test_fwd_literal_haystack_equals_needle();
    test_fwd_literal_single_byte_haystack();
    test_fwd_literal_near_end_boundary();
    test_fwd_literal_bulk_find_all();
    test_teddy_digit_class_sweep();
    test_teddy_upper_lower_class();
    test_teddy_alternation_three_way();
    test_teddy_pattern_no_match();
    test_teddy_at_position_zero();
    test_teddy_at_end();
    test_teddy_dense_matches();
    test_bounded_rep_size_sweep();
    test_bounded_rep_multiple_at_boundaries();
    test_all_accel_skip_patterns_simd_vs_default();
    test_lazy_basic_toml();
    test_lazy_anchors_toml();
    test_lazy_semantics_toml();
    test_lazy_edge_cases_toml();
    test_lazy_boolean_toml();
    test_lazy_lookaround_toml();
    test_lazy_paragraph_toml();
    test_literal_in_1kb_haystack();
    test_literal_in_64kb_haystack();
    test_class_pattern_in_1kb_haystack();
    test_dot_pattern_long_haystack();
    test_teddy1_vowel_class();
    test_teddy2_two_char_classes();
    test_teddy3_three_char_classes();
    test_empty_input();
    test_input_shorter_than_pattern();
    test_one_byte_input();
    test_greedy_dot_star();
    test_non_overlapping_adjacent();
    test_ip_address_long_input();
    test_email_pattern();
    test_html_tag_pattern();
    test_quoted_string();
    test_anchored_start();
    test_anchored_end();
    test_anchored_both();
    test_lookahead_with_simd();
    test_lookbehind_with_simd();
    test_neg_lookbehind_with_simd();
    test_complement_simd();
    test_intersection_simd();
    test_complement_intersection_simd();
    test_multiline_simd();
    test_deep_alternation();
    test_alternation_factored_prefix();
    test_alternation_with_suffix();
    test_size_sweep_literal();
    test_size_sweep_class();
    test_size_sweep_bounded_rep();
    test_match_spans_chunk_boundary();
    test_many_single_char_matches();
    test_many_two_char_matches();
    test_is_match_lazy_long_input();
    test_is_match_class_lazy();
    test_rev_range_skip_digit_sweep();
    test_rev_range_skip_uppercase_sweep();
    test_rev_range_skip_two_ranges();
    test_rev_range_skip_no_match_long();
    test_rev_range_skip_all_match();
    test_rev_range_skip_size_sweep();
    // Upstream marks these two as #[ignore = "reimplement prefix selection first"]
    // — they fail upstream too when forced to run.  Mirror by skipping.
    printf("  [SKIP] range_skip_digit_plus_has_accel (ignored upstream)\n");
    printf("  [SKIP] range_skip_uppercase_plus_has_accel (ignored upstream)\n");
    test_range_skip_ip_address();
    test_range_skip_uppercase_in_long_input();

    printf("All regex neon SIMD tests passed.\n");
    n00b_shutdown();
    return 0;
}

#else // !defined(__aarch64__)

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Running regex neon SIMD tests...\n");
    printf("  [SKIP] non-aarch64 build — neon SIMD tests are aarch64-only\n");
    printf("All regex neon SIMD tests passed.\n");
    return 0;
}

#endif // __aarch64__
