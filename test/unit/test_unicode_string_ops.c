#include "test_unicode_helpers.h"
#include "unicode/string_ops.h"
#include "unicode/encoding.h"

// Helper: create a n00b_string_t from a literal
#define S(lit) STR(lit)

TEST(test_cat)
{
    n00b_string_t c = n00b_unicode_str_cat(S("Hello"), S(" World"), .allocator = NULL);
    ASSERT_STR_EQ(c.data, "Hello World");
    ASSERT_EQ(c.codepoints, 11);
}

TEST(test_cat_many)
{
    n00b_string_t parts[] = { S("a"), S("b"), S("c") };
    n00b_string_t r = n00b_unicode_str_cat_many(parts, 3, .allocator = NULL);
    ASSERT_STR_EQ(r.data, "abc");
    ASSERT_EQ(r.codepoints, 3);
}

TEST(test_join)
{
    n00b_string_t parts[] = { S("one"), S("two"), S("three") };
    n00b_string_t r = n00b_unicode_str_join(S(", "), parts, 3, .allocator = NULL);
    ASSERT_STR_EQ(r.data, "one, two, three");
}

TEST(test_join_empty)
{
    n00b_string_t r = n00b_unicode_str_join(S(","), NULL, 0, .allocator = NULL);
    ASSERT_STR_EQ(r.data, "");
    ASSERT_EQ(r.u8_bytes, 0);
}

TEST(test_slice_basic)
{
    n00b_string_t sl = n00b_unicode_str_slice(S("Hello"), 0, 3, .allocator = NULL);
    ASSERT_EQ(sl.u8_bytes, 3);
    ASSERT(memcmp(sl.data, "Hel", 3) == 0);
}

TEST(test_slice_negative)
{
    n00b_string_t sl = n00b_unicode_str_slice(S("Hello"), -2, 5, .allocator = NULL);
    ASSERT_EQ(sl.u8_bytes, 2);
    ASSERT(memcmp(sl.data, "lo", 2) == 0);
}

TEST(test_slice_multibyte)
{
    // "café" — é is 2 bytes, but 1 grapheme
    n00b_string_t cafe = S("caf\xC3\xA9");
    n00b_string_t sl = n00b_unicode_str_slice(cafe, 0, 3, .allocator = NULL);
    ASSERT_EQ(sl.u8_bytes, 3);
    ASSERT(memcmp(sl.data, "caf", 3) == 0);

    // Full string
    n00b_string_t sl2 = n00b_unicode_str_slice(cafe, 0, 4, .allocator = NULL);
    ASSERT_EQ(sl2.u8_bytes, 5); // "café" = 5 bytes
}

TEST(test_grapheme_at)
{
    n00b_string_t g = n00b_unicode_str_grapheme_at(S("abc"), 1, .allocator = NULL);
    ASSERT_EQ(g.u8_bytes, 1);
    ASSERT(g.data[0] == 'b');
}

TEST(test_slice_bytes)
{
    n00b_string_t sl = n00b_unicode_str_slice_bytes(S("Hello World"), 6, 11, .allocator = NULL);
    ASSERT_EQ(sl.u8_bytes, 5);
    ASSERT(memcmp(sl.data, "World", 5) == 0);
}

TEST(test_find)
{
    auto r1 = n00b_unicode_str_find(S("Hello World"), S("World"));
    ASSERT(n00b_option_is_set(r1));
    ASSERT_EQ(n00b_option_get(r1), 6);

    auto r2 = n00b_unicode_str_find(S("Hello World"), S("xyz"));
    ASSERT(!n00b_option_is_set(r2));
}

TEST(test_rfind)
{
    auto r = n00b_unicode_str_rfind(S("abcabc"), S("abc"));
    ASSERT(n00b_option_is_set(r));
    ASSERT_EQ(n00b_option_get(r), 3);
}

TEST(test_contains)
{
    ASSERT(n00b_unicode_str_contains(S("Hello World"), S("World")));
    ASSERT(!n00b_unicode_str_contains(S("Hello World"), S("xyz")));
}

TEST(test_starts_ends_with)
{
    ASSERT(n00b_unicode_str_starts_with(S("Hello World"), S("Hello")));
    ASSERT(!n00b_unicode_str_starts_with(S("Hello World"), S("World")));
    ASSERT(n00b_unicode_str_ends_with(S("Hello World"), S("World")));
    ASSERT(!n00b_unicode_str_ends_with(S("Hello World"), S("Hello")));
}

TEST(test_replace)
{
    n00b_string_t r = n00b_unicode_str_replace(S("Hello World"), S("World"), S("Earth"),
                                       .allocator = NULL);
    ASSERT_STR_EQ(r.data, "Hello Earth");
}

TEST(test_replace_all)
{
    n00b_string_t r = n00b_unicode_str_replace_all(S("aXbXc"), S("X"), S("--"),
                                           .allocator = NULL);
    ASSERT_STR_EQ(r.data, "a--b--c");
}

TEST(test_replace_all_shorter)
{
    n00b_string_t r = n00b_unicode_str_replace_all(S("aXXbXXc"), S("XX"), S("Y"),
                                           .allocator = NULL);
    ASSERT_STR_EQ(r.data, "aYbYc");
}

TEST(test_split)
{
    uint32_t count;
    n00b_string_t *parts = n00b_unicode_str_split(S("one,two,three"), S(","), &count,
                                          .allocator = NULL);
    ASSERT_EQ(count, 3);
    ASSERT(memcmp(parts[0].data, "one", 3) == 0);
    ASSERT(memcmp(parts[1].data, "two", 3) == 0);
    ASSERT(memcmp(parts[2].data, "three", 5) == 0);
}

TEST(test_split_lines)
{
    uint32_t count;
    n00b_string_t *lines = n00b_unicode_str_split_lines(S("a\nb\r\nc\rd"), &count,
                                                .allocator = NULL);
    ASSERT_EQ(count, 4);
    ASSERT(memcmp(lines[0].data, "a", 1) == 0);
    ASSERT(memcmp(lines[1].data, "b", 1) == 0);
    ASSERT(memcmp(lines[2].data, "c", 1) == 0);
    ASSERT(memcmp(lines[3].data, "d", 1) == 0);
}

TEST(test_trim)
{
    n00b_string_t t = n00b_unicode_str_trim(S("  hello  "), .allocator = NULL);
    ASSERT_EQ(t.u8_bytes, 5);
    ASSERT(memcmp(t.data, "hello", 5) == 0);
}

TEST(test_trim_unicode_whitespace)
{
    // U+2003 EM SPACE (E2 80 83) + "hi" + U+00A0 NBSP (C2 A0)
    n00b_string_t t = n00b_unicode_str_trim(S("\xE2\x80\x83hi\xC2\xA0"), .allocator = NULL);
    ASSERT_EQ(t.u8_bytes, 2);
    ASSERT(memcmp(t.data, "hi", 2) == 0);
}

TEST(test_cmp)
{
    ASSERT(n00b_unicode_str_cmp(S("abc"), S("abd")) < 0);
    ASSERT(n00b_unicode_str_cmp(S("abd"), S("abc")) > 0);
    ASSERT_EQ(n00b_unicode_str_cmp(S("abc"), S("abc")), 0);
}

TEST(test_eq)
{
    ASSERT(n00b_unicode_str_eq(S("hello"), S("hello")));
    ASSERT(!n00b_unicode_str_eq(S("hello"), S("world")));
}

TEST(test_eq_nfc)
{
    // Decomposed é (e + combining acute) vs precomposed é
    ASSERT(!n00b_unicode_str_eq(S("e\xCC\x81"), S("\xC3\xA9")));
    ASSERT(n00b_unicode_str_eq_nfc(S("e\xCC\x81"), S("\xC3\xA9")));
}

TEST(test_eq_casefold)
{
    ASSERT(!n00b_unicode_str_eq(S("Hello"), S("hello")));
    ASSERT(n00b_unicode_str_eq_casefold(S("Hello"), S("hello")));
}

TEST(test_pad_right)
{
    n00b_string_t r = n00b_unicode_str_pad_right(S("hi"), 5, .allocator = NULL, .fill = ' ');
    ASSERT_STR_EQ(r.data, "hi   ");
}

TEST(test_pad_left)
{
    n00b_string_t r = n00b_unicode_str_pad_left(S("42"), 5, .allocator = NULL, .fill = '0');
    ASSERT_STR_EQ(r.data, "00042");
}

TEST(test_center)
{
    n00b_string_t r = n00b_unicode_str_center(S("hi"), 6, .allocator = NULL, .fill = '-');
    ASSERT_STR_EQ(r.data, "--hi--");
}

TEST(test_truncate)
{
    n00b_string_t r = n00b_unicode_str_truncate(S("Hello World"), 8,
                                        .allocator = NULL, .ellipsis = "...");
    ASSERT_STR_EQ(r.data, "Hello...");
}

TEST(test_repeat)
{
    n00b_string_t r = n00b_unicode_str_repeat(S("ab"), 3, .allocator = NULL);
    ASSERT_STR_EQ(r.data, "ababab");
    ASSERT_EQ(r.codepoints, 6);
}

TEST(test_repeat_zero)
{
    n00b_string_t r = n00b_unicode_str_repeat(S("abc"), 0, .allocator = NULL);
    ASSERT_STR_EQ(r.data, "");
    ASSERT_EQ(r.u8_bytes, 0);
}

TEST(test_reverse_ascii)
{
    n00b_string_t r = n00b_unicode_str_reverse(S("abcde"), .allocator = NULL);
    ASSERT_STR_EQ(r.data, "edcba");
}

TEST(test_reverse_multibyte)
{
    // "café" reversed should be "éfac"
    n00b_string_t r = n00b_unicode_str_reverse(S("caf\xC3\xA9"), .allocator = NULL);
    ASSERT_STR_EQ(r.data, "\xC3\xA9""fac");
}

TEST(test_reverse_combining)
{
    // "e" + combining acute reversed should keep the accent attached
    // Input: "ae\xCC\x81" (a + é where é = e+combining)
    // Reversed graphemes: "é" + "a"
    n00b_string_t r = n00b_unicode_str_reverse(S("ae\xCC\x81"), .allocator = NULL);
    // Should be: e+combining_acute + a
    ASSERT_EQ(r.u8_bytes, 4);
    uint32_t pos = 0;
    int32_t cp1 = n00b_unicode_utf8_decode(r.data, r.u8_bytes, &pos);
    int32_t cp2 = n00b_unicode_utf8_decode(r.data, r.u8_bytes, &pos);
    int32_t cp3 = n00b_unicode_utf8_decode(r.data, r.u8_bytes, &pos);
    ASSERT_EQ(cp1, 'e');
    ASSERT_EQ(cp2, 0x0301); // combining acute
    ASSERT_EQ(cp3, 'a');
}

TEST(test_split_graphemes)
{
    uint32_t count;
    n00b_string_t *parts = n00b_unicode_str_split_graphemes(S("abc"), &count,
                                                    .allocator = NULL);
    ASSERT_EQ(count, 3);
    ASSERT(parts[0].data[0] == 'a');
    ASSERT(parts[1].data[0] == 'b');
    ASSERT(parts[2].data[0] == 'c');
}

static void run_tests(void)
{
    RUN_TEST(test_cat);
    RUN_TEST(test_cat_many);
    RUN_TEST(test_join);
    RUN_TEST(test_join_empty);
    RUN_TEST(test_slice_basic);
    RUN_TEST(test_slice_negative);
    RUN_TEST(test_slice_multibyte);
    RUN_TEST(test_grapheme_at);
    RUN_TEST(test_slice_bytes);
    RUN_TEST(test_find);
    RUN_TEST(test_rfind);
    RUN_TEST(test_contains);
    RUN_TEST(test_starts_ends_with);
    RUN_TEST(test_replace);
    RUN_TEST(test_replace_all);
    RUN_TEST(test_replace_all_shorter);
    RUN_TEST(test_split);
    RUN_TEST(test_split_lines);
    RUN_TEST(test_trim);
    RUN_TEST(test_trim_unicode_whitespace);
    RUN_TEST(test_cmp);
    RUN_TEST(test_eq);
    RUN_TEST(test_eq_nfc);
    RUN_TEST(test_eq_casefold);
    RUN_TEST(test_pad_right);
    RUN_TEST(test_pad_left);
    RUN_TEST(test_center);
    RUN_TEST(test_truncate);
    RUN_TEST(test_repeat);
    RUN_TEST(test_repeat_zero);
    RUN_TEST(test_reverse_ascii);
    RUN_TEST(test_reverse_multibyte);
    RUN_TEST(test_reverse_combining);
    RUN_TEST(test_split_graphemes);
}

TEST_MAIN()
