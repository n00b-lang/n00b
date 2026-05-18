#include "test_unicode_helpers.h"
#include "text/strings/format.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_plain_text)
{
    n00b_string_t *s = r"hello world";
    n00b_string_t *r = n00b_format(s);
    ASSERT_EQ(r->u8_bytes, 11);
    ASSERT_STR_EQ(r->data, "hello world");
    ASSERT(!n00b_option_is_set(n00b_str_get_style_info(r)));
}

TEST(test_bold_style)
{
    n00b_string_t *s = n00b_string_from_raw("[|b|]hello[|/b|]", 16);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "hello");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->bold, N00B_TRI_YES);
}

TEST(test_nested_styles)
{
    n00b_string_t *s = n00b_string_from_raw("[|b|]bold [|i|]both[|/i|][|/b|]", 31);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "bold both");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    // Should have style records.
    ASSERT(info->num_styles >= 1);
}

TEST(test_named_style)
{
    n00b_string_t *s = n00b_string_from_raw("[|em|]italic[|/em|]", 19);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "italic");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->italic, N00B_TRI_YES);
}

TEST(test_role_tag)
{
    n00b_string_t *s = n00b_string_from_raw("[|@code|]mono[|/@code|]", 23);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "mono");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->font_hint, N00B_FONT_MONO);
}

TEST(test_subst_string)
{
    n00b_string_t *name = r"world";
    n00b_string_t *desc = n00b_string_from_raw("hello [|#|]!", 12);
    n00b_string_t *r = n00b_format(desc, name);
    ASSERT_STR_EQ(r->data, "hello world!");
}

TEST(test_subst_int_with_spec)
{
    n00b_string_t *desc = n00b_string_from_raw("[|#:,d|]", 8);
    n00b_string_t *r = n00b_format(desc, (void *)(int64_t)1234567);
    ASSERT_STR_EQ(r->data, "1,234,567");
}

TEST(test_subst_float_with_spec)
{
    n00b_string_t *desc = n00b_string_from_raw("[|#:.2f|]", 9);
    double val         = 3.14159;
    n00b_string_t *r = n00b_format(desc, &val);
    ASSERT_STR_EQ(r->data, "3.14");
}

TEST(test_guillemet_bold)
{
    n00b_string_t *s = n00b_string_from_raw("\xC2\xAB" "b" "\xC2\xBB" "text" "\xC2\xAB" "/b" "\xC2\xBB", 15);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "text");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    ASSERT(info->num_styles >= 1);
    ASSERT_EQ(info->styles[0].info->bold, N00B_TRI_YES);
}

TEST(test_reset_clears_styles)
{
    n00b_string_t *s = n00b_string_from_raw("[|b|]bold[|/|]plain", 19);
    n00b_string_t *r = n00b_format(s);
    ASSERT_STR_EQ(r->data, "boldplain");

    auto                      info_opt = n00b_str_get_style_info(r);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info     = n00b_option_get(info_opt);
    // "bold" part has a style record that ends before "plain".
    ASSERT(info->num_styles >= 1);
    ASSERT(n00b_option_is_set(info->styles[0].end));
    // The end should be at byte 4 ("bold" is 4 bytes).
    ASSERT_EQ(n00b_option_get(info->styles[0].end), 4);
}

TEST(test_cformat_convenience)
{
    n00b_string_t *name = r"test";
    n00b_string_t *r = n00b_cformat("hello [|#|]", name);
    ASSERT_STR_EQ(r->data, "hello test");
}

// Regression: the C-string trap.  Passing a raw `const char *` to a
// no-format-spec «#» substitution used to make the format engine
// interpret the C string bytes as an n00b_string_t header, read a
// garbage u8_bytes field, and infinite-loop outbuf_append's sizing
// loop.  See MEMORY.md → feedback_printf_cstring_trap.md.
//
// We exercise the post-fix path by constructing a fake n00b_string_t
// with a deliberately oversized u8_bytes — the exact shape the trap
// produces — and asserting the format engine emits the diagnostic
// placeholder instead of hanging or reading past the buffer.
TEST(test_format_subst_oversized_u8bytes_fallback)
{
    struct {
        char  *data;
        size_t u8_bytes;
        size_t codepoints;
        void  *styling;
    } fake = {
        .data       = "x",
        .u8_bytes   = (size_t)0xffffffffULL,
        .codepoints = 1,
        .styling    = NULL,
    };

    n00b_string_t *r = n00b_cformat("[|#|]", (n00b_string_t *)&fake);
    ASSERT_STR_EQ(r->data, "<bad-string-arg>");
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
    RUN_TEST(test_format_subst_oversized_u8bytes_fallback);
}

TEST_MAIN()
