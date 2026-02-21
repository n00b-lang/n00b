#include "test_unicode_helpers.h"
#include "unicode/emoji.h"
#include "unicode/encoding.h"

TEST(test_is_emoji)
{
    ASSERT(n00b_unicode_is_emoji(0x1F600));  // 😀
    ASSERT(n00b_unicode_is_emoji(0x2764));   // ❤
    ASSERT(!n00b_unicode_is_emoji('A'));
    ASSERT(!n00b_unicode_is_emoji(' '));
}

TEST(test_is_emoji_presentation)
{
    ASSERT(n00b_unicode_is_emoji_presentation(0x1F600));  // 😀
    ASSERT(!n00b_unicode_is_emoji_presentation('A'));
}

TEST(test_emoji_scan_basic)
{
    n00b_string_t s = *r"\xF0\x9F\x98\x80"; // 😀
    n00b_unicode_emoji_scan_result_t r = n00b_unicode_emoji_scan(s, 0);
    ASSERT(r.type != N00B_UNICODE_EMOJI_NONE);
    ASSERT_EQ(r.seq_bytes, 4);
}

TEST(test_emoji_scan_flag)
{
    // 🇺🇸 = U+1F1FA U+1F1F8
    n00b_string_t s = *r"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    n00b_unicode_emoji_scan_result_t r = n00b_unicode_emoji_scan(s, 0);
    ASSERT_EQ(r.type, N00B_UNICODE_EMOJI_FLAG);
    ASSERT_EQ(r.seq_bytes, 8);
}

TEST(test_emoji_scan_not_emoji)
{
    n00b_string_t s = *r"Hello";
    n00b_unicode_emoji_scan_result_t r = n00b_unicode_emoji_scan(s, 0);
    ASSERT_EQ(r.type, N00B_UNICODE_EMOJI_NONE);
    ASSERT_EQ(r.seq_bytes, 0);
}

static void run_tests(void)
{
    RUN_TEST(test_is_emoji);
    RUN_TEST(test_is_emoji_presentation);
    RUN_TEST(test_emoji_scan_basic);
    RUN_TEST(test_emoji_scan_flag);
    RUN_TEST(test_emoji_scan_not_emoji);
}

TEST_MAIN()
