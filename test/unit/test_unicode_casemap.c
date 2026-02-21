#include "test_unicode_helpers.h"
#include "unicode/casemap.h"
#include "unicode/encoding.h"

TEST(test_simple_toupper)
{
    ASSERT_EQ(n00b_unicode_toupper_cp('a'), 'A');
    ASSERT_EQ(n00b_unicode_toupper_cp('z'), 'Z');
    ASSERT_EQ(n00b_unicode_toupper_cp('A'), 'A');
    ASSERT_EQ(n00b_unicode_toupper_cp('0'), '0');
    ASSERT_EQ(n00b_unicode_toupper_cp(0x00E9), 0x00C9); // é → É
}

TEST(test_simple_tolower)
{
    ASSERT_EQ(n00b_unicode_tolower_cp('A'), 'a');
    ASSERT_EQ(n00b_unicode_tolower_cp('Z'), 'z');
    ASSERT_EQ(n00b_unicode_tolower_cp('a'), 'a');
    ASSERT_EQ(n00b_unicode_tolower_cp(0x00C9), 0x00E9); // É → é
}

TEST(test_simple_totitle)
{
    ASSERT_EQ(n00b_unicode_totitle_cp('a'), 'A');
    ASSERT_EQ(n00b_unicode_totitle_cp('A'), 'A');
}

TEST(test_casefold_simple)
{
    ASSERT_EQ(n00b_unicode_casefold_cp('A'), 'a');
    ASSERT_EQ(n00b_unicode_casefold_cp('a'), 'a');
    ASSERT_EQ(n00b_unicode_casefold_cp(0x00C9), 0x00E9);
}

TEST(test_full_uppercase)
{
    n00b_string_t upper = n00b_unicode_toupper(*r"hello", .allocator = nullptr);
    ASSERT_STR_EQ(upper.data, "HELLO");
}

TEST(test_full_lowercase)
{
    n00b_string_t lower = n00b_unicode_tolower(*r"HELLO", .allocator = nullptr);
    ASSERT_STR_EQ(lower.data, "hello");
}

TEST(test_full_casefold)
{
    n00b_string_t folded
        = n00b_unicode_casefold(*r"Hello World", .allocator = nullptr);
    ASSERT_STR_EQ(folded.data, "hello world");
}

TEST(test_casecmp)
{
    ASSERT_EQ(n00b_unicode_casecmp(*r"Hello", *r"hello"), 0);
    ASSERT(n00b_unicode_casecmp(*r"apple", *r"BANANA") < 0);
}

TEST(test_sharp_s_uppercase)
{
    // ß (U+00DF) → "SS" when uppercased
    n00b_string_t upper = n00b_unicode_toupper(*r"\xC3\x9F", .allocator = nullptr);
    ASSERT_STR_EQ(upper.data, "SS");
}

static void
run_tests(void)
{
    RUN_TEST(test_simple_toupper);
    RUN_TEST(test_simple_tolower);
    RUN_TEST(test_simple_totitle);
    RUN_TEST(test_casefold_simple);
    RUN_TEST(test_full_uppercase);
    RUN_TEST(test_full_lowercase);
    RUN_TEST(test_full_casefold);
    RUN_TEST(test_casecmp);
    RUN_TEST(test_sharp_s_uppercase);
}

TEST_MAIN()
