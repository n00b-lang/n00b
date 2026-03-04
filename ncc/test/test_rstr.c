// test_rstr.c — Tests for r"..." rich string transform.

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

static int
bytes_eq(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a;
    const unsigned char *pb = b;

    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return 0;
        }
    }

    return 1;
}

typedef struct ncc_text_style_t {
    int bold;
    int italic;
    int underline;
    int double_underline;
    int strikethrough;
    int reverse;
    int dim;
    int blink;
    int text_case;
} ncc_text_style_t;

typedef struct {
    int    has_value;
    size_t value;
} ncc_option_size_t;

typedef struct {
    ncc_text_style_t *info;
    const char        *tag;
    size_t             start;
    ncc_option_size_t end;
} ncc_style_record_t;

struct ncc_string_t {
    size_t u8_bytes;
    char  *data;
    size_t codepoints;
    void  *styling;
};

typedef struct ncc_string_t ncc_string_t;

typedef struct {
    int64_t             num_styles;
    ncc_text_style_t  *base_style;
    ncc_style_record_t styles[];
} ncc_styling_info_t;

static int test_count = 0;
static int fail_count = 0;

#define TEST(name) \
    do { test_count++; printf("  test %d: %s ... ", test_count, name); } while (0)
#define PASS() \
    do { printf("PASS\n"); } while (0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); fail_count++; } while (0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while (0)

static void
test_plain_string(void)
{
    TEST("plain r-string (no markup)");

    ncc_string_t *s = r"Hello world";

    CHECK(s != NULL, "null pointer");
    CHECK(s->u8_bytes == 11, "wrong byte count");
    CHECK(s->codepoints == 11, "wrong codepoint count");
    CHECK(s->styling == NULL, "styling should be NULL");
    CHECK(bytes_eq(s->data, "Hello world", 11), "wrong data");
    PASS();
}

static void
test_bold_markup(void)
{
    TEST("styled r-string with bold");

    ncc_string_t *s = r"Hello «b»world«/b»!";

    CHECK(s != NULL, "null pointer");
    CHECK(s->u8_bytes == 12, "wrong byte count");
    CHECK(bytes_eq(s->data, "Hello world!", 12), "wrong stripped text");
    CHECK(s->styling != NULL, "styling should not be NULL");

    ncc_styling_info_t *si = (ncc_styling_info_t *)s->styling;

    CHECK(si->num_styles == 1, "expected 1 style record");
    CHECK(si->styles[0].info != NULL, "style info should not be NULL");
    CHECK(si->styles[0].info->bold == 2, "bold should be 2 (TRI_YES)");
    CHECK(si->styles[0].start == 6, "start should be 6");
    CHECK(si->styles[0].end.has_value == 1, "end should have value");
    CHECK(si->styles[0].end.value == 11, "end should be 11");
    PASS();
}

static void
test_adjacent_concat(void)
{
    TEST("adjacent string concatenation");

    ncc_string_t *s = r"Hello " "world";

    CHECK(s != NULL, "null pointer");
    CHECK(s->u8_bytes == 11, "wrong byte count");
    CHECK(bytes_eq(s->data, "Hello world", 11), "wrong data");
    PASS();
}

static void
test_escaped_guillemet(void)
{
    TEST("escaped guillemet");

    ncc_string_t *s = r"Use \« for literal";

    CHECK(s != NULL, "null pointer");
    CHECK(s->u8_bytes == 18, "wrong byte count");
    CHECK(s->styling == NULL, "should have no styling");
    CHECK((unsigned char)s->data[4] == 0xC2, "expected C2");
    CHECK((unsigned char)s->data[5] == 0xAB, "expected AB");
    PASS();
}

static void
test_multiple_styles(void)
{
    TEST("multiple style tags");

    ncc_string_t *s = r"«b»bold«/b» and «i»italic«/i»";

    CHECK(s != NULL, "null pointer");
    CHECK(bytes_eq(s->data, "bold and italic", 15), "wrong stripped text");
    CHECK(s->u8_bytes == 15, "wrong byte count");

    ncc_styling_info_t *si = (ncc_styling_info_t *)s->styling;

    CHECK(si->num_styles == 2, "expected 2 style records");
    PASS();
}

static void
test_bracket_syntax(void)
{
    TEST("bracket tag syntax");

    ncc_string_t *s = r"Hello [|b|]world[|/b|]!";

    CHECK(s != NULL, "null pointer");
    CHECK(s->u8_bytes == 12, "wrong byte count");
    CHECK(bytes_eq(s->data, "Hello world!", 12), "wrong stripped text");
    CHECK(s->styling != NULL, "styling should not be NULL");

    ncc_styling_info_t *si = (ncc_styling_info_t *)s->styling;

    CHECK(si->num_styles == 1, "expected 1 style record");
    CHECK(si->styles[0].info->bold == 2, "bold should be 2");
    PASS();
}

static void
test_reset(void)
{
    TEST("reset tag");

    ncc_string_t *s = r"«b»bold«/»normal";

    CHECK(s != NULL, "null pointer");
    CHECK(bytes_eq(s->data, "boldnormal", 10), "wrong stripped text");

    ncc_styling_info_t *si = (ncc_styling_info_t *)s->styling;

    CHECK(si->num_styles == 1, "expected 1 style record");
    CHECK(si->styles[0].start == 0, "start should be 0");
    CHECK(si->styles[0].end.has_value == 1, "end should have value");
    CHECK(si->styles[0].end.value == 4, "end should be 4");
    PASS();
}

static void
test_unclosed_tag(void)
{
    TEST("unclosed tag extends to end");

    ncc_string_t *s = r"«b»bold forever";

    CHECK(s != NULL, "null pointer");
    CHECK(bytes_eq(s->data, "bold forever", 12), "wrong stripped text");

    ncc_styling_info_t *si = (ncc_styling_info_t *)s->styling;

    CHECK(si->num_styles == 1, "expected 1 style record");
    CHECK(si->styles[0].end.has_value == 0, "end should be open (no value)");
    PASS();
}

int
main(void)
{
    printf("=== r-string transform tests ===\n");

    test_plain_string();
    test_bold_markup();
    test_adjacent_concat();
    test_escaped_guillemet();
    test_multiple_styles();
    test_bracket_syntax();
    test_reset();
    test_unclosed_tag();

    printf("\n%d tests, %d failures\n", test_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
