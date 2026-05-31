// Phase 11 / stream_test.c — typed translation.
//
// Source: ~/resharp-c/tests/stream_test.c.  The original loaded a TOML
// fixture of (pattern, input, matches[, vs_find_all]) entries, ran
// `regex_stream` on each, asserted equality with the fixture's expected
// matches, and (when `vs_find_all = true`) cross-checked against
// `regex_find_all`.  Inline tests at the bottom port the upstream Rust
// `test_stream_prefix_skip_helps`, `test_stream_with_callback`,
// `test_cross_chunk_boundary`, `test_stream_chunk`, `seek_fwd_rev_cursor`,
// `seek_fwd_from_middle`, `seek_rev_from_middle`, `seek_no_match`,
// `seek_fwd_skips_match_before_pos`, `seek_fwd_with_class_pattern`.
//
// n00b-regex has no TOML reader; the cases below are embedded
// byte-identical from `test/data/regex/stream.toml`.

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"

#include "internal/regex/regex.h"
#include "internal/regex/stream.h"

// ---- Fixture (byte-identical with stream.toml) ----------------------------

typedef struct match_pair_t {
    size_t start;
    size_t end;
} match_pair_t;

typedef struct stream_case_t {
    const char         *name;
    const char         *pattern;
    const uint8_t      *input;
    size_t              input_len;
    const match_pair_t *matches;
    size_t              n_matches;
    bool                vs_find_all;
} stream_case_t;

static const match_pair_t k_basic_ab_matches[] = {
    {0, 2}, {4, 6}, {6, 8},
};
static const match_pair_t k_basic_digits_shortest_matches[] = {
    {1, 2}, {2, 3}, {4, 5},
};
static const match_pair_t k_branch_overtake_start_matches[] = {
    {1, 3},
};
static const match_pair_t k_begin_anchor_caret_match_matches[] = {
    {0, 3},
};
static const match_pair_t k_empty_input_nullable_matches[] = {
    {0, 0},
};
static const match_pair_t k_begin_anchor_only_at_start_matches[] = {
    {0, 3},
};
static const match_pair_t k_lookaround_match_span_matches[] = {
    {11, 13}, {37, 39},
};

static const stream_case_t k_cases[] = {
    {
        .name = "basic_ab", .pattern = "ab",
        .input = (const uint8_t *)"abxxabab", .input_len = 8,
        .matches = k_basic_ab_matches, .n_matches = 3,
        .vs_find_all = false,
    },
    {
        .name = "basic_digits_shortest", .pattern = "\\d+",
        .input = (const uint8_t *)"a12b3", .input_len = 5,
        .matches = k_basic_digits_shortest_matches, .n_matches = 3,
        .vs_find_all = false,
    },
    {
        .name = "branch_overtake_start", .pattern = "aab|cb",
        .input = (const uint8_t *)"acb", .input_len = 3,
        .matches = k_branch_overtake_start_matches, .n_matches = 1,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_caret_match", .pattern = "^foo",
        .input = (const uint8_t *)"foo", .input_len = 3,
        .matches = k_begin_anchor_caret_match_matches, .n_matches = 1,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_caret_no_match_xfoo", .pattern = "^foo",
        .input = (const uint8_t *)"xfoo", .input_len = 4,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_A_no_match_xfoo", .pattern = "\\Afoo",
        .input = (const uint8_t *)"xfoo", .input_len = 4,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_lb_A_no_match_xfoo",
        .pattern = "(?<=\\A)foo",
        .input = (const uint8_t *)"xfoo", .input_len = 4,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = true,
    },
    {
        .name = "empty_input_nullable", .pattern = "a*",
        .input = (const uint8_t *)"", .input_len = 0,
        .matches = k_empty_input_nullable_matches, .n_matches = 1,
        .vs_find_all = false,
    },
    {
        .name = "empty_input_non_nullable", .pattern = "a+",
        .input = (const uint8_t *)"", .input_len = 0,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = false,
    },
    {
        .name = "begin_anchor_empty_input", .pattern = "^foo",
        .input = (const uint8_t *)"", .input_len = 0,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_only_at_start", .pattern = "^foo",
        .input = (const uint8_t *)"fooXfoo", .input_len = 7,
        .matches = k_begin_anchor_only_at_start_matches, .n_matches = 1,
        .vs_find_all = true,
    },
    {
        .name = "begin_anchor_no_begin_match_body_rejected",
        .pattern = "^foo",
        .input = (const uint8_t *)"xfoofoo", .input_len = 7,
        .matches = nullptr, .n_matches = 0,
        .vs_find_all = true,
    },
    {
        .name = "lookaround_match_span",
        .pattern = "(?-u)(?<=<row Id=\")\\d+(?=\")",
        .input = (const uint8_t *)
                 "  <row Id=\"42\" Foo=\"bar\"/>  <row Id=\"99\" />",
        .input_len = 43,
        .matches = k_lookaround_match_span_matches, .n_matches = 2,
        .vs_find_all = true,
    },
};

// ---- Helpers --------------------------------------------------------------

static bool
match_list_equal_pairs(n00b_list_t(Match) *got, const match_pair_t *want,
                       size_t n_want)
{
    size_t n_got = n00b_list_len(*got);
    if (n_got != n_want) {
        return false;
    }
    for (size_t i = 0; i < n_got; ++i) {
        Match m = n00b_list_get(*got, i);
        if (m.start != want[i].start || m.end != want[i].end) {
            return false;
        }
    }
    return true;
}

static bool
match_list_equal(n00b_list_t(Match) *a, n00b_list_t(Match) *b)
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

static bool
usize_list_equal(n00b_list_t(size_t) *a, n00b_list_t(size_t) *b)
{
    size_t na = n00b_list_len(*a);
    size_t nb = n00b_list_len(*b);
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; ++i) {
        size_t va = n00b_list_get(*a, i);
        size_t vb = n00b_list_get(*b, i);
        if (va != vb) {
            return false;
        }
    }
    return true;
}

// ---- The fixture-driven test ----------------------------------------------

static void
test_stream_fixture(void)
{
    constexpr size_t n_cases = sizeof(k_cases) / sizeof(k_cases[0]);

    for (size_t i = 0; i < n_cases; ++i) {
        const stream_case_t *tc = &k_cases[i];

        Regex *re = regex_new(tc->pattern);
        assert(re != nullptr);

        n00b_list_t(Match) got = n00b_list_new_private(Match);
        n00b_result_t(int) r = regex_stream(re, tc->input, tc->input_len, &got);
        assert(!n00b_result_is_err(r));

        assert(match_list_equal_pairs(&got, tc->matches, tc->n_matches));

        if (tc->vs_find_all) {
            n00b_list_t(Match) all = n00b_list_new_private(Match);
            n00b_regex_engine_err_t ea = regex_find_all(re, tc->input,
                                                        tc->input_len, &all);
            assert(ea == N00B_REGEX_ENGINE_ERR_NONE);
            assert(match_list_equal(&got, &all));
            n00b_list_free(all);
        }

        n00b_list_free(got);
        regex_free(re);
    }

    printf("  [PASS] stream_fixture\n");
}

// ---- test_stream_prefix_skip_helps ----------------------------------------

static void
test_stream_prefix_skip_helps(void)
{
    // 50_000 repetitions of the haystack, each containing one match.
    size_t prefix_len = 44; // 44 dots
    const char *needle = "Id=\"42\" .";
    size_t needle_len = strlen(needle);
    size_t per_iter = prefix_len + needle_len;
    size_t total = 50000 * per_iter;
    char *data = n00b_alloc_array(char, total);
    char dots[45];
    memset(dots, '.', 44);
    dots[44] = 0;
    char *p = data;
    for (int i = 0; i < 50000; ++i) {
        memcpy(p, dots, prefix_len);
        p += prefix_len;
        memcpy(p, needle, needle_len);
        p += needle_len;
    }

    Regex *re = regex_new("Id=\"\\d+\"");
    assert(re != nullptr);

    n00b_list_t(Match) m = n00b_list_new_private(Match);
    n00b_result_t(int) r = regex_stream(re, (const uint8_t *)data, total, &m);
    assert(!n00b_result_is_err(r));
    assert(n00b_list_len(m) == 50000);

    n00b_list_free(m);
    regex_free(re);
    n00b_free(data);

    printf("  [PASS] stream_prefix_skip_helps\n");
}

// ---- test_stream_with_callback --------------------------------------------

typedef struct cb_ctx_t {
    n00b_list_t(Match) *vec;
    size_t              count;
    bool                fired;
} cb_ctx_t;

static void
cb_push(void *ctx, Match m)
{
    cb_ctx_t *c = (cb_ctx_t *)ctx;
    if (c->vec) {
        n00b_list_push(*c->vec, m);
    }
    c->count += 1;
    c->fired = true;
}

static void
test_stream_with_callback(void)
{
    Regex *re = regex_new("\\d+");
    assert(re != nullptr);

    const uint8_t input[] = "a12 b34 c5 d6789";
    size_t n = sizeof(input) - 1;

    n00b_list_t(Match) want = n00b_list_new_private(Match);
    n00b_result_t(int) r = regex_stream(re, input, n, &want);
    assert(!n00b_result_is_err(r));

    n00b_list_t(Match) got = n00b_list_new_private(Match);
    cb_ctx_t ctx = {.vec = &got, .count = 0, .fired = false};
    r = regex_stream_with(re, input, n, cb_push, &ctx);
    assert(!n00b_result_is_err(r));
    assert(match_list_equal(&got, &want));

    cb_ctx_t counter = {.vec = nullptr, .count = 0, .fired = false};
    r = regex_stream_with(re, input, n, cb_push, &counter);
    assert(!n00b_result_is_err(r));
    assert(counter.count == n00b_list_len(want));

    cb_ctx_t empty = {.vec = nullptr, .count = 0, .fired = false};
    r = regex_stream_with(re, (const uint8_t *)"", 0, cb_push, &empty);
    assert(!n00b_result_is_err(r));
    assert(!empty.fired);

    n00b_list_free(got);
    n00b_list_free(want);
    regex_free(re);

    printf("  [PASS] stream_with_callback\n");
}

// ---- test_cross_chunk_boundary --------------------------------------------

typedef struct end_ctx_t {
    n00b_list_t(size_t) *vec;
} end_ctx_t;

static void
end_push(void *ctx, size_t end)
{
    end_ctx_t *c = (end_ctx_t *)ctx;
    n00b_list_push(*c->vec, end);
}

static void
test_cross_chunk_boundary(void)
{
    Regex *re = regex_new("abcdef");
    assert(re != nullptr);

    n00b_list_t(size_t) got = n00b_list_new_private(size_t);
    end_ctx_t ctx = {.vec = &got};
    StreamState s = stream_state_new();
    StreamState ns;
    n00b_result_t(int) r = regex_stream_chunk(re, (const uint8_t *)"abc", 3,
                                              s, &ns, end_push, &ctx);
    assert(!n00b_result_is_err(r));
    s = ns;
    r = regex_stream_chunk(re, (const uint8_t *)"def", 3, s, &ns,
                           end_push, &ctx);
    assert(!n00b_result_is_err(r));

    n00b_list_t(size_t) want = n00b_list_new_private(size_t);
    r = regex_stream_ends(re, (const uint8_t *)"abcdef", 6, &want);
    assert(!n00b_result_is_err(r));
    assert(usize_list_equal(&got, &want));

    n00b_list_free(got);
    n00b_list_free(want);
    regex_free(re);

    printf("  [PASS] cross_chunk_boundary\n");
}

// ---- test_stream_chunk ----------------------------------------------------

static void
test_stream_chunk(void)
{
    Regex *re = regex_new("\\d+");
    assert(re != nullptr);

    const uint8_t input[] = "a12 b34 c5 d6789";
    size_t n = sizeof(input) - 1;

    n00b_list_t(size_t) want = n00b_list_new_private(size_t);
    n00b_result_t(int) r = regex_stream_ends(re, input, n, &want);
    assert(!n00b_result_is_err(r));

    size_t chunk_sizes[] = {1, 2, 3, 4, 7, 16, n};
    for (size_t i = 0; i < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); ++i) {
        size_t cs = chunk_sizes[i];
        n00b_list_t(size_t) got = n00b_list_new_private(size_t);
        end_ctx_t ctx = {.vec = &got};
        StreamState st = stream_state_new();
        size_t pos = 0;
        while (pos < n) {
            size_t this_len = (n - pos) < cs ? (n - pos) : cs;
            StreamState ns;
            n00b_result_t(int) rr = regex_stream_chunk(re, input + pos,
                                                       this_len, st, &ns,
                                                       end_push, &ctx);
            assert(!n00b_result_is_err(rr));
            st = ns;
            pos += this_len;
        }
        assert(usize_list_equal(&got, &want));
        n00b_list_free(got);
    }

    n00b_list_free(want);
    regex_free(re);

    printf("  [PASS] stream_chunk\n");
}

// ---- seek_fwd_rev_cursor --------------------------------------------------

static void
test_seek_fwd_rev_cursor(void)
{
    Regex *re = regex_new("a[bc]+d");
    assert(re != nullptr);

    const uint8_t input[] = "xx abcd yy abbcd zz acd ww abd";
    size_t n = sizeof(input) - 1;

    n00b_list_t(Match) sm = n00b_list_new_private(Match);
    n00b_result_t(int) r = regex_stream(re, input, n, &sm);
    assert(!n00b_result_is_err(r));

    n00b_list_t(size_t) fwd = n00b_list_new_private(size_t);
    uint32_t s = REGEX_SEEK_INITIAL;
    size_t   p = 0;
    for (;;) {
        bool found;
        uint32_t ns;
        size_t end_pos;
        n00b_result_t(int) rr = regex_seek_fwd(re, input, n, s, p,
                                               &found, &ns, &end_pos);
        assert(!n00b_result_is_err(rr));
        if (!found) {
            break;
        }
        n00b_list_push(fwd, end_pos);
        s = ns;
        p = end_pos;
    }
    n00b_list_t(size_t) want_ends = n00b_list_new_private(size_t);
    for (size_t i = 0; i < n00b_list_len(sm); ++i) {
        Match m = n00b_list_get(sm, i);
        n00b_list_push(want_ends, m.end);
    }
    assert(usize_list_equal(&fwd, &want_ends));

    n00b_list_t(size_t) rev = n00b_list_new_private(size_t);
    s = REGEX_SEEK_INITIAL;
    p = n;
    for (;;) {
        bool found;
        uint32_t ns;
        size_t start_pos;
        n00b_result_t(int) rr = regex_seek_rev(re, input, n, s, p,
                                               &found, &ns, &start_pos);
        assert(!n00b_result_is_err(rr));
        if (!found) {
            break;
        }
        n00b_list_push(rev, start_pos);
        s = ns;
        p = start_pos;
    }
    n00b_list_t(size_t) want_starts = n00b_list_new_private(size_t);
    for (size_t i = n00b_list_len(sm); i > 0; --i) {
        Match m = n00b_list_get(sm, i - 1);
        n00b_list_push(want_starts, m.start);
    }
    if (!usize_list_equal(&rev, &want_starts)) {
        fprintf(stderr, "seek_rev starts mismatch: got=[");
        for (size_t i = 0; i < n00b_list_len(rev); ++i) {
            fprintf(stderr, "%s%zu", i == 0 ? "" : ", ",
                    n00b_list_get(rev, i));
        }
        fprintf(stderr, "] expected=[");
        for (size_t i = 0; i < n00b_list_len(want_starts); ++i) {
            fprintf(stderr, "%s%zu", i == 0 ? "" : ", ",
                    n00b_list_get(want_starts, i));
        }
        fprintf(stderr, "]\n");
    }
    assert(usize_list_equal(&rev, &want_starts));

    n00b_list_free(fwd);
    n00b_list_free(rev);
    n00b_list_free(want_ends);
    n00b_list_free(want_starts);
    n00b_list_free(sm);
    regex_free(re);

    printf("  [PASS] seek_fwd_rev_cursor\n");
}

// ---- seek_fwd_from_middle / seek_rev_from_middle --------------------------

static void
test_seek_fwd_from_middle(void)
{
    Regex *re = regex_new("lookaround");
    assert(re != nullptr);

    const uint8_t input[] = "foo lookaround bar baz lookaround qux end";
    size_t n = sizeof(input) - 1;
    bool found;
    uint32_t ns;
    size_t end;
    n00b_result_t(int) r = regex_seek_fwd(re, input, n, REGEX_SEEK_INITIAL,
                                          20, &found, &ns, &end);
    assert(!n00b_result_is_err(r));
    assert(found);
    assert(end == 33);
    assert(memcmp(input + end - 10, "lookaround", 10) == 0);

    regex_free(re);
    printf("  [PASS] seek_fwd_from_middle\n");
}

static void
test_seek_rev_from_middle(void)
{
    Regex *re = regex_new("lookaround");
    assert(re != nullptr);

    const uint8_t input[] = "foo lookaround bar baz lookaround qux end";
    bool found;
    uint32_t ns;
    size_t start;
    n00b_result_t(int) r = regex_seek_rev(re, input, sizeof(input) - 1,
                                          REGEX_SEEK_INITIAL, 20, &found,
                                          &ns, &start);
    assert(!n00b_result_is_err(r));
    assert(found);
    assert(start == 4);
    assert(memcmp(input + start, "lookaround", 10) == 0);

    regex_free(re);
    printf("  [PASS] seek_rev_from_middle\n");
}

// ---- seek_no_match --------------------------------------------------------

static void
test_seek_no_match(void)
{
    Regex *re = regex_new("zzz");
    assert(re != nullptr);

    const uint8_t input[] = "the quick brown fox jumps over the lazy dog";
    size_t n = sizeof(input) - 1;
    bool found;
    uint32_t ns;
    size_t e_or_s;
    n00b_result_t(int) r = regex_seek_fwd(re, input, n, REGEX_SEEK_INITIAL,
                                          10, &found, &ns, &e_or_s);
    assert(!n00b_result_is_err(r));
    assert(!found);

    r = regex_seek_rev(re, input, n, REGEX_SEEK_INITIAL, 30,
                       &found, &ns, &e_or_s);
    assert(!n00b_result_is_err(r));
    assert(!found);

    regex_free(re);
    printf("  [PASS] seek_no_match\n");
}

// ---- seek_fwd_skips_match_before_pos --------------------------------------

static void
test_seek_fwd_skips_match_before_pos(void)
{
    Regex *re = regex_new("abcdef");
    assert(re != nullptr);

    const uint8_t input[] = "xx abcdef yy abcdef zz";
    size_t n = sizeof(input) - 1;
    bool found;
    uint32_t ns;
    size_t end;
    n00b_result_t(int) r = regex_seek_fwd(re, input, n, REGEX_SEEK_INITIAL,
                                          0, &found, &ns, &end);
    assert(!n00b_result_is_err(r));
    assert(found);
    assert(end == 9);

    r = regex_seek_fwd(re, input, n, REGEX_SEEK_INITIAL, 5,
                       &found, &ns, &end);
    assert(!n00b_result_is_err(r));
    assert(found);
    assert(end == 19);

    r = regex_seek_fwd(re, input, n, REGEX_SEEK_INITIAL, 20,
                       &found, &ns, &end);
    assert(!n00b_result_is_err(r));
    assert(!found);

    regex_free(re);
    printf("  [PASS] seek_fwd_skips_match_before_pos\n");
}

// ---- seek_fwd_with_class_pattern ------------------------------------------

static void
test_seek_fwd_with_class_pattern(void)
{
    Regex *re = regex_new("\\d+");
    assert(re != nullptr);

    const uint8_t input[] = "abc 123 def 4567 ghi 89 jkl";
    size_t n = sizeof(input) - 1;
    n00b_list_t(size_t) got = n00b_list_new_private(size_t);
    uint32_t s = REGEX_SEEK_INITIAL;
    size_t   p = 8;
    for (;;) {
        bool found;
        uint32_t ns;
        size_t end;
        n00b_result_t(int) r = regex_seek_fwd(re, input, n, s, p,
                                              &found, &ns, &end);
        assert(!n00b_result_is_err(r));
        if (!found) {
            break;
        }
        n00b_list_push(got, end);
        s = ns;
        p = end;
    }
    n00b_list_t(size_t) want = n00b_list_new_private(size_t);
    size_t expected[] = {13, 14, 15, 16, 22, 23};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        n00b_list_push(want, expected[i]);
    }
    assert(usize_list_equal(&got, &want));

    n00b_list_free(got);
    n00b_list_free(want);
    regex_free(re);

    printf("  [PASS] seek_fwd_with_class_pattern\n");
}

// ---- Test runner entry point ----------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex stream tests...\n");

    test_stream_fixture();
    test_stream_prefix_skip_helps();
    test_stream_with_callback();
    test_cross_chunk_boundary();
    test_stream_chunk();
    test_seek_fwd_rev_cursor();
    test_seek_fwd_from_middle();
    test_seek_rev_from_middle();
    test_seek_no_match();
    test_seek_fwd_skips_match_before_pos();
    test_seek_fwd_with_class_pattern();

    printf("All regex stream tests passed.\n");
    n00b_shutdown();
    return 0;
}
