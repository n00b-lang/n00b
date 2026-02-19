// Test: r"..." rich string literal compilation.
//
// Verifies that r"..." produces a static n00b_string_t with correct
// fields (byte count, codepoint count, data, styling).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// Minimal type stubs matching the runtime layout, so we can inspect
// the generated compound literals without linking libn00b.

typedef enum {
    N00B_TRI_UNSPECIFIED = 0,
    N00B_TRI_NO,
    N00B_TRI_YES,
} n00b_tristate_t;

typedef enum {
    N00B_TEXT_CASE_NONE  = 0,
    N00B_TEXT_CASE_UPPER,
    N00B_TEXT_CASE_LOWER,
    N00B_TEXT_CASE_TITLE,
    N00B_TEXT_CASE_CAPS,
} n00b_text_case_t;

typedef enum {
    N00B_FONT_DEFAULT = 0,
    N00B_FONT_MONO,
    N00B_FONT_SERIF,
    N00B_FONT_SANS,
} n00b_font_hint_t;

typedef uint32_t n00b_color_t;

typedef struct n00b_text_style_t {
    n00b_tristate_t   bold;
    n00b_tristate_t   italic;
    n00b_tristate_t   underline;
    n00b_tristate_t   double_underline;
    n00b_tristate_t   strikethrough;
    n00b_tristate_t   reverse;
    n00b_tristate_t   dim;
    n00b_tristate_t   blink;
    n00b_text_case_t  text_case;
    n00b_font_hint_t  font_hint;
    int8_t            font_index;
    int8_t            fg_palette_ix;
    int8_t            bg_palette_ix;
    n00b_color_t      fg_rgb;
    n00b_color_t      bg_rgb;
} n00b_text_style_t;

typedef struct {
    _Bool  has_value;
    size_t value;
} n00b_option_size_t;

typedef struct n00b_style_record_t {
    n00b_text_style_t   *info;
    const char          *tag;
    size_t               start;
    n00b_option_size_t   end;
} n00b_style_record_t;

typedef struct n00b_string_style_info_t {
    int64_t              num_styles;
    n00b_text_style_t   *base_style;
    n00b_style_record_t  styles[];
} n00b_string_style_info_t;

struct n00b_string_t {
    int64_t  u8_bytes;
    char    *data;
    int64_t  codepoints;
    void    *styling;
};

#define ASSERT(cond, msg)                                    \
    do {                                                     \
        if (!(cond)) {                                       \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg,      \
                    __FILE__, __LINE__);                      \
            return 1;                                        \
        }                                                    \
    } while (0)

int
main(void)
{
    // Test 1: Plain string (no markup)
    struct n00b_string_t *s1 = r"hello world";
    ASSERT(s1->u8_bytes == 11, "s1 byte count");
    ASSERT(s1->codepoints == 11, "s1 codepoint count");
    ASSERT(memcmp(s1->data, "hello world", 11) == 0, "s1 data");
    ASSERT(s1->styling == NULL, "s1 no styling");
    printf("  [PASS] plain string\n");

    // Test 2: Bold markup (using guillemets: « = \xC2\xAB, » = \xC2\xBB)
    struct n00b_string_t *s2 = r"«b»bold«/b»";
    ASSERT(s2->u8_bytes == 4, "s2 byte count");
    ASSERT(memcmp(s2->data, "bold", 4) == 0, "s2 data");
    ASSERT(s2->styling != NULL, "s2 has styling");
    n00b_string_style_info_t *info2 = (n00b_string_style_info_t *)s2->styling;
    ASSERT(info2->num_styles == 1, "s2 one style record");
    ASSERT(info2->styles[0].info != NULL, "s2 info not null");
    ASSERT(info2->styles[0].info->bold == N00B_TRI_YES, "s2 bold");
    ASSERT(info2->styles[0].start == 0, "s2 start");
    ASSERT(info2->styles[0].end.has_value == 1, "s2 end set");
    ASSERT(info2->styles[0].end.value == 4, "s2 end value");
    printf("  [PASS] bold markup\n");

    // Test 3: Named style (deferred)
    struct n00b_string_t *s3 = r"«em»text«/em»";
    ASSERT(s3->u8_bytes == 4, "s3 byte count");
    ASSERT(memcmp(s3->data, "text", 4) == 0, "s3 data");
    ASSERT(s3->styling != NULL, "s3 has styling");
    n00b_string_style_info_t *info3 = (n00b_string_style_info_t *)s3->styling;
    ASSERT(info3->num_styles == 1, "s3 one style record");
    ASSERT(info3->styles[0].info == NULL, "s3 info null (deferred)");
    ASSERT(info3->styles[0].tag != NULL, "s3 tag set");
    ASSERT(strcmp(info3->styles[0].tag, "em") == 0, "s3 tag is em");
    printf("  [PASS] named style (deferred)\n");

    // Test 4: Role (deferred)
    struct n00b_string_t *s4 = r"«@code»foo«/@code»";
    ASSERT(s4->u8_bytes == 3, "s4 byte count");
    ASSERT(memcmp(s4->data, "foo", 3) == 0, "s4 data");
    n00b_string_style_info_t *info4 = (n00b_string_style_info_t *)s4->styling;
    ASSERT(info4->styles[0].info == NULL, "s4 info null (deferred)");
    ASSERT(info4->styles[0].tag != NULL, "s4 tag set");
    ASSERT(strcmp(info4->styles[0].tag, "@code") == 0, "s4 tag is @code");
    printf("  [PASS] role (deferred)\n");

    // Test 5: Multiple styles
    struct n00b_string_t *s5 = r"«b»«i»both«/i»«/b»";
    ASSERT(s5->u8_bytes == 4, "s5 byte count");
    ASSERT(memcmp(s5->data, "both", 4) == 0, "s5 data");
    n00b_string_style_info_t *info5 = (n00b_string_style_info_t *)s5->styling;
    ASSERT(info5->num_styles == 2, "s5 two style records");
    printf("  [PASS] multiple styles\n");

    // Test 6: Mixed plain and styled text
    struct n00b_string_t *s6 = r"hello «b»world«/b»!";
    ASSERT(s6->u8_bytes == 12, "s6 byte count");
    ASSERT(memcmp(s6->data, "hello world!", 12) == 0, "s6 data");
    n00b_string_style_info_t *info6 = (n00b_string_style_info_t *)s6->styling;
    ASSERT(info6->num_styles == 1, "s6 one style record");
    ASSERT(info6->styles[0].start == 6, "s6 bold starts at 6");
    ASSERT(info6->styles[0].end.has_value == 1, "s6 end set");
    ASSERT(info6->styles[0].end.value == 11, "s6 bold ends at 11");
    printf("  [PASS] mixed plain and styled\n");

    printf("All r\"...\" tests passed.\n");
    return 0;
}
