#include "test_unicode_helpers.h"
#include "strings/style_registry.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_default_styles_exist)
{
    ASSERT(n00b_str_style_lookup("em") != nullptr);
    ASSERT(n00b_str_style_lookup("em1") != nullptr);
    ASSERT(n00b_str_style_lookup("em2") != nullptr);
    ASSERT(n00b_str_style_lookup("em3") != nullptr);
    ASSERT(n00b_str_style_lookup("h1") != nullptr);
    ASSERT(n00b_str_style_lookup("h2") != nullptr);
    ASSERT(n00b_str_style_lookup("h3") != nullptr);
}

TEST(test_default_roles_exist)
{
    ASSERT(n00b_str_role_lookup("@code") != nullptr);
    ASSERT(n00b_str_role_lookup("@mono") != nullptr);
    ASSERT(n00b_str_role_lookup("@heading") != nullptr);
    ASSERT(n00b_str_role_lookup("@body") != nullptr);
    ASSERT(n00b_str_role_lookup("@error") != nullptr);
    ASSERT(n00b_str_role_lookup("@success") != nullptr);
    ASSERT(n00b_str_role_lookup("@muted") != nullptr);
    ASSERT(n00b_str_role_lookup("@link") != nullptr);
    ASSERT(n00b_str_role_lookup("@label") != nullptr);
    ASSERT(n00b_str_role_lookup("@button") != nullptr);
    ASSERT(n00b_str_role_lookup("@input") != nullptr);
}

TEST(test_em_is_italic)
{
    n00b_text_style_t *s = n00b_str_style_lookup("em");
    ASSERT(s != nullptr);
    ASSERT_EQ(s->italic, N00B_TRI_YES);
}

TEST(test_code_is_mono)
{
    n00b_text_style_t *s = n00b_str_role_lookup("@code");
    ASSERT(s != nullptr);
    ASSERT_EQ(s->font_hint, N00B_FONT_MONO);
}

TEST(test_absent_name_returns_null)
{
    ASSERT(n00b_str_style_lookup("nonexistent") == nullptr);
    ASSERT(n00b_str_role_lookup("@nonexistent") == nullptr);
}

TEST(test_custom_style_registration)
{
    n00b_text_style_t *custom = n00b_str_style_new();
    custom->strikethrough     = N00B_TRI_YES;
    n00b_str_style_register("my_custom", custom);

    n00b_text_style_t *found = n00b_str_style_lookup("my_custom");
    ASSERT(found != nullptr);
    ASSERT_EQ(found->strikethrough, N00B_TRI_YES);

    n00b_free(custom);
}

TEST(test_h1_is_bold_upper)
{
    n00b_text_style_t *s = n00b_str_style_lookup("h1");
    ASSERT(s != nullptr);
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
    RUN_TEST(test_absent_name_returns_null);
    RUN_TEST(test_custom_style_registration);
    RUN_TEST(test_h1_is_bold_upper);
}

TEST_MAIN()
