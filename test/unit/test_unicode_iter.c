#include "test_unicode_helpers.h"
#include "text/unicode/iter.h"

TEST(test_foreach_cp)
{
    n00b_string_t          s = *r"A\xC3\xA9"; // "Aé"
    n00b_codepoint_t cps[10];
    int                 n = 0;

    n00b_unicode_foreach_cp(&s, cp)
    {
        cps[n++] = (n00b_codepoint_t)cp;
    }

    ASSERT_EQ(n, 2);
    ASSERT_EQ(cps[0], 'A');
    ASSERT_EQ(cps[1], 0x00E9);
}

TEST(test_foreach_cp_break)
{
    n00b_string_t s = *r"abcdef";
    int        n = 0;

    n00b_unicode_foreach_cp(&s, cp)
    {
        (void)cp;
        n++;
        if (n == 3)
            break;
    }

    ASSERT_EQ(n, 3);
}

TEST(test_foreach_cp_continue)
{
    n00b_string_t s     = *r"abc";
    int        count = 0;

    n00b_unicode_foreach_cp(&s, cp)
    {
        if (cp == 'b')
            continue;
        count++;
    }

    ASSERT_EQ(count, 2);
}

TEST(test_foreach_grapheme)
{
    // "e" + combining acute + "x" → 2 grapheme clusters
    n00b_string_t s = *r"e\xCC\x81x";
    int        n = 0;
    uint32_t   lens[10];

    n00b_unicode_foreach_grapheme(&s, g)
    {
        lens[n++] = g.u8_bytes;
    }

    ASSERT_EQ(n, 2);
    ASSERT_EQ(lens[0], 3); // e + combining acute = 3 bytes
    ASSERT_EQ(lens[1], 1); // x = 1 byte
}

TEST(test_foreach_grapheme_break)
{
    n00b_string_t s = *r"abcdef";
    int        n = 0;

    n00b_unicode_foreach_grapheme(&s, g)
    {
        (void)g;
        n++;
        if (n == 2)
            break;
    }

    ASSERT_EQ(n, 2);
}

TEST(test_foreach_word)
{
    n00b_string_t s = *r"Hello World";
    int        n = 0;

    n00b_unicode_foreach_word(&s, w)
    {
        (void)w;
        n++;
    }

    // Words include whitespace segments: "Hello", " ", "World"
    ASSERT(n >= 2);
}

TEST(test_foreach_sentence)
{
    n00b_string_t s = *r"Hello. World.";
    int        n = 0;

    n00b_unicode_foreach_sentence(&s, sent)
    {
        (void)sent;
        n++;
    }

    ASSERT(n >= 1);
}

TEST(test_foreach_line_lf)
{
    n00b_string_t s = *r"line1\nline2\nline3";
    int        n = 0;
    char       bufs[3][20];

    n00b_unicode_foreach_line(&s, line)
    {
        if (n < 3) {
            memcpy(bufs[n], line.data, line.u8_bytes);
            bufs[n][line.u8_bytes] = '\0';
        }
        n++;
    }

    ASSERT_EQ(n, 3);
    ASSERT_STR_EQ(bufs[0], "line1");
    ASSERT_STR_EQ(bufs[1], "line2");
    ASSERT_STR_EQ(bufs[2], "line3");
}

TEST(test_foreach_line_crlf)
{
    n00b_string_t s = *r"a\r\nb\r\nc";
    int        n = 0;

    n00b_unicode_foreach_line(&s, line)
    {
        (void)line;
        n++;
    }

    ASSERT_EQ(n, 3);
}

TEST(test_foreach_line_mixed)
{
    n00b_string_t s = *r"a\nb\rc\r\nd";
    int        n = 0;

    n00b_unicode_foreach_line(&s, line)
    {
        (void)line;
        n++;
    }

    ASSERT_EQ(n, 4);
}

TEST(test_foreach_line_break)
{
    n00b_string_t s = *r"a\nb\nc\nd";
    int        n = 0;

    n00b_unicode_foreach_line(&s, line)
    {
        (void)line;
        n++;
        if (n == 2)
            break;
    }

    ASSERT_EQ(n, 2);
}

TEST(test_foreach_cp_empty)
{
    n00b_string_t s = *r"";
    int        n = 0;

    n00b_unicode_foreach_cp(&s, cp)
    {
        (void)cp;
        n++;
    }

    ASSERT_EQ(n, 0);
}

TEST(test_foreach_grapheme_empty)
{
    n00b_string_t s = *r"";
    int        n = 0;

    n00b_unicode_foreach_grapheme(&s, g)
    {
        (void)g;
        n++;
    }

    ASSERT_EQ(n, 0);
}

static void
run_tests(void)
{
    RUN_TEST(test_foreach_cp);
    RUN_TEST(test_foreach_cp_break);
    RUN_TEST(test_foreach_cp_continue);
    RUN_TEST(test_foreach_grapheme);
    RUN_TEST(test_foreach_grapheme_break);
    RUN_TEST(test_foreach_word);
    RUN_TEST(test_foreach_sentence);
    RUN_TEST(test_foreach_line_lf);
    RUN_TEST(test_foreach_line_crlf);
    RUN_TEST(test_foreach_line_mixed);
    RUN_TEST(test_foreach_line_break);
    RUN_TEST(test_foreach_cp_empty);
    RUN_TEST(test_foreach_grapheme_empty);
}

TEST_MAIN()
