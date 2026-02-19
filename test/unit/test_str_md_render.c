#include "test_unicode_helpers.h"
#include "strings/md_render.h"
#include "strings/markdown.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_plain_text)
{
    n00b_string_t src  = STR("hello world");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "hello world");
    // Plain text should have no style records (md4c wraps in <p> but
    // paragraphs don't push a style).
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    if (info) {
        // Any records that exist should be empty styles.
        for (int64_t i = 0; i < info->num_styles; i++) {
            ASSERT(n00b_str_style_is_empty(info->styles[i].info));
        }
    }
}

TEST(test_bold)
{
    n00b_string_t src  = STR("**bold**");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "bold");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    // Find the bold record.
    bool found_bold = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->bold == N00B_TRI_YES) {
            found_bold = true;
            break;
        }
    }
    ASSERT(found_bold);
}

TEST(test_italic)
{
    n00b_string_t src  = STR("*italic*");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "italic");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    bool found_italic = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->italic == N00B_TRI_YES) {
            found_italic = true;
            break;
        }
    }
    ASSERT(found_italic);
}

TEST(test_inline_code)
{
    n00b_string_t src  = STR("`code`");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "code");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    bool found_mono = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->font_hint == N00B_FONT_MONO) {
            found_mono = true;
            break;
        }
    }
    ASSERT(found_mono);
}

TEST(test_strikethrough)
{
    n00b_string_t src  = STR("~~strike~~");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "strike");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    bool found_strike = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->strikethrough == N00B_TRI_YES) {
            found_strike = true;
            break;
        }
    }
    ASSERT(found_strike);
}

TEST(test_nested_bold_italic)
{
    n00b_string_t src  = STR("**bold *both* bold**");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "bold both bold");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    // Should have at least one record with both bold and italic.
    bool found_both = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->bold == N00B_TRI_YES
            && info->styles[i].info->italic == N00B_TRI_YES) {
            found_both = true;
            break;
        }
    }
    ASSERT(found_both);
}

TEST(test_heading)
{
    n00b_string_t src  = STR("# Heading");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "Heading");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    bool found_bold = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->bold == N00B_TRI_YES) {
            found_bold = true;
            break;
        }
    }
    ASSERT(found_bold);
}

TEST(test_code_block)
{
    n00b_string_t src  = STR("```\nint x;\n```");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    // md4c may include a trailing newline; check the code content is present.
    ASSERT(strstr(r.data, "int x;") != nullptr);

    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);
    ASSERT(info->num_styles >= 1);

    bool found_mono = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->font_hint == N00B_FONT_MONO) {
            found_mono = true;
            break;
        }
    }
    ASSERT(found_mono);
}

TEST(test_mixed_spans)
{
    n00b_string_t src  = STR("normal **bold** *italic*");
    auto          tree = n00b_parse_markdown(src);
    n00b_string_t r    = n00b_str_md_render(tree);

    ASSERT_STR_EQ(r.data, "normal bold italic");
    n00b_string_style_info_t *info = n00b_str_get_style_info(r);
    ASSERT(info != nullptr);

    // Should have separate bold and italic records.
    bool found_bold   = false;
    bool found_italic = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->bold == N00B_TRI_YES) {
            found_bold = true;
        }
        if (info->styles[i].info->italic == N00B_TRI_YES) {
            found_italic = true;
        }
    }
    ASSERT(found_bold);
    ASSERT(found_italic);
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_plain_text);
    RUN_TEST(test_bold);
    RUN_TEST(test_italic);
    RUN_TEST(test_inline_code);
    RUN_TEST(test_strikethrough);
    RUN_TEST(test_nested_bold_italic);
    RUN_TEST(test_heading);
    RUN_TEST(test_code_block);
    RUN_TEST(test_mixed_spans);
}

TEST_MAIN()
