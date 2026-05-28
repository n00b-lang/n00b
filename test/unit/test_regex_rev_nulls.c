// Phase 11 / rev_nulls_test.c — typed translation.
//
// Source: ~/resharp-c/tests/rev_nulls_test.c.  The original loaded a
// TOML fixture of (pattern, input, rev_nulls) cases, compiled each
// pattern via the engine FFI, and verified that
// `regex_collect_rev_nulls_debug` returned the expected list of
// reverse-null positions (sorted descending).  n00b-regex has no TOML
// reader; the cases below are embedded byte-identical from
// `test/data/regex/rev_nulls.toml`.
//
// The expected oracle vectors are byte-identical with the TOML file —
// data values are correctness oracles per § 7.5 / Phase 11 rules and
// must not be paraphrased.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"

#include "internal/regex/regex.h"

// ---- Embedded fixture (byte-identical with rev_nulls.toml) ----------------

typedef struct test_case_t {
    const char    *name;
    const char    *pattern;
    const uint8_t *input;
    size_t         input_len;
    const size_t  *rev_nulls;
    size_t         rev_nulls_len;
} test_case_t;

// Per-case rev_nulls oracle arrays.

static const size_t rn_digits[]                          = {0};
static const size_t rn_sorted_space_class_space[]        = {12, 6, 0};
static const size_t rn_sorted_class_no_anchors[]         = {13, 7, 1};
static const size_t rn_readme_lookahead_lookbehind[]     = {13, 7, 1};
static const size_t rn_lookahead_simple[]                = {4, 1};
static const size_t rn_dotstar_lookahead_aaa_short[]     = {1, 0};
static const size_t rn_dotstar_lookahead_aaa_long[]      = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0};
static const size_t rn_dotstar_lookahead_chained_short[] = {4, 3, 2, 1, 0, 0};
static const size_t rn_dotstar_lookahead_chained_long[]  = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0};
static const size_t rn_lookahead_word_boundary[]         = {2, 1, 0};
static const size_t rn_lookbehind_lookahead_combined[]   = {2, 1};
static const size_t rn_lookahead_class_repetition[]      = {5, 4, 2, 1, 0};
static const size_t rn_lookahead_time_pattern[]          = {1, 0};
static const size_t rn_dotstar_inner_literal_multiline[] = {12, 11, 10, 9, 8, 7, 6};

#define CASE(n_, p_, in_, rn_)                                                 \
    {.name      = n_,                                                          \
     .pattern   = p_,                                                          \
     .input     = (const uint8_t *)(in_),                                      \
     .input_len = sizeof(in_) - 1,                                             \
     .rev_nulls = (rn_),                                                       \
     .rev_nulls_len = sizeof(rn_) / sizeof((rn_)[0])}

static const test_case_t k_cases[] = {
    CASE("digits",                          "1(?=[012])\\d",                  "11",                       rn_digits),
    CASE("sorted_space_class_space",        " [A-Z][a-z]+ ",                  " Hello World Foo ",        rn_sorted_space_class_space),
    CASE("sorted_class_no_anchors",         "[A-Z][a-z]+",                    " Hello World Foo ",        rn_sorted_class_no_anchors),
    CASE("readme_lookahead_lookbehind",     "(?<=\\s)[A-Z][a-z]+(?=\\s)",     " Hello World Foo ",        rn_readme_lookahead_lookbehind),
    CASE("lookahead_simple",                "a(?=b)",                         "_ab_ab_",                  rn_lookahead_simple),
    CASE("dotstar_lookahead_aaa_short",     ".*(?=aaa)",                      "baaa",                     rn_dotstar_lookahead_aaa_short),
    CASE("dotstar_lookahead_aaa_long",      ".*(?=aaa)",                      "bbbbbbbbbbaaa",            rn_dotstar_lookahead_aaa_long),
    CASE("dotstar_lookahead_chained_short", ".*(?=.*bbb)(?=.*ccc)",           "aaa bbb ccc",              rn_dotstar_lookahead_chained_short),
    CASE("dotstar_lookahead_chained_long",  ".*(?=.*bbb)(?=.*ccc)",           "aaaaaaaaaa bbb aaaaaaaaaa ccc", rn_dotstar_lookahead_chained_long),
    CASE("lookahead_word_boundary",         "a+\\b(?=.*---)",                 "aaa ---",                  rn_lookahead_word_boundary),
    CASE("lookbehind_lookahead_combined",   "(?<=a.*).(?=.*c)",               "a__c",                     rn_lookbehind_lookahead_combined),
    CASE("lookahead_class_repetition",      "[a-z]+(?=[A-Z])",                "abcDefGhi",                rn_lookahead_class_repetition),
    CASE("lookahead_time_pattern",          "\\d+(?=[aApP]\\.?[mM]\\.?)",     "10pm",                     rn_lookahead_time_pattern),
    CASE("dotstar_inner_literal_multiline", ".*=.*",                          "first\nsecond=line\nthird", rn_dotstar_inner_literal_multiline),
};

#undef CASE

// ---- The test -------------------------------------------------------------

static void
run_one_case(const test_case_t *tc)
{
    Regex *re = regex_new(tc->pattern);
    assert(re != nullptr);

    n00b_list_t(size_t) *got = regex_collect_rev_nulls_debug(
        re, tc->input, tc->input_len);
    assert(got != nullptr);

    size_t got_len = n00b_list_len(*got);
    // Sorted-descending invariant.
    for (size_t i = 1; i < got_len; ++i) {
        size_t prev = n00b_list_get(*got, i - 1);
        size_t cur  = n00b_list_get(*got, i);
        assert(cur <= prev);
    }

    // Byte-for-byte equality with the oracle.
    if (got_len != tc->rev_nulls_len) {
        fprintf(stderr,
                "rev_nulls length mismatch: case=%s pattern=%s expected=%zu got=%zu actual=[",
                tc->name,
                tc->pattern,
                tc->rev_nulls_len,
                got_len);
        for (size_t i = 0; i < got_len; ++i) {
            fprintf(stderr, "%s%zu", i == 0 ? "" : ", ",
                    n00b_list_get(*got, i));
        }
        fprintf(stderr, "]\n");
    }
    assert(got_len == tc->rev_nulls_len);
    for (size_t i = 0; i < got_len; ++i) {
        size_t v = n00b_list_get(*got, i);
        if (v != tc->rev_nulls[i]) {
            fprintf(stderr,
                    "rev_nulls value mismatch: case=%s pattern=%s index=%zu expected=%zu got=%zu\n",
                    tc->name,
                    tc->pattern,
                    i,
                    tc->rev_nulls[i],
                    v);
        }
        assert(v == tc->rev_nulls[i]);
    }

    n00b_list_free(*got);
    regex_free(re);
}

static void
test_rev_nulls_toml(void)
{
    constexpr size_t n_cases = sizeof(k_cases) / sizeof(k_cases[0]);
    for (size_t k = 0; k < n_cases; ++k) {
        run_one_case(&k_cases[k]);
    }
    printf("  [PASS] rev_nulls_toml\n");
}

// ---- Test runner entry point ----------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex rev_nulls tests...\n");
    test_rev_nulls_toml();
    printf("All regex rev_nulls tests passed.\n");

    n00b_shutdown();
    return 0;
}
