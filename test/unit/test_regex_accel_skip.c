// Phase 11 port of resharp-c/tests/accel_skip_test.c -- BFS derivative
// prefix-skip optimization exercise.  Test cases mirror
// test/data/regex/accel_skip.toml byte-for-byte (patterns, inputs,
// expected matches in utf-8 half-open intervals).

#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/list.h"
#include "adt/result.h"
#include "text/regex/regex.h"

// ============================================================================
// Embedded test fixture -- byte-identical to test/data/regex/accel_skip.toml.
// ============================================================================

typedef struct {
    int64_t start;
    int64_t end;
} expected_match_t;

typedef struct {
    const char             *pattern;
    const char             *input;
    size_t                  input_len;
    const expected_match_t *matches;
    size_t                  matches_len;
} loaded_test_t;

#define M0      ((const expected_match_t *)nullptr)
#define MN(arr) ((const expected_match_t *)(arr))
#define LEN_OF(arr) (sizeof(arr) / sizeof((arr)[0]))

static const expected_match_t m_hello[]            = {{1, 2}, {4, 5}};
static const expected_match_t m_10am[]             = {{0, 2}};
static const expected_match_t m_secondline[]       = {{6, 17}};
static const expected_match_t m_abcx[]             = {{0, 4}};
static const expected_match_t m_exact[]            = {{0, 5}};
static const expected_match_t m_huck[]             = {{0, 30}};
static const expected_match_t m_world_end[]        = {{6, 11}};
static const expected_match_t m_hello_anchor[]     = {{0, 5}};
static const expected_match_t m_digits_anchor[]    = {{0, 5}};
static const expected_match_t m_keyvalue[]         = {{0, 9}};
static const expected_match_t m_a_eq_b_c_eq_d[]    = {{0, 7}};
static const expected_match_t m_eqs[]              = {{0, 3}};
static const expected_match_t m_xxabcxx[]          = {{2, 5}};
static const expected_match_t m_abcabc[]           = {{0, 3}, {3, 6}};
static const expected_match_t m_abc_only[]         = {{0, 3}};
static const expected_match_t m_z_end[]            = {{20, 21}};
static const expected_match_t m_z_start[]          = {{0, 1}};
static const expected_match_t m_needle[]           = {{32, 38}};
static const expected_match_t m_aababab[]          = {{1, 3}, {3, 5}, {5, 7}};
static const expected_match_t m_x_alone[]          = {{0, 1}};
static const expected_match_t m_hello_twice[]      = {{4, 9}, {13, 18}};
static const expected_match_t m_q_in_zeros[]       = {{44, 45}};
static const expected_match_t m_digits_class[]     = {{3, 6}, {9, 12}};
static const expected_match_t m_neg_class[]        = {{3, 9}};
static const expected_match_t m_titlecase[]        = {{0, 5}, {6, 11}, {12, 15}};
static const expected_match_t m_aaa_class[]        = {{3, 6}};
static const expected_match_t m_aaa[]              = {{0, 3}};
static const expected_match_t m_abab[]             = {{2, 6}, {8, 10}};
static const expected_match_t m_abbc[]             = {{1, 5}, {6, 11}, {12, 18}};
static const expected_match_t m_a3[]               = {{0, 3}, {3, 6}};
static const expected_match_t m_dotg[]             = {{6, 9}, {10, 13}, {15, 18}};
static const expected_match_t m_adotb[]            = {{0, 3}, {4, 7}, {8, 11}};
static const expected_match_t m_cat_dog[]          = {{9, 12}, {19, 22}};
static const expected_match_t m_aabb[]             = {{0, 2}, {2, 4}};
static const expected_match_t m_foobarbaz[]        = {{3, 6}, {9, 12}, {15, 18}};
static const expected_match_t m_nondigitseq[]      = {{0, 3}, {3, 9}, {9, 9}};
static const expected_match_t m_nonand[]           = {{0, 6}, {6, 9}, {9, 9}};
static const expected_match_t m_nonnd[]            = {{0, 2}, {2, 4}, {4, 4}};
static const expected_match_t m_a_no_e[]           = {{0, 4}};
static const expected_match_t m_cats_andint[]      = {{8, 12}};
static const expected_match_t m_cats[]             = {{0, 4}};
static const expected_match_t m_abc_int[]          = {{0, 3}};
static const expected_match_t m_password_clean[]   = {{9, 21}};
static const expected_match_t m_password_clean2[]  = {{11, 23}};
static const expected_match_t m_a_la[]             = {{1, 2}, {4, 5}};
static const expected_match_t m_bb_la[]            = {{2, 4}};
static const expected_match_t m_ab_la[]            = {{1, 3}, {4, 6}};
static const expected_match_t m_lb_a[]             = {{1, 2}};
static const expected_match_t m_lb_a2[]            = {{4, 5}};
static const expected_match_t m_lb_aaa_rest[]      = {{3, 6}};
static const expected_match_t m_lb_author_rest[]   = {{6, 19}};
static const expected_match_t m_nla_d_a[]          = {{4, 5}, {7, 8}};
static const expected_match_t m_nla_b_c[]          = {{1, 2}};
static const expected_match_t m_ipv4[]             = {{13, 26}, {37, 45}};
static const expected_match_t m_email[]            = {{8, 24}, {28, 42}};
static const expected_match_t m_html_comment[]     = {{0, 18}};
static const expected_match_t m_date[]             = {{0, 7}};
static const expected_match_t m_abc_under[]        = {{0, 3}};
static const expected_match_t m_under_abc[]        = {{15, 18}};
static const expected_match_t m_xyz_multi[]        = {{0, 3}, {3, 6}, {19, 22}, {22, 25}};
static const expected_match_t m_paragraphs[]       = {{1, 6}, {6, 11}, {11, 16}};
static const expected_match_t m_q_to_q[]           = {{0, 14}};
static const expected_match_t m_dotstar_aaa[]      = {{0, 6}};
static const expected_match_t m_wb_11[]            = {{0, 2}};
static const expected_match_t m_wb_11_space[]      = {{1, 3}};
static const expected_match_t m_11_wb[]            = {{0, 2}};
static const expected_match_t m_11_wb_space[]      = {{0, 2}};
static const expected_match_t m_marker[]           = {{74, 80}, {150, 156}};
static const expected_match_t m_xy_end[]           = {{100, 102}};
static const expected_match_t m_lethargy_air[]     = {{1, 26}};
static const expected_match_t m_parens[]           = {{1, 13}};
static const expected_match_t m_quoted[]           = {{4, 11}, {16, 21}};
static const expected_match_t m_html_h[]           = {{0, 31}};
static const expected_match_t m_brace_inner1[]     = {{2, 9}};
static const expected_match_t m_brace_inner2[]     = {{2, 7}, {9, 11}};
static const expected_match_t m_finn[]             = {{0, 25}};
static const expected_match_t m_dotstar_inner[]    = {{0, 73}};
static const expected_match_t m_currency[]         = {{6, 12}, {19, 27}, {32, 43}};

// Inputs containing embedded newlines that must be byte-identical to the
// TOML.  Each block uses an explicit-length literal to keep things obvious.
static const char in_second[]   = "first\nsecond=line\nthird";
static const char in_paragraph[] = "\naaa\n\nbbb\n\nccc\n\n";
static const char in_lethargy[]  = "\nlethargy, and and the air tainted with\nc\n";

static const loaded_test_t k_tests[] = {
    {"[aeiou]", "hello", 5, MN(m_hello), LEN_OF(m_hello)},
    {"[0-9]+(?=[aA]\\.?[mM]\\.?)", "10am", 4, MN(m_10am), LEN_OF(m_10am)},
    {".*=.*", in_second, sizeof in_second - 1, MN(m_secondline), LEN_OF(m_secondline)},
    {".+x", "abcx", 4, MN(m_abcx), LEN_OF(m_abcx)},
    {"^exact$", "exact", 5, MN(m_exact), LEN_OF(m_exact)},
    {".*Huck.*&~(.*F.*)",
     "The Adventures of Huckleberry Finn', published in 1885.",
     55, MN(m_huck), LEN_OF(m_huck)},
    {"world$", "hello world", 11, MN(m_world_end), LEN_OF(m_world_end)},
    {"^hello", "hello world", 11, MN(m_hello_anchor), LEN_OF(m_hello_anchor)},
    {"^[0-9]+$", "12345", 5, MN(m_digits_anchor), LEN_OF(m_digits_anchor)},
    {".*=.*", "key=value", 9, MN(m_keyvalue), LEN_OF(m_keyvalue)},
    {".*=.*", "no equals here", 14, M0, 0},
    {".*=.*", "a=b c=d", 7, MN(m_a_eq_b_c_eq_d), LEN_OF(m_a_eq_b_c_eq_d)},
    {".*=.*", "===", 3, MN(m_eqs), LEN_OF(m_eqs)},
    {"abc", "xxabcxx", 7, MN(m_xxabcxx), LEN_OF(m_xxabcxx)},
    {"abc", "abcabc", 6, MN(m_abcabc), LEN_OF(m_abcabc)},
    {"abc", "xyzxyz", 6, M0, 0},
    {"abc", "abc", 3, MN(m_abc_only), LEN_OF(m_abc_only)},
    {"z", "aaaaaaaaaaaaaaaaaaaaz", 21, MN(m_z_end), LEN_OF(m_z_end)},
    {"z", "zaaaaaaaaaaaaaaaaaaa", 20, MN(m_z_start), LEN_OF(m_z_start)},
    {"needle", "haystackhaystackhaystackhaystackneedlehaystack", 46,
     MN(m_needle), LEN_OF(m_needle)},
    {"needle", "nope", 4, M0, 0},
    {"ab", "aababab", 7, MN(m_aababab), LEN_OF(m_aababab)},
    {"x", "x", 1, MN(m_x_alone), LEN_OF(m_x_alone)},
    {"hello", "    hello    hello    ", 22, MN(m_hello_twice), LEN_OF(m_hello_twice)},
    {"q", "00000000000000000000000000000000000000000000q000", 48,
     MN(m_q_in_zeros), LEN_OF(m_q_in_zeros)},
    {"q", "000000000000000000000000000000000000000000000000", 48, M0, 0},
    {"[0-9]+", "abc123def456", 12, MN(m_digits_class), LEN_OF(m_digits_class)},
    {"[aeiou]", "bcdfg", 5, M0, 0},
    {"[^a-z]+", "abc123ABC", 9, MN(m_neg_class), LEN_OF(m_neg_class)},
    {"[A-Z][a-z]+", "Hello World Foo", 15, MN(m_titlecase), LEN_OF(m_titlecase)},
    {"^\\d+$", "123a5", 5, M0, 0},
    {"^exact$", "not exact", 9, M0, 0},
    {"a+", "bbbaaabbb", 9, MN(m_aaa_class), LEN_OF(m_aaa_class)},
    {"a+", "aaa", 3, MN(m_aaa), LEN_OF(m_aaa)},
    {"(ab)+", "__abab__ab__", 12, MN(m_abab), LEN_OF(m_abab)},
    {"ab{2,4}c", "xabbcxabbbcxabbbbcx", 19, MN(m_abbc), LEN_OF(m_abbc)},
    {"a{3}", "aaaaaa", 6, MN(m_a3), LEN_OF(m_a3)},
    {"..g", "dfdff dfggg gfgdfg gddfdf", 25, MN(m_dotg), LEN_OF(m_dotg)},
    {"a.b", "axb ayb azb", 11, MN(m_adotb), LEN_OF(m_adotb)},
    {"cat|dog", "I have a cat and a dog", 22, MN(m_cat_dog), LEN_OF(m_cat_dog)},
    {"aa|bb", "aabb", 4, MN(m_aabb), LEN_OF(m_aabb)},
    {"foo|bar|baz", "___foo___bar___baz___", 21, MN(m_foobarbaz), LEN_OF(m_foobarbaz)},
    {"~(_*\\d\\d_*)", "Aa11aBaAA", 9, MN(m_nondigitseq), LEN_OF(m_nondigitseq)},
    {"~(.*and.*)", "__A and B", 9, MN(m_nonand), LEN_OF(m_nonand)},
    {"~(.*nd.*)", "_ndB", 4, MN(m_nonnd), LEN_OF(m_nonnd)},
    {"a~(_*e_*)", "abcdefghijklmnop", 16, MN(m_a_no_e), LEN_OF(m_a_no_e)},
    {"c...&...s", "raining cats and dogs", 21, MN(m_cats_andint), LEN_OF(m_cats_andint)},
    {"c.*&.*s", "cats blah blah blah", 19, MN(m_cats), LEN_OF(m_cats)},
    {".*a.*&.*b.*&.*c.*", "abc", 3, MN(m_abc_int), LEN_OF(m_abc_int)},
    {"~(.*\\d\\d.*)&[a-zA-Z\\d]{8,}", "tej55zhA25wXu8bvQxFxt", 21,
     MN(m_password_clean), LEN_OF(m_password_clean)},
    {"~(.*\\d\\d.*)&[a-zA-Z\\d]{8,}", "y tej55zhA25wXu8bvQxFxt o", 25,
     MN(m_password_clean2), LEN_OF(m_password_clean2)},
    {".*[a-z].*&.*[A-Z].*&.*\\d.*&[a-zA-Z\\d]{8,}&~(.*\\d\\d.*)",
     "y tej55zhA25wXu8bvQxFxt o", 25,
     MN(m_password_clean2), LEN_OF(m_password_clean2)},
    {"a(?=b)", "_ab_ab_", 7, MN(m_a_la), LEN_OF(m_a_la)},
    {"bb(?=aa)", "__bbaa__", 8, MN(m_bb_la), LEN_OF(m_bb_la)},
    {"(ab)+(?=_)", "_ab_ab_", 7, MN(m_ab_la), LEN_OF(m_ab_la)},
    {"(?<=b)a", "ba", 2, MN(m_lb_a), LEN_OF(m_lb_a)},
    {"(?<=b)a", "bbbba", 5, MN(m_lb_a2), LEN_OF(m_lb_a2)},
    {"(?<=aaa).*", "aaabbb", 6, MN(m_lb_aaa_rest), LEN_OF(m_lb_aaa_rest)},
    {"(?<=author).*", "author: abc and def", 19,
     MN(m_lb_author_rest), LEN_OF(m_lb_author_rest)},
    {"(?<!\\d)a", "1a__a__a", 8, MN(m_nla_d_a), LEN_OF(m_nla_d_a)},
    {"(?<!b)c", " c", 2, MN(m_nla_b_c), LEN_OF(m_nla_b_c)},
    {"\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}",
     "connect from 192.168.1.100 to server 10.0.0.1 on port 443", 57,
     MN(m_ipv4), LEN_OF(m_ipv4)},
    {"[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}",
     "contact user@example.com or admin@test.org for info", 51,
     MN(m_email), LEN_OF(m_email)},
    {"<!--[\\s\\S]*--[ \\t]*>", "<!-- anything -- >", 18,
     MN(m_html_comment), LEN_OF(m_html_comment)},
    {"((\\d{2})|(\\d))\\/((\\d{2})|(\\d))\\/((\\d{4})|(\\d{2}))",
     "4/05/89", 7, MN(m_date), LEN_OF(m_date)},
    {"abc", "abc_______________", 18, MN(m_abc_under), LEN_OF(m_abc_under)},
    {"abc", "_______________abc", 18, MN(m_under_abc), LEN_OF(m_under_abc)},
    {"xyz", "xyzxyz_____________xyzxyz", 25, MN(m_xyz_multi), LEN_OF(m_xyz_multi)},
    {"(?:.+\n)+\n", in_paragraph, sizeof in_paragraph - 1,
     MN(m_paragraphs), LEN_OF(m_paragraphs)},
    {"q.*q", "q my comment q", 14, MN(m_q_to_q), LEN_OF(m_q_to_q)},
    {".*a{3}", "aa aaa", 6, MN(m_dotstar_aaa), LEN_OF(m_dotstar_aaa)},
    {"(?<=\\W|\\A)11", "11", 2, MN(m_wb_11), LEN_OF(m_wb_11)},
    {"(?<=\\W|\\A)11", " 11", 3, MN(m_wb_11_space), LEN_OF(m_wb_11_space)},
    {"11(?=\\W|\\z)", "11", 2, MN(m_11_wb), LEN_OF(m_11_wb)},
    {"11(?=\\W|\\z)", "11 ", 3, MN(m_11_wb_space), LEN_OF(m_11_wb_space)},
    {"MARKER",
     "..........................................................................MARKER......................................................................MARKER..........",
     170, MN(m_marker), LEN_OF(m_marker)},
    {"XY",
     "____________________________________________________________________________________________________XY",
     102, MN(m_xy_end), LEN_OF(m_xy_end)},
    {"lethargy.*air", in_lethargy, sizeof in_lethargy - 1,
     MN(m_lethargy_air), LEN_OF(m_lethargy_air)},
    {"\\(.*\\)", "f(x) and g(y)", 13, MN(m_parens), LEN_OF(m_parens)},
    {"\"[^\"]*\"", "say \"hello\" and \"bye\"", 21,
     MN(m_quoted), LEN_OF(m_quoted)},
    {"(?<!\xe9\xbf\x9b" ")(apres)\\b", "\xe9\xbf\x9b" "apres", 8, M0, 0},
    {"<h[1-6]>.*</h[1-6]>", "<h1>Title</h1> and <h2>Sub</h2>", 31,
     MN(m_html_h), LEN_OF(m_html_h)},
    {"<h.{1,60}>.*<\\/h(5|3|2|1|4)>",
     "<li><a href=\"/wiki/Lattes_Editori\" title=\"Lattes Editori\">Lattes Editori</a></li>",
     81, M0, 0},
    {"ab{0,5}c", "bbabbbbbc", 9, MN(m_brace_inner1), LEN_OF(m_brace_inner1)},
    {"ab{0,5}c", "bbabbbcbbac", 11, MN(m_brace_inner2), LEN_OF(m_brace_inner2)},
    {"F.*&~(.*Finn)", "Finn', published in 1885.", 25,
     MN(m_finn), LEN_OF(m_finn)},
    {".*=.*",
     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaakey=value",
     73, MN(m_dotstar_inner), LEN_OF(m_dotstar_inner)},
    {"(?:USD|EUR|GBP|JPY|CNY|INR|CAD|AUD|CHF|SEK|NOK|DKK|HKD|SGD|NZD|MXN|BRL|ZAR|RUB|KRW|TRY|PLN|THB|IDR|HUF|CZK|ILS|CLP|PHP|AED|COP|SAR|RON)\\s*\\d+(?:,\\d{3})*(?:\\.\\d{2})?",
     "hello USD100 world EUR42.50 foo GBP1,000.00 bar", 47,
     MN(m_currency), LEN_OF(m_currency)},
};

// ============================================================================
// accel_skip_lazy -- one regex compile + find_all per case.
// Mirrors resharp-c::accel_skip_lazy (the `#[ignore]` variant).
// ============================================================================

static void
test_accel_skip_lazy(void)
{
    size_t n = sizeof k_tests / sizeof k_tests[0];

    for (size_t i = 0; i < n; ++i) {
        const loaded_test_t *t = &k_tests[i];

        n00b_string_t *pat = n00b_string_from_cstr(t->pattern);
        n00b_result_t(n00b_regex_t *) rc =
            n00b_regex_new(pat, .max_dfa_capacity = 10000);
        if (!n00b_result_is_ok(rc)) {
            fprintf(stderr, "[case %zu] compile failed: pattern=%s\n",
                    i, t->pattern);
            assert(n00b_result_is_ok(rc));
        }
        n00b_regex_t *re = n00b_result_get(rc);

        n00b_string_t *input = n00b_string_from_raw(t->input,
                                                    (int64_t)t->input_len);
        n00b_list_t(n00b_regex_match_t) *got = n00b_regex_matches(re, input);
        assert(got != nullptr);

        size_t got_len = n00b_list_len(*got);
        if (got_len != t->matches_len) {
            fprintf(stderr,
                    "[case %zu] match-count mismatch: pattern=%s got=%zu want=%zu\n",
                    i, t->pattern, got_len, t->matches_len);
        }
        assert(got_len == t->matches_len);

        for (size_t j = 0; j < got_len; ++j) {
            n00b_regex_match_t m = n00b_list_get(*got, j);
            if (m.start != t->matches[j].start ||
                m.end   != t->matches[j].end) {
                fprintf(stderr,
                        "[case %zu] match %zu mismatch: pattern=%s "
                        "got=[%lld,%lld) want=[%lld,%lld)\n",
                        i, j, t->pattern,
                        (long long)m.start, (long long)m.end,
                        (long long)t->matches[j].start,
                        (long long)t->matches[j].end);
            }
            assert(m.start == t->matches[j].start);
            assert(m.end   == t->matches[j].end);
        }
    }

    printf("  [PASS] accel_skip_lazy (%zu cases)\n", n);
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex accel_skip tests...\n");
    test_accel_skip_lazy();
    printf("All regex accel_skip tests passed.\n");

    n00b_shutdown();
    return 0;
}
