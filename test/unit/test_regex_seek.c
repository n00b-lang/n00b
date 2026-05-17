// Phase 11 typed translation of resharp-c/tests/seek_test.c.
//
// The original test exercised the engine-level seek API
// (`regex_seek_fwd` / `regex_seek_rev`) which takes a raw byte buffer
// plus an explicit (state, pos) pair and out-params.  The translated
// form uses the public n00b API (§ 4): `n00b_string_t *` input plus an
// `n00b_regex_cursor_t *` that carries the (state, pos) pair across
// calls, with results returned as `n00b_option_t(n00b_regex_match_t)`.
//
// Pattern + input data are byte-identical to the source; only the
// container types and call shapes change.

#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/option.h"
#include "adt/result.h"
#include "text/regex/regex.h"

#define ERROR_TAG "ERROR"
#define ERROR_LEN 5

// build_input: 5 line-pairs ("line N no match here\nline N ERROR
// something\n").  Returns an n00b_string_t * input in *out and fills
// *out_ends with the expected ERROR-end byte offsets.  The list value
// must be a freshly-initialised n00b_list_new(size_t).
static void
build_input(n00b_string_t **out, n00b_list_t(size_t) *out_ends)
{
    constexpr size_t cap = 4096;
    char            *buf = n00b_alloc_array(char, cap);
    size_t           len = 0;

    for (int i = 0; i < 5; ++i) {
        char line1[64];
        int  l1 = snprintf(line1, sizeof(line1),
                           "line %d no match here\n", i);
        assert(len + (size_t)l1 < cap);
        memcpy(buf + len, line1, (size_t)l1);
        len += (size_t)l1;

        char   line2[64];
        int    l2    = snprintf(line2, sizeof(line2),
                                "line %d ERROR something\n", i);
        size_t start = len;
        assert(len + (size_t)l2 < cap);
        memcpy(buf + len, line2, (size_t)l2);
        len += (size_t)l2;
        // Find offset of "ERROR" within line2.
        const char *p = strstr(line2, ERROR_TAG);
        assert(p != nullptr);
        size_t off = (size_t)(p - line2);
        n00b_list_push(*out_ends, start + off + ERROR_LEN);
    }

    *out = n00b_string_from_raw(buf, (int64_t)len);
}

static bool
size_list_eq(n00b_list_t(size_t) a, n00b_list_t(size_t) b)
{
    size_t an = n00b_list_len(a);
    size_t bn = n00b_list_len(b);
    if (an != bn) {
        return false;
    }
    for (size_t i = 0; i < an; ++i) {
        if (n00b_list_get(a, i) != n00b_list_get(b, i)) {
            return false;
        }
    }
    return true;
}

// ---- seek_fwd_walks_all_matches -------------------------------------------

static void
test_seek_fwd_walks_all_matches(void)
{
    n00b_result_t(n00b_regex_t *) rr =
        n00b_regex_new(n00b_string_from_cstr("\\bERROR\\b"));
    assert(n00b_result_is_ok(rr));
    n00b_regex_t *re = n00b_result_get(rr);

    n00b_string_t      *input = nullptr;
    n00b_list_t(size_t) want  = n00b_list_new(size_t);
    build_input(&input, &want);

    n00b_list_t(size_t)  got = n00b_list_new(size_t);
    n00b_regex_cursor_t *cur = n00b_regex_cursor_new(re);
    for (;;) {
        n00b_option_t(n00b_regex_match_t) m =
            n00b_regex_seek_fwd(re, input, cur);
        if (!n00b_option_is_set(m)) {
            break;
        }
        n00b_regex_match_t hit = n00b_option_get(m);
        n00b_list_push(got, (size_t)hit.end);
    }
    assert(size_list_eq(got, want));

    printf("  [PASS] seek_fwd_walks_all_matches\n");
}

// ---- seek_rev_walks_all_matches_rightmost_first ---------------------------

static void
test_seek_rev_walks_all_matches_rightmost_first(void)
{
    n00b_result_t(n00b_regex_t *) rr =
        n00b_regex_new(n00b_string_from_cstr("\\bERROR\\b"));
    assert(n00b_result_is_ok(rr));
    n00b_regex_t *re = n00b_result_get(rr);

    n00b_string_t      *input = nullptr;
    n00b_list_t(size_t) ends  = n00b_list_new(size_t);
    build_input(&input, &ends);

    n00b_list_t(size_t) want_starts = n00b_list_new(size_t);
    size_t              en          = n00b_list_len(ends);
    for (size_t i = en; i > 0; --i) {
        size_t e = n00b_list_get(ends, i - 1);
        n00b_list_push(want_starts, e - ERROR_LEN);
    }

    n00b_list_t(size_t)  got = n00b_list_new(size_t);
    n00b_regex_cursor_t *cur = n00b_regex_cursor_at(re, input->u8_bytes);
    for (;;) {
        n00b_option_t(n00b_regex_match_t) m =
            n00b_regex_seek_rev(re, input, cur);
        if (!n00b_option_is_set(m)) {
            break;
        }
        n00b_regex_match_t hit = n00b_option_get(m);
        n00b_list_push(got, (size_t)hit.start);
    }
    assert(size_list_eq(got, want_starts));

    printf("  [PASS] seek_rev_walks_all_matches_rightmost_first\n");
}

// ---- seek_fwd_respects_word_boundary --------------------------------------

static void
test_seek_fwd_respects_word_boundary(void)
{
    n00b_result_t(n00b_regex_t *) rr =
        n00b_regex_new(n00b_string_from_cstr("\\bERROR\\b"));
    assert(n00b_result_is_ok(rr));
    n00b_regex_t *re = n00b_result_get(rr);

    n00b_string_t *input =
        n00b_string_from_cstr("xERRORx ERROR yERRORy ERROR.");

    n00b_list_t(size_t)  got = n00b_list_new(size_t);
    n00b_regex_cursor_t *cur = n00b_regex_cursor_new(re);
    for (;;) {
        n00b_option_t(n00b_regex_match_t) m =
            n00b_regex_seek_fwd(re, input, cur);
        if (!n00b_option_is_set(m)) {
            break;
        }
        n00b_regex_match_t hit = n00b_option_get(m);
        n00b_list_push(got, (size_t)hit.end);
    }
    assert(n00b_list_len(got) == 2);
    assert(n00b_list_get(got, 0) == 13);
    assert(n00b_list_get(got, 1) == 27);

    printf("  [PASS] seek_fwd_respects_word_boundary\n");
}

// ---- seek_fwd_from_offset_skips_earlier_matches ---------------------------

static void
test_seek_fwd_from_offset_skips_earlier_matches(void)
{
    n00b_result_t(n00b_regex_t *) rr =
        n00b_regex_new(n00b_string_from_cstr("\\bERROR\\b"));
    assert(n00b_result_is_ok(rr));
    n00b_regex_t *re = n00b_result_get(rr);

    n00b_string_t *input =
        n00b_string_from_cstr("ERROR aaa ERROR bbb ERROR");

    // From offset 6, the next match ends at 15.
    n00b_regex_cursor_t *c6 = n00b_regex_cursor_at(re, 6);
    n00b_option_t(n00b_regex_match_t) m6 =
        n00b_regex_seek_fwd(re, input, c6);
    assert(n00b_option_is_set(m6));
    assert(n00b_option_get(m6).end == 15);

    // From offset 16, the next match ends at 25.
    n00b_regex_cursor_t *c16 = n00b_regex_cursor_at(re, 16);
    n00b_option_t(n00b_regex_match_t) m16 =
        n00b_regex_seek_fwd(re, input, c16);
    assert(n00b_option_is_set(m16));
    assert(n00b_option_get(m16).end == 25);

    // From offset 25 (end of input), no further match.
    n00b_regex_cursor_t *c25 = n00b_regex_cursor_at(re, 25);
    n00b_option_t(n00b_regex_match_t) m25 =
        n00b_regex_seek_fwd(re, input, c25);
    assert(!n00b_option_is_set(m25));

    printf("  [PASS] seek_fwd_from_offset_skips_earlier_matches\n");
}

// ---- seek_rev_from_offset_skips_later_matches -----------------------------

static void
test_seek_rev_from_offset_skips_later_matches(void)
{
    n00b_result_t(n00b_regex_t *) rr =
        n00b_regex_new(n00b_string_from_cstr("\\bERROR\\b"));
    assert(n00b_result_is_ok(rr));
    n00b_regex_t *re = n00b_result_get(rr);

    n00b_string_t *input =
        n00b_string_from_cstr("ERROR aaa ERROR bbb ERROR");

    // From offset 10, the previous match starts at 0.
    n00b_regex_cursor_t *c10 = n00b_regex_cursor_at(re, 10);
    n00b_option_t(n00b_regex_match_t) m10 =
        n00b_regex_seek_rev(re, input, c10);
    assert(n00b_option_is_set(m10));
    assert(n00b_option_get(m10).start == 0);

    // From offset 0, no earlier match.
    n00b_regex_cursor_t *c0 = n00b_regex_cursor_at(re, 0);
    n00b_option_t(n00b_regex_match_t) m0 =
        n00b_regex_seek_rev(re, input, c0);
    assert(!n00b_option_is_set(m0));

    printf("  [PASS] seek_rev_from_offset_skips_later_matches\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex seek tests...\n");

    test_seek_fwd_walks_all_matches();
    test_seek_rev_walks_all_matches_rightmost_first();
    test_seek_fwd_respects_word_boundary();
    test_seek_fwd_from_offset_skips_earlier_matches();
    test_seek_rev_from_offset_skips_later_matches();

    printf("All regex seek tests passed.\n");
    n00b_shutdown();
    return 0;
}
