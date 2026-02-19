#include "test_unicode_helpers.h"
#include "strings/md_lines.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_single_paragraph)
{
    n00b_string_t src  = STR("hello world");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 1);

    n00b_string_t line = n00b_array_get(lines, 0);
    ASSERT_STR_EQ(line.data, "hello world");

    n00b_array_free(lines);
}

TEST(test_two_paragraphs)
{
    n00b_string_t src  = STR("first\n\nsecond");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 2);

    n00b_string_t l0 = n00b_array_get(lines, 0);
    n00b_string_t l1 = n00b_array_get(lines, 1);
    ASSERT_STR_EQ(l0.data, "first");
    ASSERT_STR_EQ(l1.data, "second");

    n00b_array_free(lines);
}

TEST(test_heading)
{
    n00b_string_t src  = STR("# Title");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 1);

    n00b_string_t line = n00b_array_get(lines, 0);
    ASSERT_STR_EQ(line.data, "Title");

    n00b_string_style_info_t *info = n00b_str_get_style_info(line);
    ASSERT(info != nullptr);
    bool found_bold = false;
    for (int64_t i = 0; i < info->num_styles; i++) {
        if (info->styles[i].info->bold == N00B_TRI_YES) {
            found_bold = true;
        }
    }
    ASSERT(found_bold);

    n00b_array_free(lines);
}

TEST(test_unordered_list)
{
    n00b_string_t src  = STR("- alpha\n- beta\n- gamma");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 3);

    n00b_string_t l0 = n00b_array_get(lines, 0);
    n00b_string_t l1 = n00b_array_get(lines, 1);
    n00b_string_t l2 = n00b_array_get(lines, 2);
    ASSERT_STR_EQ(l0.data, "- alpha");
    ASSERT_STR_EQ(l1.data, "- beta");
    ASSERT_STR_EQ(l2.data, "- gamma");

    n00b_array_free(lines);
}

TEST(test_code_block)
{
    n00b_string_t src  = STR("```\nint x;\nint y;\n```");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    // Should have at least 2 code lines.
    ASSERT(n00b_array_len(lines) >= 2);

    // Each line should have mono style.
    bool all_mono = true;
    for (size_t i = 0; i < n00b_array_len(lines); i++) {
        n00b_string_t line = n00b_array_get(lines, i);
        n00b_string_style_info_t *info = n00b_str_get_style_info(line);
        if (!info) {
            all_mono = false;
            break;
        }
        bool has_mono = false;
        for (int64_t j = 0; j < info->num_styles; j++) {
            if (info->styles[j].info->font_hint == N00B_FONT_MONO) {
                has_mono = true;
            }
        }
        if (!has_mono) {
            all_mono = false;
        }
    }
    ASSERT(all_mono);

    // Check content of first two lines.
    n00b_string_t l0 = n00b_array_get(lines, 0);
    n00b_string_t l1 = n00b_array_get(lines, 1);
    ASSERT_STR_EQ(l0.data, "int x;");
    ASSERT_STR_EQ(l1.data, "int y;");

    n00b_array_free(lines);
}

TEST(test_hr)
{
    n00b_string_t src  = STR("above\n\n---\n\nbelow");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 3);

    n00b_string_t l0 = n00b_array_get(lines, 0);
    n00b_string_t l1 = n00b_array_get(lines, 1);
    n00b_string_t l2 = n00b_array_get(lines, 2);
    ASSERT_STR_EQ(l0.data, "above");
    ASSERT_STR_EQ(l1.data, "---");
    ASSERT_STR_EQ(l2.data, "below");

    n00b_array_free(lines);
}

TEST(test_inline_styles_in_paragraph)
{
    n00b_string_t src  = STR("normal **bold** *italic*");
    auto          tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);
    ASSERT_EQ(n00b_array_len(lines), 1);

    n00b_string_t line = n00b_array_get(lines, 0);
    ASSERT_STR_EQ(line.data, "normal bold italic");

    n00b_string_style_info_t *info = n00b_str_get_style_info(line);
    ASSERT(info != nullptr);

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

    n00b_array_free(lines);
}

TEST(test_mixed_doc)
{
    // Heading, paragraph, list, HR, code block.
    n00b_string_t src = STR(
        "# Title\n\nSome text.\n\n- one\n- two\n\n---\n\n```\ncode\n```");
    auto tree = n00b_parse_markdown(src);

    n00b_array_t(n00b_string_t) lines = n00b_str_md_to_lines(tree);

    // Should have: Title, Some text., - one, - two, ---, code
    ASSERT(n00b_array_len(lines) >= 6);

    n00b_string_t l0 = n00b_array_get(lines, 0);
    ASSERT_STR_EQ(l0.data, "Title");

    n00b_string_t l1 = n00b_array_get(lines, 1);
    ASSERT_STR_EQ(l1.data, "Some text.");

    n00b_array_free(lines);
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_single_paragraph);
    RUN_TEST(test_two_paragraphs);
    RUN_TEST(test_heading);
    RUN_TEST(test_unordered_list);
    RUN_TEST(test_code_block);
    RUN_TEST(test_hr);
    RUN_TEST(test_inline_styles_in_paragraph);
    RUN_TEST(test_mixed_doc);
}

TEST_MAIN()
