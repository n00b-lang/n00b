#include "test_unicode_helpers.h"
#include "text/strings/rich_desc.h"

// ===================================================================
// Helpers
// ===================================================================

#define PARSE(s) n00b_rich_desc_parse((s), (int32_t)strlen(s))

// ===================================================================
// Tests
// ===================================================================

TEST(test_plain_text)
{
    n00b_rich_desc_t *d = PARSE("hello world");
    ASSERT_EQ(d->num_segments, 1);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_TEXT);
    ASSERT_EQ(d->segments[0].offset, 0);
    ASSERT_EQ(d->segments[0].length, 11);
}

TEST(test_bold_tag)
{
    n00b_rich_desc_t *d = PARSE("[|b|]hello[|/b|]");
    ASSERT_EQ(d->num_segments, 3);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_PROP_ON);
    ASSERT_STR_EQ(d->segments[0].tag, "b");
    ASSERT_EQ(d->segments[1].kind, N00B_RICH_TEXT);
    ASSERT_EQ(d->segments[2].kind, N00B_RICH_PROP_OFF);
    ASSERT_STR_EQ(d->segments[2].tag, "b");
}

TEST(test_named_style)
{
    n00b_rich_desc_t *d = PARSE("[|em|]text[|/em|]");
    ASSERT_EQ(d->num_segments, 3);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_STYLE_ON);
    ASSERT_STR_EQ(d->segments[0].tag, "em");
    ASSERT_EQ(d->segments[2].kind, N00B_RICH_STYLE_OFF);
}

TEST(test_role_tag)
{
    n00b_rich_desc_t *d = PARSE("[|@code|]x[|/@code|]");
    ASSERT_EQ(d->num_segments, 3);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_ROLE_ON);
    ASSERT_STR_EQ(d->segments[0].tag, "@code");
    ASSERT_EQ(d->segments[2].kind, N00B_RICH_ROLE_OFF);
    ASSERT_STR_EQ(d->segments[2].tag, "@code");
}

TEST(test_reset)
{
    n00b_rich_desc_t *d = PARSE("[|b|]text[|/|]more");
    ASSERT(d->num_segments >= 3);
    // Find the reset.
    bool found_reset = false;
    for (int i = 0; i < d->num_segments; i++) {
        if (d->segments[i].kind == N00B_RICH_RESET) {
            found_reset = true;
        }
    }
    ASSERT(found_reset);
}

TEST(test_subst_auto_index)
{
    n00b_rich_desc_t *d = PARSE("[|#|] and [|#|]");
    int sub_count       = 0;
    for (int i = 0; i < d->num_segments; i++) {
        if (d->segments[i].kind == N00B_RICH_SUBST) {
            ASSERT_EQ(d->segments[i].offset, sub_count);
            sub_count++;
        }
    }
    ASSERT_EQ(sub_count, 2);
}

TEST(test_subst_explicit_index)
{
    n00b_rich_desc_t *d = PARSE("[|#1|]");
    ASSERT_EQ(d->num_segments, 1);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_SUBST);
    ASSERT_EQ(d->segments[0].offset, 1);
}

TEST(test_subst_with_spec)
{
    n00b_rich_desc_t *d = PARSE("[|#0:,d|]");
    ASSERT_EQ(d->num_segments, 1);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_SUBST);
    ASSERT_EQ(d->segments[0].offset, 0);
    ASSERT(d->segments[0].tag != nullptr);
    ASSERT_STR_EQ(d->segments[0].tag, ",d");
}

TEST(test_subst_strip)
{
    n00b_rich_desc_t *d = PARSE("[|#!|]");
    ASSERT_EQ(d->num_segments, 1);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_SUBST);
    ASSERT(d->segments[0].strip_style);
}

TEST(test_guillemet_syntax)
{
    // «b» and «/b» in UTF-8
    n00b_rich_desc_t *d = PARSE("\xC2\xAB" "b" "\xC2\xBB"
                                 "text"
                                 "\xC2\xAB" "/b" "\xC2\xBB");
    ASSERT_EQ(d->num_segments, 3);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_PROP_ON);
    ASSERT_STR_EQ(d->segments[0].tag, "b");
    ASSERT_EQ(d->segments[1].kind, N00B_RICH_TEXT);
    ASSERT_EQ(d->segments[2].kind, N00B_RICH_PROP_OFF);
}

TEST(test_escape)
{
    n00b_rich_desc_t *d = PARSE("\\[|not a tag|]");
    // The leading \[ should produce '[' as text, then "|not a tag|]" as text.
    // Exact segment count depends on parser, but there should be no tags.
    for (int i = 0; i < d->num_segments; i++) {
        ASSERT_EQ(d->segments[i].kind, N00B_RICH_TEXT);
    }
}

TEST(test_cache_hit)
{
    const char *s = "cached [|b|]text[|/b|]";
    n00b_rich_desc_t *d1 = n00b_rich_desc_parse(s, (int32_t)strlen(s));
    n00b_rich_desc_t *d2 = n00b_rich_desc_parse(s, (int32_t)strlen(s));
    ASSERT(d1 == d2); // same pointer = cache hit
}

TEST(test_case_tag)
{
    n00b_rich_desc_t *d = PARSE("[|upper|]TEXT[|/upper|]");
    ASSERT_EQ(d->num_segments, 3);
    ASSERT_EQ(d->segments[0].kind, N00B_RICH_PROP_ON);
    ASSERT_STR_EQ(d->segments[0].tag, "upper");
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    RUN_TEST(test_plain_text);
    RUN_TEST(test_bold_tag);
    RUN_TEST(test_named_style);
    RUN_TEST(test_role_tag);
    RUN_TEST(test_reset);
    RUN_TEST(test_subst_auto_index);
    RUN_TEST(test_subst_explicit_index);
    RUN_TEST(test_subst_with_spec);
    RUN_TEST(test_subst_strip);
    RUN_TEST(test_guillemet_syntax);
    RUN_TEST(test_escape);
    RUN_TEST(test_cache_hit);
    RUN_TEST(test_case_tag);
}

TEST_MAIN()
