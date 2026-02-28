#include "test_unicode_helpers.h"
#include "text/unicode/security.h"
#include "text/unicode/encoding.h"

TEST(test_skeleton)
{
    n00b_string_t skel = n00b_unicode_skeleton(*r"hello", .allocator = nullptr);
    ASSERT(skel.data != nullptr);
    ASSERT(skel.u8_bytes > 0);
}

TEST(test_confusable_same)
{
    ASSERT(n00b_unicode_is_confusable(*r"hello", *r"hello"));
}

TEST(test_script_restriction_ascii)
{
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"hello");
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_ASCII_ONLY);
}

TEST(test_script_restriction_single)
{
    // Japanese hiragana only
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"\xE3\x81\x82\xE3\x81\x84"); // あい
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT);
}

TEST(test_inherited_ignored)
{
    // Latin letter 'a' + COMBINING ACUTE ACCENT (U+0301, Inherited)
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"a\xCC\x81");
    // Inherited doesn't count as a separate script
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT);
}

TEST(test_mixed_latin_cyrillic)
{
    // "paypal" with Cyrillic а (U+0430) instead of Latin a
    // "p" + U+0430 + "ypal"
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"p\xD0\xB0ypal");
    // Latin + Cyrillic = MODERATELY_RESTRICTIVE (Latin + one recommended)
    ASSERT(level > N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT);
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_MODERATELY_RESTRICTIVE);
}

TEST(test_japanese_mixed)
{
    // Han (世) + Hiragana (あ) + Katakana (ア)
    n00b_unicode_restriction_level_t level = n00b_unicode_script_restriction(
        *r"\xE4\xB8\x96\xE3\x81\x82\xE3\x82\xA2");
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_HIGHLY_RESTRICTIVE);
}

TEST(test_korean_mixed)
{
    // Hangul (한) + Han (字)
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"\xED\x95\x9C\xE5\xAD\x97");
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_HIGHLY_RESTRICTIVE);
}

TEST(test_latin_plus_one)
{
    // Latin 'a' + Greek α (U+03B1)
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"a\xCE\xB1");
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_MODERATELY_RESTRICTIVE);
}

TEST(test_has_mixed_scripts)
{
    // Latin + Cyrillic → mixed
    ASSERT(n00b_unicode_has_mixed_scripts(*r"p\xD0\xB0ypal"));

    // Pure Latin → not mixed
    ASSERT(!n00b_unicode_has_mixed_scripts(*r"hello"));
}

TEST(test_script_extensions_middle_dot)
{
    // U+00B7 MIDDLE DOT has Script_Extensions: many scripts including Latin
    // Latin 'l' + U+00B7 + Latin 'l' should be SINGLE_SCRIPT
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"l\xC2\xB7l");
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_SINGLE_SCRIPT);
}

TEST(test_common_digits)
{
    // Digits (Common) + Latin → SINGLE_SCRIPT
    n00b_unicode_restriction_level_t level
        = n00b_unicode_script_restriction(*r"abc123");
    // Digits are Common, so only Latin is non-common → SINGLE_SCRIPT
    // But ASCII only since all < 0x80
    ASSERT_EQ(level, N00B_UNICODE_RESTRICTION_ASCII_ONLY);
}

static void
run_tests(void)
{
    RUN_TEST(test_skeleton);
    RUN_TEST(test_confusable_same);
    RUN_TEST(test_script_restriction_ascii);
    RUN_TEST(test_script_restriction_single);
    RUN_TEST(test_inherited_ignored);
    RUN_TEST(test_mixed_latin_cyrillic);
    RUN_TEST(test_japanese_mixed);
    RUN_TEST(test_korean_mixed);
    RUN_TEST(test_latin_plus_one);
    RUN_TEST(test_has_mixed_scripts);
    RUN_TEST(test_script_extensions_middle_dot);
    RUN_TEST(test_common_digits);
}

TEST_MAIN()
