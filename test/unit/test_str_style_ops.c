#include "test_unicode_helpers.h"
#include "text/strings/style_ops.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_new_is_empty)
{
    n00b_text_style_t *s = n00b_str_style_new();
    ASSERT(s != nullptr);
    ASSERT(n00b_str_style_is_empty(s));
    ASSERT_EQ(s->bold, N00B_TRI_UNSPECIFIED);
    ASSERT_EQ(s->font_index, -1);
    ASSERT_EQ(s->fg_palette_ix, -1);
    ASSERT(!n00b_color_is_set(s->fg_rgb));
    n00b_free(s);
}

TEST(test_copy_is_equal)
{
    n00b_text_style_t *a = n00b_str_style_new();
    a->bold              = N00B_TRI_YES;
    a->fg_rgb            = n00b_color_make(0xFF0000);
    n00b_text_style_t *b = n00b_str_style_copy(a);
    ASSERT(n00b_str_style_eq(a, b));
    // Mutate copy -- original unchanged.
    b->italic = N00B_TRI_YES;
    ASSERT(!n00b_str_style_eq(a, b));
    n00b_free(a);
    n00b_free(b);
}

TEST(test_merge_inheritance)
{
    n00b_text_style_t *base = n00b_str_style_new();
    base->bold              = N00B_TRI_YES;
    base->italic            = N00B_TRI_NO;
    base->fg_palette_ix     = 3;

    n00b_text_style_t *over = n00b_str_style_new();
    over->italic            = N00B_TRI_YES; // override
    over->underline         = N00B_TRI_YES; // new

    n00b_text_style_t *m = n00b_str_style_merge(base, over);
    ASSERT_EQ(m->bold, N00B_TRI_YES);      // inherited
    ASSERT_EQ(m->italic, N00B_TRI_YES);    // overridden
    ASSERT_EQ(m->underline, N00B_TRI_YES); // new from overlay
    ASSERT_EQ(m->fg_palette_ix, 3);        // inherited
    ASSERT_EQ(m->bg_palette_ix, -1);       // both unset

    n00b_free(base);
    n00b_free(over);
    n00b_free(m);
}

TEST(test_merge_color)
{
    n00b_text_style_t *base = n00b_str_style_new();
    base->fg_rgb            = n00b_color_make(0x112233);

    n00b_text_style_t *over = n00b_str_style_new();
    // overlay has no fg color set -- should inherit base
    n00b_text_style_t *m = n00b_str_style_merge(base, over);
    ASSERT(n00b_color_is_set(m->fg_rgb));
    ASSERT_EQ(n00b_color_rgb(m->fg_rgb), 0x112233);

    // Now set overlay color -- should override
    over->fg_rgb          = n00b_color_make(0xAABBCC);
    n00b_text_style_t *m2 = n00b_str_style_merge(base, over);
    ASSERT_EQ(n00b_color_rgb(m2->fg_rgb), 0xAABBCC);

    n00b_free(base);
    n00b_free(over);
    n00b_free(m);
    n00b_free(m2);
}

TEST(test_eq_null_handling)
{
    ASSERT(n00b_str_style_eq(nullptr, nullptr));
    n00b_text_style_t *s = n00b_str_style_new();
    ASSERT(!n00b_str_style_eq(s, nullptr));
    ASSERT(!n00b_str_style_eq(nullptr, s));
    n00b_free(s);
}

TEST(test_is_empty_false_when_set)
{
    n00b_text_style_t *s = n00b_str_style_new();
    s->dim               = N00B_TRI_YES;
    ASSERT(!n00b_str_style_is_empty(s));
    n00b_free(s);
}

TEST(test_merge_text_case_font_hint)
{
    n00b_text_style_t *base = n00b_str_style_new();
    base->text_case         = N00B_TEXT_CASE_UPPER;
    base->font_hint         = N00B_FONT_MONO;

    n00b_text_style_t *over = n00b_str_style_new();
    // Both NONE/DEFAULT in overlay → inherit base
    n00b_text_style_t *m = n00b_str_style_merge(base, over);
    ASSERT_EQ(m->text_case, N00B_TEXT_CASE_UPPER);
    ASSERT_EQ(m->font_hint, N00B_FONT_MONO);

    // Set overlay → override
    over->font_hint       = N00B_FONT_SANS;
    n00b_text_style_t *m2 = n00b_str_style_merge(base, over);
    ASSERT_EQ(m2->font_hint, N00B_FONT_SANS);
    ASSERT_EQ(m2->text_case, N00B_TEXT_CASE_UPPER); // still inherited

    n00b_free(base);
    n00b_free(over);
    n00b_free(m);
    n00b_free(m2);
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_new_is_empty);
    RUN_TEST(test_copy_is_equal);
    RUN_TEST(test_merge_inheritance);
    RUN_TEST(test_merge_color);
    RUN_TEST(test_eq_null_handling);
    RUN_TEST(test_is_empty_false_when_set);
    RUN_TEST(test_merge_text_case_font_hint);
}

TEST_MAIN()
