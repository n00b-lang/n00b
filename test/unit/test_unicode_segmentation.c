#include "test_unicode_helpers.h"
#include "unicode/segmentation.h"
#include "unicode/encoding.h"

// ---------------------------------------------------------------------------
// Manual unit tests
// ---------------------------------------------------------------------------

TEST(test_grapheme_ascii)
{
    ASSERT_EQ(n00b_unicode_grapheme_count(*r"abc"), 3);
}

TEST(test_grapheme_combining)
{
    ASSERT_EQ(n00b_unicode_grapheme_count(*r"e\xCC\x81"), 1);
}

TEST(test_grapheme_crlf)
{
    ASSERT_EQ(n00b_unicode_grapheme_count(*r"\r\n"), 1);
}

TEST(test_grapheme_emoji_flag)
{
    ASSERT_EQ(n00b_unicode_grapheme_count(*r"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"), 1);
}

TEST(test_grapheme_hangul)
{
    ASSERT_EQ(n00b_unicode_grapheme_count(*r"\xED\x95\x9C"), 1);
}

TEST(test_grapheme_iter)
{
    n00b_unicode_break_iter_t *it = n00b_unicode_grapheme_iter(*r"ab");

    int32_t b1 = n00b_unicode_break_next(it);
    ASSERT_EQ(b1, 1);

    int32_t b2 = n00b_unicode_break_next(it);
    ASSERT_EQ(b2, 2);

    int32_t b3 = n00b_unicode_break_next(it);
    ASSERT_EQ(b3, -1);

    n00b_unicode_break_iter_free(it);
}

TEST(test_word_iter)
{
    n00b_unicode_break_iter_t *it = n00b_unicode_word_iter(*r"Hello world");

    int count = 0;
    while (n00b_unicode_break_next(it) >= 0) {
        count++;
    }
    ASSERT(count >= 3);
    n00b_unicode_break_iter_free(it);
}

TEST(test_sentence_iter)
{
    n00b_unicode_break_iter_t *it = n00b_unicode_sentence_iter(*r"Hello. World.");

    int count = 0;
    while (n00b_unicode_break_next(it) >= 0) {
        count++;
    }
    ASSERT(count >= 1);
    n00b_unicode_break_iter_free(it);
}

// ---------------------------------------------------------------------------
// Conformance test harness for break tests
// ---------------------------------------------------------------------------

// Thin wrappers: ncc keyword functions have an extra hidden kargs param,
// so their address doesn't match a plain function-pointer type.
static n00b_unicode_break_iter_t *wrap_grapheme_iter(n00b_string_t s) { return n00b_unicode_grapheme_iter(s); }
static n00b_unicode_break_iter_t *wrap_word_iter(n00b_string_t s)     { return n00b_unicode_word_iter(s); }
static n00b_unicode_break_iter_t *wrap_sentence_iter(n00b_string_t s) { return n00b_unicode_sentence_iter(s); }

static void
run_break_conformance(const char *filename, const char *label,
                      n00b_unicode_break_iter_t *(*iter_fn)(n00b_string_t))
{
    FILE *f = fopen(filename, "r");
    if (!f) {
        printf("  [SKIP] %s not found\n", filename);
        return;
    }

    char line[4096];
    int total = 0, passed_count = 0, fail_count = 0;

    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        n00b_codepoint_t cps[256];
        int breaks[257]; // breaks[i] = 1 if break before cps[i]
        memset(breaks, 0, sizeof(breaks));

        uint32_t n = parse_break_test_line(line, cps, breaks, 256);
        if (n == 0) continue;

        // Build UTF-8 string from codepoints
        n00b_string_t s = cps_to_str(cps, n);

        // Run break iterator and collect boundary byte offsets
        n00b_unicode_break_iter_t *it = iter_fn(s);
        uint32_t actual_breaks[257];
        uint32_t n_actual = 0;

        int32_t b;
        while ((b = n00b_unicode_break_next(it)) >= 0) {
            if (n_actual < 256) {
                actual_breaks[n_actual++] = (uint32_t)b;
            }
        }
        // End of string is always a boundary
        if (n_actual == 0 || actual_breaks[n_actual - 1] != (uint32_t)s.u8_bytes) {
            actual_breaks[n_actual++] = (uint32_t)s.u8_bytes;
        }

        n00b_unicode_break_iter_free(it);

        // Build expected boundary byte offsets from breaks[] array
        uint32_t expected_breaks[257];
        uint32_t n_expected = 0;
        uint32_t byte_pos = 0;

        for (uint32_t i = 0; i < n; i++) {
            if (i > 0 && breaks[i]) {
                expected_breaks[n_expected++] = byte_pos;
            }
            char tmp[4];
            byte_pos += n00b_unicode_utf8_encode(cps[i], tmp);
        }
        // End of string boundary
        expected_breaks[n_expected++] = byte_pos;

        // Compare
        bool ok = (n_expected == n_actual);
        if (ok) {
            for (uint32_t i = 0; i < n_expected; i++) {
                if (expected_breaks[i] != actual_breaks[i]) {
                    ok = false;
                    break;
                }
            }
        }

        total++;
        if (ok) {
            passed_count++;
        } else {
            fail_count++;
            if (fail_count <= 5) {
                // Show first few failures
                printf("    FAIL line %d: expected %d breaks, got %d | ",
                       total, n_expected, n_actual);
                printf("cps:");
                for (uint32_t i = 0; i < n; i++) {
                    printf(" %04X", cps[i]);
                }
                printf("\n");
            }
        }
    }

    fclose(f);

    CONFORMANCE_RESULT(label, passed_count, total, fail_count);
}

static void run_tests(void)
{
    RUN_TEST(test_grapheme_ascii);
    RUN_TEST(test_grapheme_combining);
    RUN_TEST(test_grapheme_crlf);
    RUN_TEST(test_grapheme_emoji_flag);
    RUN_TEST(test_grapheme_hangul);
    RUN_TEST(test_grapheme_iter);
    RUN_TEST(test_word_iter);
    RUN_TEST(test_sentence_iter);

    run_break_conformance("test/data/GraphemeBreakTest.txt",
                          "GraphemeBreakTest.txt conformance:",
                          wrap_grapheme_iter);
    run_break_conformance("test/data/WordBreakTest.txt",
                          "WordBreakTest.txt conformance:",
                          wrap_word_iter);
    run_break_conformance("test/data/SentenceBreakTest.txt",
                          "SentenceBreakTest.txt conformance:",
                          wrap_sentence_iter);
}

TEST_MAIN()
