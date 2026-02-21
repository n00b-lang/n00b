#include "test_unicode_helpers.h"
#include "strings/string_style.h"
#include "strings/style_registry.h"

// ===================================================================
// Tests
// ===================================================================

TEST(test_no_styling_by_default)
{
    n00b_string_t s = *r"hello";
    ASSERT(!n00b_option_is_set(n00b_str_get_style_info(s)));
}

TEST(test_set_base_style)
{
    n00b_string_t s    = *r"hello";
    n00b_text_style_t *st = n00b_str_style_new();
    st->bold              = N00B_TRI_YES;

    n00b_string_t s2 = n00b_str_set_base_style(s, st);
    auto info_opt = n00b_str_get_style_info(s2);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info = n00b_option_get(info_opt);
    ASSERT(info->base_style != nullptr);
    ASSERT_EQ(info->base_style->bold, N00B_TRI_YES);
    ASSERT_EQ(info->num_styles, 0);

    // Original unchanged.
    ASSERT(!n00b_option_is_set(n00b_str_get_style_info(s)));

    n00b_free(st);
}

TEST(test_add_style_record)
{
    n00b_string_t s    = *r"hello world";
    n00b_text_style_t *st = n00b_str_style_new();
    st->italic            = N00B_TRI_YES;

    n00b_string_t s2 = n00b_str_add_style(s, st, 0,
                                            n00b_option_set(size_t, 5));
    auto add_info_opt = n00b_str_get_style_info(s2);
    ASSERT(n00b_option_is_set(add_info_opt));
    n00b_string_style_info_t *info = n00b_option_get(add_info_opt);
    ASSERT_EQ(info->num_styles, 1);
    ASSERT_EQ(info->styles[0].start, 0);
    ASSERT(n00b_option_is_set(info->styles[0].end));
    ASSERT_EQ(n00b_option_get(info->styles[0].end), 5);
    ASSERT_EQ(info->styles[0].info->italic, N00B_TRI_YES);

    n00b_free(st);
}

TEST(test_open_ended_range)
{
    n00b_string_t s    = *r"abc";
    n00b_text_style_t *st = n00b_str_style_new();
    st->underline         = N00B_TRI_YES;

    n00b_string_t s2 = n00b_str_add_style(s, st, 1,
                                            n00b_option_none(size_t));
    n00b_string_style_info_t *info = n00b_option_get(n00b_str_get_style_info(s2));
    ASSERT_EQ(info->num_styles, 1);
    ASSERT(!n00b_option_is_set(info->styles[0].end));

    n00b_free(st);
}

TEST(test_resolve_style_at_no_styling)
{
    n00b_string_t s    = *r"hello";
    n00b_text_style_t *r = n00b_str_resolve_style_at(s, 0);
    ASSERT(n00b_str_style_is_empty(r));
    n00b_free(r);
}

TEST(test_resolve_style_at_base_only)
{
    n00b_string_t s    = *r"hello";
    n00b_text_style_t *st = n00b_str_style_new();
    st->bold              = N00B_TRI_YES;

    n00b_string_t s2   = n00b_str_set_base_style(s, st);
    n00b_text_style_t *r = n00b_str_resolve_style_at(s2, 3);
    ASSERT_EQ(r->bold, N00B_TRI_YES);
    ASSERT_EQ(r->italic, N00B_TRI_UNSPECIFIED);

    n00b_free(st);
    n00b_free(r);
}

TEST(test_resolve_style_at_with_range)
{
    n00b_string_t s    = *r"hello world";
    n00b_text_style_t *base = n00b_str_style_new();
    base->bold              = N00B_TRI_YES;

    n00b_text_style_t *range = n00b_str_style_new();
    range->italic            = N00B_TRI_YES;

    n00b_string_t s2 = n00b_str_set_base_style(s, base);
    s2 = n00b_str_add_style(s2, range, 6, n00b_option_set(size_t, 11));

    // Inside range: both bold and italic.
    n00b_text_style_t *r = n00b_str_resolve_style_at(s2, 7);
    ASSERT_EQ(r->bold, N00B_TRI_YES);
    ASSERT_EQ(r->italic, N00B_TRI_YES);
    n00b_free(r);

    // Outside range: only bold.
    r = n00b_str_resolve_style_at(s2, 3);
    ASSERT_EQ(r->bold, N00B_TRI_YES);
    ASSERT_EQ(r->italic, N00B_TRI_UNSPECIFIED);
    n00b_free(r);

    n00b_free(base);
    n00b_free(range);
}

TEST(test_strip_styles)
{
    n00b_string_t s    = *r"hello";
    n00b_text_style_t *st = n00b_str_style_new();
    st->bold              = N00B_TRI_YES;

    n00b_string_t s2 = n00b_str_set_base_style(s, st);
    ASSERT(n00b_option_is_set(n00b_str_get_style_info(s2)));

    n00b_string_t s3 = n00b_str_strip_styles(s2);
    ASSERT(!n00b_option_is_set(n00b_str_get_style_info(s3)));

    n00b_free(st);
}

TEST(test_deferred_tag_named_style)
{
    // Create a string with a deferred tag record (.info=nullptr, .tag="em").
    // Resolving should lazily look up "em" from the style registry.
    n00b_string_t s = *r"hello";

    // Manually build a style info with a deferred tag record.
    n00b_string_style_info_t *info =
        n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t, 1);
    info->num_styles      = 1;
    info->base_style      = nullptr;
    info->styles[0].info  = nullptr;
    info->styles[0].tag   = "em";
    info->styles[0].start = 0;
    info->styles[0].end   = n00b_option_none(size_t);

    n00b_string_t s2 = s;
    s2.styling = info;

    // Resolve style at position 0 — should trigger lazy lookup.
    n00b_text_style_t *resolved = n00b_str_resolve_style_at(s2, 0);
    // "em" is a built-in style with italic=YES.
    ASSERT(resolved != nullptr);
    ASSERT_EQ(resolved->italic, N00B_TRI_YES);

    // After resolution, the record's info should be cached.
    ASSERT(info->styles[0].info != nullptr);

    n00b_free(resolved);
}

TEST(test_deferred_tag_role)
{
    // Create a string with a deferred role record (.tag="@code").
    n00b_string_t s = *r"code";

    n00b_string_style_info_t *info =
        n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t, 1);
    info->num_styles      = 1;
    info->base_style      = nullptr;
    info->styles[0].info  = nullptr;
    info->styles[0].tag   = "@code";
    info->styles[0].start = 0;
    info->styles[0].end   = n00b_option_none(size_t);

    n00b_string_t s2 = s;
    s2.styling = info;

    n00b_text_style_t *resolved = n00b_str_resolve_style_at(s2, 0);
    // "@code" is a built-in role — should resolve to something.
    ASSERT(resolved != nullptr);

    // After resolution, info should be cached.
    ASSERT(info->styles[0].info != nullptr);

    n00b_free(resolved);
}

TEST(test_add_style_with_tag)
{
    // Use n00b_str_add_style with .tag kwarg to apply a named style.
    // Register "test.bold" (bold=YES), add via tag, resolve -> bold=YES.
    n00b_text_style_t *tb = n00b_str_style_new();
    tb->bold              = N00B_TRI_YES;
    n00b_str_style_register("test.bold", tb);

    n00b_string_t s = *r"hello world";
    n00b_string_t s2 = n00b_str_add_style(s, nullptr, 0,
                                            n00b_option_set(size_t, 5),
                                            .tag = "test.bold");

    auto info_opt = n00b_str_get_style_info(s2);
    ASSERT(n00b_option_is_set(info_opt));
    n00b_string_style_info_t *info = n00b_option_get(info_opt);
    ASSERT_EQ(info->num_styles, 1);
    ASSERT(info->styles[0].info == nullptr); // deferred -- not yet resolved
    ASSERT(info->styles[0].tag != nullptr);

    // Resolve at position inside range -> should trigger lazy lookup.
    n00b_text_style_t *r = n00b_str_resolve_style_at(s2, 2);
    ASSERT(r != nullptr);
    ASSERT_EQ(r->bold, N00B_TRI_YES);

    // Outside the range -> no bold.
    n00b_text_style_t *r2 = n00b_str_resolve_style_at(s2, 7);
    ASSERT(r2 != nullptr);
    ASSERT(n00b_str_style_is_empty(r2));

    n00b_free(tb);
    n00b_free(r);
    n00b_free(r2);
}

TEST(test_add_style_with_role_tag)
{
    // Use existing @code role (font_hint=MONO), add via .tag = "@code".
    n00b_string_t s = *r"code snippet";
    n00b_string_t s2 = n00b_str_add_style(s, nullptr, 0,
                                            n00b_option_none(size_t),
                                            .tag = "@code");

    n00b_text_style_t *r = n00b_str_resolve_style_at(s2, 0);
    ASSERT(r != nullptr);
    ASSERT_EQ(r->font_hint, N00B_FONT_MONO);

    n00b_free(r);
}

TEST(test_deferred_tag_unknown)
{
    // Unknown tag: should be skipped without crash.
    n00b_string_t s = *r"xyz";

    n00b_string_style_info_t *info =
        n00b_alloc_flex(n00b_string_style_info_t, n00b_style_record_t, 1);
    info->num_styles      = 1;
    info->base_style      = nullptr;
    info->styles[0].info  = nullptr;
    info->styles[0].tag   = "nonexistent_style_xyzzy";
    info->styles[0].start = 0;
    info->styles[0].end   = n00b_option_none(size_t);

    n00b_string_t s2 = s;
    s2.styling = info;

    // Should get an empty style (the unknown tag is skipped).
    n00b_text_style_t *resolved = n00b_str_resolve_style_at(s2, 0);
    ASSERT(resolved != nullptr);
    ASSERT(n00b_str_style_is_empty(resolved));

    n00b_free(resolved);
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_no_styling_by_default);
    RUN_TEST(test_set_base_style);
    RUN_TEST(test_add_style_record);
    RUN_TEST(test_open_ended_range);
    RUN_TEST(test_resolve_style_at_no_styling);
    RUN_TEST(test_resolve_style_at_base_only);
    RUN_TEST(test_resolve_style_at_with_range);
    RUN_TEST(test_strip_styles);
    RUN_TEST(test_add_style_with_tag);
    RUN_TEST(test_add_style_with_role_tag);
    RUN_TEST(test_deferred_tag_named_style);
    RUN_TEST(test_deferred_tag_role);
    RUN_TEST(test_deferred_tag_unknown);
}

TEST_MAIN()
