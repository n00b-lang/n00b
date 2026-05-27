#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "adt/list.h"
#include "adt/option.h"
#include "internal/regex/accel.h"
#include "internal/regex/engine.h"
#include "internal/regex/prefix.h"
#include "util/simd/mod.h"

struct n00b_simd_ByteRange;

extern n00b_simd_RevSearchBytes  *n00b_simd_RevSearchBytes_new(n00b_list_t(uint8_t) bytes);
extern n00b_simd_RevSearchRanges *n00b_simd_RevSearchRanges_new(n00b_list_t(U8Pair) ranges);
extern const uint8_t *n00b_simd_rev_search_bytes_bytes(const n00b_simd_RevSearchBytes *s,
                                                       size_t                         *out_len);
extern const struct n00b_simd_ByteRange *
n00b_simd_rev_search_ranges_ranges(const n00b_simd_RevSearchRanges *s, size_t *out_len);
extern n00b_simd_FwdLiteralSearch *n00b_simd_FwdLiteralSearch_new(const uint8_t *needle,
                                                                  size_t         nlen);
extern uint8_t n00b_simd_FwdLiteralSearch_rare_byte(const n00b_simd_FwdLiteralSearch *self);
extern void    n00b_simd_FwdLiteralSearch_free(n00b_simd_FwdLiteralSearch *p);
extern n00b_simd_FwdPrefixSearchSimd *n00b_simd_FwdPrefixSearch_new(size_t        total_len,
                                                                    const size_t *freq_order,
                                                                    size_t freq_order_len,
                                                                    const ByteVec *byte_sets,
                                                                    size_t         bs_len,
                                                                    const TSet    *all_sets,
                                                                    size_t         as_len);
extern n00b_simd_RevTeddySearch      *n00b_simd_RevTeddySearch_new(size_t         num_simd,
                                                                   const ByteVec *window,
                                                                   size_t         window_len,
                                                                   const TSet    *all_sets,
                                                                   size_t         as_len,
                                                                   size_t         tail_offset);
extern n00b_option_t(size_t)
    n00b_simd_rev_prefix_search_find_rev(const n00b_simd_RevTeddySearch *s,
                                         const uint8_t                  *haystack,
                                         size_t                          hlen,
                                         size_t                          end);
extern n00b_simd_FwdRangeSearch *n00b_simd_FwdRangeSearch_new(size_t         total_len,
                                                              size_t         anchor_pos,
                                                              const uint8_t *lo,
                                                              const uint8_t *hi,
                                                              size_t         ranges_len,
                                                              const TSet    *all_sets,
                                                              size_t         all_sets_len);

#if !defined(__aarch64__)
static void
test_backendless_simd_contract(void)
{
    uint8_t haystack[] = {'a', 'b', 'c'};
    size_t  len        = SIZE_MAX;

    assert(!n00b_simd_has_simd());

    n00b_list_t(uint8_t) bytes = {};
    assert(n00b_simd_RevSearchBytes_new(bytes) == nullptr);
    assert(n00b_simd_rev_search_bytes_bytes(nullptr, &len) == nullptr);
    assert(len == 0);
    assert(!n00b_option_is_set(
        n00b_simd_rev_search_bytes_find_fwd(nullptr, haystack, sizeof(haystack))));
    assert(!n00b_option_is_set(
        n00b_simd_rev_search_bytes_find_rev(nullptr, haystack, sizeof(haystack))));

    n00b_list_t(U8Pair) ranges = {};
    len                        = SIZE_MAX;
    assert(n00b_simd_RevSearchRanges_new(ranges) == nullptr);
    assert(n00b_simd_rev_search_ranges_ranges(nullptr, &len) == nullptr);
    assert(len == 0);
    assert(!n00b_option_is_set(
        n00b_simd_rev_search_ranges_find_fwd(nullptr, haystack, sizeof(haystack))));
    assert(!n00b_option_is_set(
        n00b_simd_rev_search_ranges_find_rev(nullptr, haystack, sizeof(haystack))));

    assert(n00b_simd_FwdLiteralSearch_new(haystack, sizeof(haystack)) == nullptr);
    assert(n00b_simd_FwdLiteralSearch_rare_byte(nullptr) == 0);
    assert(n00b_simd_fwd_literal_search_len(nullptr) == 0);
    assert(!n00b_option_is_set(
        n00b_simd_fwd_literal_search_find_fwd(nullptr, haystack, sizeof(haystack))));
    n00b_simd_fwd_literal_search_find_all_fixed(nullptr, haystack, sizeof(haystack), nullptr);
    n00b_simd_FwdLiteralSearch_free(nullptr);

    assert(n00b_simd_FwdPrefixSearch_new(0, nullptr, 0, nullptr, 0, nullptr, 0) == nullptr);
    assert(n00b_simd_fwd_prefix_search_simd_len(nullptr) == 0);
    assert(!n00b_option_is_set(
        n00b_simd_fwd_prefix_search_simd_find_fwd(nullptr, haystack, sizeof(haystack), 0)));

    assert(n00b_simd_RevTeddySearch_new(0, nullptr, 0, nullptr, 0, 0) == nullptr);
    assert(!n00b_option_is_set(
        n00b_simd_rev_prefix_search_find_rev(nullptr, haystack, sizeof(haystack), 0)));

    assert(n00b_simd_FwdRangeSearch_new(0, 0, nullptr, nullptr, 0, nullptr, 0) == nullptr);
    assert(n00b_simd_fwd_range_search_len(nullptr) == 0);
    assert(!n00b_option_is_set(
        n00b_simd_fwd_range_search_find_fwd(nullptr, haystack, sizeof(haystack), 0)));
}

static void
test_wrapper_rejects_null_payloads(void)
{
    assert(fwd_prefix_search_new_literal(nullptr, nullptr) == nullptr);
    assert(fwd_prefix_search_new_prefix(nullptr, nullptr) == nullptr);
    assert(fwd_prefix_search_new_range(nullptr, nullptr) == nullptr);
}
#endif

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running backendless SIMD contract tests...\n");

#if defined(__aarch64__)
    printf("  [SKIP] backendless SIMD contract is non-aarch64-only\n");
#else
    test_backendless_simd_contract();
    printf("  [PASS] backendless SIMD stubs\n");
    test_wrapper_rejects_null_payloads();
    printf("  [PASS] wrapper null payload rejection\n");
#endif

    printf("All backendless SIMD contract tests passed.\n");
    n00b_shutdown();
    return 0;
}
