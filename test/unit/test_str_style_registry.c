#include "test_unicode_helpers.h"
#include "strings/style_registry.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_default_styles_exist)
{
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("em")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("em1")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("em2")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("em3")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("h1")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("h2")));
    ASSERT(n00b_option_is_set(n00b_str_style_lookup("h3")));
}

TEST(test_default_roles_exist)
{
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@code")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@mono")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@heading")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@body")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@error")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@success")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@muted")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@link")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@label")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@button")));
    ASSERT(n00b_option_is_set(n00b_str_role_lookup("@input")));
}

TEST(test_em_is_italic)
{
    auto em_opt = n00b_str_style_lookup("em");
    ASSERT(n00b_option_is_set(em_opt));
    ASSERT_EQ(n00b_option_get(em_opt)->italic, N00B_TRI_YES);
}

TEST(test_code_is_mono)
{
    auto code_opt = n00b_str_role_lookup("@code");
    ASSERT(n00b_option_is_set(code_opt));
    ASSERT_EQ(n00b_option_get(code_opt)->font_hint, N00B_FONT_MONO);
}

TEST(test_absent_name_returns_none)
{
    ASSERT(!n00b_option_is_set(n00b_str_style_lookup("nonexistent")));
    ASSERT(!n00b_option_is_set(n00b_str_role_lookup("@nonexistent")));
}

TEST(test_custom_style_registration)
{
    n00b_text_style_t *custom = n00b_str_style_new();
    custom->strikethrough     = N00B_TRI_YES;
    n00b_str_style_register("my_custom", custom);

    auto found_opt = n00b_str_style_lookup("my_custom");
    ASSERT(n00b_option_is_set(found_opt));
    ASSERT_EQ(n00b_option_get(found_opt)->strikethrough, N00B_TRI_YES);

    n00b_free(custom);
}

TEST(test_h1_is_bold_upper)
{
    auto h1_opt = n00b_str_style_lookup("h1");
    ASSERT(n00b_option_is_set(h1_opt));
    n00b_text_style_t *s = n00b_option_get(h1_opt);
    ASSERT_EQ(s->bold, N00B_TRI_YES);
    ASSERT_EQ(s->text_case, N00B_TEXT_CASE_UPPER);
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_default_styles_exist);
    RUN_TEST(test_default_roles_exist);
    RUN_TEST(test_em_is_italic);
    RUN_TEST(test_code_is_mono);
    RUN_TEST(test_absent_name_returns_none);
    RUN_TEST(test_custom_style_registration);
    RUN_TEST(test_h1_is_bold_upper);
}

TEST_MAIN()
