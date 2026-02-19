#include "test_unicode_helpers.h"
#include "strings/format.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_plain_text)
{
    n00b_string_t s = STR("hello world");
    n00b_string_t r = n00b_format(s);
    ASSERT_EQ(r.u8_bytes, 11);
    ASSERT_STR_EQ(r.data, "hello world");
    ASSERT(n00b_str_get_style_info(r) == nullptr);
}

TEST(test_bold_style)
{
    n00b_string_t s = STR("[|b|]hello[|/b|]");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "hello");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->bold, N00B_TRI_YES);
}

TEST(test_nested_styles)
{
    n00b_string_t s = STR("[|b|]bold [|i|]both[|/i|][|/b|]");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "bold both");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    // Should have style records.
    ASSERT(info->num_styles >= 1);
}

TEST(test_named_style)
{
    n00b_string_t s = STR("[|em|]italic[|/em|]");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "italic");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->italic, N00B_TRI_YES);
}

TEST(test_role_tag)
{
    n00b_string_t s = STR("[|@code|]mono[|/@code|]");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "mono");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->font_hint, N00B_FONT_MONO);
}

TEST(test_subst_string)
{
    n00b_string_t name = STR("world");
    n00b_string_t desc = STR("hello [|#|]!");
    n00b_string_t r    = n00b_format(desc, &name);
    ASSERT_STR_EQ(r.data, "hello world!");
}

TEST(test_subst_int_with_spec)
{
    n00b_string_t desc = STR("[|#:,d|]");
    n00b_string_t r    = n00b_format(desc, (void *)(int64_t)1234567);
    ASSERT_STR_EQ(r.data, "1,234,567");
}

TEST(test_subst_float_with_spec)
{
    n00b_string_t desc = STR("[|#:.2f|]");
    double val         = 3.14159;
    n00b_string_t r    = n00b_format(desc, &val);
    ASSERT_STR_EQ(r.data, "3.14");
}

TEST(test_guillemet_bold)
{
    n00b_string_t s = STR("\xC2\xAB" "b" "\xC2\xBB"
                           "text"
                           "\xC2\xAB" "/b" "\xC2\xBB");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "text");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->bold, N00B_TRI_YES);
}

TEST(test_reset_clears_styles)
{
    n00b_string_t s = STR("[|b|]bold[|/|]plain");
    n00b_string_t r = n00b_format(s);
    ASSERT_STR_EQ(r.data, "boldplain");

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    // "bold" part has a style record that ends before "plain".
    ASSERT(info->num_styles >= 1);
    ASSERT(n00b_option_is_set(info->styles[0].end));
    // The end should be at byte 4 ("bold" is 4 bytes).
    ASSERT_EQ(n00b_option_get(info->styles[0].end), 4);
}

TEST(test_cformat_convenience)
{
    n00b_string_t name = STR("test");
    n00b_string_t r    = n00b_cformat("hello [|#|]", &name);
    ASSERT_STR_EQ(r.data, "hello test");
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_plain_text);
    RUN_TEST(test_bold_style);
    RUN_TEST(test_nested_styles);
    RUN_TEST(test_named_style);
    RUN_TEST(test_role_tag);
    RUN_TEST(test_subst_string);
    RUN_TEST(test_subst_int_with_spec);
    RUN_TEST(test_subst_float_with_spec);
    RUN_TEST(test_guillemet_bold);
    RUN_TEST(test_reset_clears_styles);
    RUN_TEST(test_cformat_convenience);
}

TEST_MAIN()
