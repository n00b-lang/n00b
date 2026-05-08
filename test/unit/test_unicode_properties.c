#include "test_unicode_helpers.h"
#include "text/unicode/properties.h"
#include "text/unicode/encoding.h"


TEST(test_general_category)
{
    ASSERT_EQ(n00b_unicode_general_category('A'), N00B_UNICODE_GC_LU);
    ASSERT_EQ(n00b_unicode_general_category('a'), N00B_UNICODE_GC_LL);
    ASSERT_EQ(n00b_unicode_general_category('0'), N00B_UNICODE_GC_ND);
    ASSERT_EQ(n00b_unicode_general_category(' '), N00B_UNICODE_GC_ZS);
    ASSERT_EQ(n00b_unicode_general_category(0x00), N00B_UNICODE_GC_CC);
    ASSERT_EQ(n00b_unicode_general_category(0x4E16), N00B_UNICODE_GC_LO); // 世
}

TEST(test_combining_class)
{
    ASSERT_EQ(n00b_unicode_combining_class('A'), 0);
    ASSERT_EQ(n00b_unicode_combining_class(0x0300), 230); // COMBINING GRAVE ACCENT
    ASSERT_EQ(n00b_unicode_combining_class(0x0327), 202); // COMBINING CEDILLA
}

TEST(test_script)
{
    n00b_unicode_script_t latin = n00b_unicode_script('A');
    ASSERT(latin != 0); // Should not be Unknown/Common for Latin letter
    const char *name = n00b_unicode_script_name(latin);
    ASSERT(name != nullptr);
    ASSERT_STR_EQ(name, "Latin");

    n00b_unicode_script_t han = n00b_unicode_script(0x4E16);
    ASSERT_STR_EQ(n00b_unicode_script_name(han), "Han");
}

TEST(test_bidi_class)
{
    ASSERT_EQ(n00b_unicode_bidi_class('A'), N00B_UNICODE_BIDI_L);
    ASSERT_EQ(n00b_unicode_bidi_class(0x0627), N00B_UNICODE_BIDI_AL); // Arabic Alef
    ASSERT_EQ(n00b_unicode_bidi_class('0'), N00B_UNICODE_BIDI_EN);
}

TEST(test_east_asian_width)
{
    ASSERT_EQ(n00b_unicode_east_asian_width('A'), N00B_UNICODE_EAW_NA);
    ASSERT_EQ(n00b_unicode_east_asian_width(0x4E16), N00B_UNICODE_EAW_W); // 世 is Wide
    ASSERT_EQ(n00b_unicode_east_asian_width(0xFF01), N00B_UNICODE_EAW_F);  // ！ Fullwidth
}

TEST(test_char_width)
{
    ASSERT_EQ(n00b_unicode_char_width('A'), 1);
    ASSERT_EQ(n00b_unicode_char_width(0x4E16), 2); // Wide
    ASSERT_EQ(n00b_unicode_char_width(0x0300), 0); // Combining mark
}

TEST(test_binary_properties)
{
    ASSERT(n00b_unicode_has_property(' ', N00B_UNICODE_PROP_WHITE_SPACE));
    ASSERT(!n00b_unicode_has_property('A', N00B_UNICODE_PROP_WHITE_SPACE));
    ASSERT(n00b_unicode_has_property('A', N00B_UNICODE_PROP_ALPHABETIC));
    ASSERT(n00b_unicode_has_property('A', N00B_UNICODE_PROP_UPPERCASE));
    ASSERT(n00b_unicode_has_property('a', N00B_UNICODE_PROP_LOWERCASE));
    ASSERT(n00b_unicode_has_property('0', N00B_UNICODE_PROP_HEX_DIGIT));
    ASSERT(n00b_unicode_has_property('a', N00B_UNICODE_PROP_HEX_DIGIT));
    ASSERT(!n00b_unicode_has_property('g', N00B_UNICODE_PROP_HEX_DIGIT));
}

TEST(test_ascii_changes_when_casefolded)
{
    ASSERT(n00b_unicode_has_property('A',
                                     N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED));
    ASSERT(n00b_unicode_has_property('Z',
                                     N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED));
    ASSERT(!n00b_unicode_has_property('a',
                                      N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED));
    ASSERT(!n00b_unicode_has_property('z',
                                      N00B_UNICODE_PROP_CHANGES_WHEN_CASEFOLDED));

    ASSERT(n00b_unicode_has_property('a',
                                     N00B_UNICODE_PROP_CHANGES_WHEN_CASEMAPPED));
}

TEST(test_ascii_table_only_properties)
{
    ASSERT(n00b_unicode_has_property('0', N00B_UNICODE_PROP_EMOJI));
    ASSERT(n00b_unicode_has_property('9', N00B_UNICODE_PROP_EMOJI));
    ASSERT(n00b_unicode_has_property('#', N00B_UNICODE_PROP_EMOJI_COMPONENT));
    ASSERT(n00b_unicode_has_property('*', N00B_UNICODE_PROP_EMOJI_COMPONENT));
    ASSERT(n00b_unicode_has_property('A', N00B_UNICODE_PROP_GRAPHEME_BASE));
    ASSERT(n00b_unicode_has_property('i', N00B_UNICODE_PROP_SOFT_DOTTED));
    ASSERT(n00b_unicode_has_property('j', N00B_UNICODE_PROP_SOFT_DOTTED));
}

TEST(test_id_properties)
{
    ASSERT(n00b_unicode_has_property('A', N00B_UNICODE_PROP_ID_START));
    ASSERT(n00b_unicode_has_property('A', N00B_UNICODE_PROP_ID_CONTINUE));
    ASSERT(!n00b_unicode_has_property('0', N00B_UNICODE_PROP_ID_START));
    ASSERT(n00b_unicode_has_property('0', N00B_UNICODE_PROP_ID_CONTINUE));
    ASSERT(n00b_unicode_has_property('_', N00B_UNICODE_PROP_ID_CONTINUE));
}

TEST(test_display_width)
{
    ASSERT_EQ(n00b_unicode_display_width(r"Hello"), 5);
    ASSERT_EQ(n00b_unicode_display_width(r"世界"), 4);
}

TEST(test_joining_type)
{
    ASSERT_EQ(n00b_unicode_joining_type('A'), N00B_UNICODE_JT_U); // Non_Joining
    ASSERT_EQ(n00b_unicode_joining_type(0x0627), N00B_UNICODE_JT_R); // Arabic Alef = Right_Joining
}

// --- Numeric property tests ---

TEST(test_numeric_ascii_digits)
{
    for (int i = 0; i <= 9; i++) {
        n00b_codepoint_t cp = '0' + i;
        ASSERT_EQ(n00b_unicode_numeric_type(cp), N00B_UNICODE_NUMERIC_DECIMAL);
        n00b_unicode_numeric_value_t v = n00b_unicode_numeric_value(cp);
        ASSERT_EQ(v.numerator, i);
        ASSERT_EQ(v.denominator, 1);
    }
}

TEST(test_numeric_arabic_digits)
{
    // U+0660-U+0669: Arabic-Indic digits
    for (int i = 0; i <= 9; i++) {
        n00b_codepoint_t cp = 0x0660 + i;
        ASSERT_EQ(n00b_unicode_numeric_type(cp), N00B_UNICODE_NUMERIC_DECIMAL);
        n00b_unicode_numeric_value_t v = n00b_unicode_numeric_value(cp);
        ASSERT_EQ(v.numerator, i);
        ASSERT_EQ(v.denominator, 1);
    }
}

TEST(test_numeric_fractions)
{
    // U+00BC = VULGAR FRACTION ONE QUARTER (1/4)
    n00b_unicode_numeric_value_t v = n00b_unicode_numeric_value(0x00BC);
    ASSERT_EQ(v.type, N00B_UNICODE_NUMERIC_NUMERIC);
    ASSERT_EQ(v.numerator, 1);
    ASSERT_EQ(v.denominator, 4);

    // U+00BD = VULGAR FRACTION ONE HALF (1/2)
    v = n00b_unicode_numeric_value(0x00BD);
    ASSERT_EQ(v.type, N00B_UNICODE_NUMERIC_NUMERIC);
    ASSERT_EQ(v.numerator, 1);
    ASSERT_EQ(v.denominator, 2);

    // U+00BE = VULGAR FRACTION THREE QUARTERS (3/4)
    v = n00b_unicode_numeric_value(0x00BE);
    ASSERT_EQ(v.type, N00B_UNICODE_NUMERIC_NUMERIC);
    ASSERT_EQ(v.numerator, 3);
    ASSERT_EQ(v.denominator, 4);
}

TEST(test_numeric_non_numeric)
{
    ASSERT_EQ(n00b_unicode_numeric_type('A'), N00B_UNICODE_NUMERIC_NONE);
    n00b_unicode_numeric_value_t v = n00b_unicode_numeric_value('A');
    ASSERT_EQ(v.type, N00B_UNICODE_NUMERIC_NONE);
}

TEST(test_digit_value)
{
    n00b_option_t(int32_t) d5 = n00b_unicode_digit_value('5');
    ASSERT(n00b_option_is_set(d5));
    ASSERT_EQ(n00b_option_get(d5), 5);
    n00b_option_t(int32_t) d0 = n00b_unicode_digit_value('0');
    ASSERT(n00b_option_is_set(d0));
    ASSERT_EQ(n00b_option_get(d0), 0);
    n00b_option_t(int32_t) d9 = n00b_unicode_digit_value('9');
    ASSERT(n00b_option_is_set(d9));
    ASSERT_EQ(n00b_option_get(d9), 9);
    ASSERT(!n00b_option_is_set(n00b_unicode_digit_value('A')));
    ASSERT(!n00b_option_is_set(n00b_unicode_digit_value(0x00BC))); // fraction, not a digit
}

TEST(test_numeric_superscript)
{
    // U+00B2 = SUPERSCRIPT TWO (Digit type)
    ASSERT_EQ(n00b_unicode_numeric_type(0x00B2), N00B_UNICODE_NUMERIC_DIGIT);
    n00b_unicode_numeric_value_t v = n00b_unicode_numeric_value(0x00B2);
    ASSERT_EQ(v.numerator, 2);
    ASSERT_EQ(v.denominator, 1);
    n00b_option_t(int32_t) db2 = n00b_unicode_digit_value(0x00B2);
    ASSERT(n00b_option_is_set(db2));
    ASSERT_EQ(n00b_option_get(db2), 2);
}

// --- Script extensions tests ---

TEST(test_script_extensions_basic)
{
    // Latin 'A' should have singleton {Latin}
    n00b_unicode_script_t ext[32];
    int count = n00b_unicode_script_extensions('A', ext, 32);
    ASSERT_EQ(count, 1);
    ASSERT_STR_EQ(n00b_unicode_script_name(ext[0]), "Latin");
}

TEST(test_script_extensions_middle_dot)
{
    // U+00B7 MIDDLE DOT has multiple script extensions
    n00b_unicode_script_t ext[32];
    int count = n00b_unicode_script_extensions(0x00B7, ext, 32);
    ASSERT(count > 1); // Should have many scripts including Latin
}

static void run_tests(void)
{
    RUN_TEST(test_general_category);
    RUN_TEST(test_combining_class);
    RUN_TEST(test_script);
    RUN_TEST(test_bidi_class);
    RUN_TEST(test_east_asian_width);
    RUN_TEST(test_char_width);
    RUN_TEST(test_binary_properties);
    RUN_TEST(test_ascii_changes_when_casefolded);
    RUN_TEST(test_ascii_table_only_properties);
    RUN_TEST(test_id_properties);
    RUN_TEST(test_display_width);
    RUN_TEST(test_joining_type);
    RUN_TEST(test_numeric_ascii_digits);
    RUN_TEST(test_numeric_arabic_digits);
    RUN_TEST(test_numeric_fractions);
    RUN_TEST(test_numeric_non_numeric);
    RUN_TEST(test_digit_value);
    RUN_TEST(test_numeric_superscript);
    RUN_TEST(test_script_extensions_basic);
    RUN_TEST(test_script_extensions_middle_dot);
}

TEST_MAIN()
