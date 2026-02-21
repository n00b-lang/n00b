#include "test_unicode_helpers.h"
#include "strings/format_spec.h"

// ===================================================================
// Spec parsing tests
// ===================================================================

TEST(test_parse_simple_d)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("d", 1);
    ASSERT_EQ(fs.type, 'd');
    ASSERT_EQ(fs.width, -1);
    ASSERT_EQ(fs.precision, -1);
    ASSERT(!fs.left_align);
    ASSERT(!fs.zero_pad);
    ASSERT(!fs.commas);
}

TEST(test_parse_comma_d)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(",d", 2);
    ASSERT_EQ(fs.type, 'd');
    ASSERT(fs.commas);
}

TEST(test_parse_width)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("10d", 3);
    ASSERT_EQ(fs.type, 'd');
    ASSERT_EQ(fs.width, 10);
}

TEST(test_parse_precision)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(".2f", 3);
    ASSERT_EQ(fs.type, 'f');
    ASSERT_EQ(fs.precision, 2);
}

TEST(test_parse_zero_pad)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("08d", 3);
    ASSERT_EQ(fs.type, 'd');
    ASSERT(fs.zero_pad);
    ASSERT_EQ(fs.width, 8);
}

TEST(test_parse_left_align)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("-20s", 4);
    ASSERT_EQ(fs.type, 's');
    ASSERT(fs.left_align);
    ASSERT_EQ(fs.width, 20);
}

TEST(test_parse_sign_plus)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("+d", 2);
    ASSERT_EQ(fs.type, 'd');
    ASSERT(fs.sign_plus);
}

TEST(test_parse_scientific)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(".3e", 3);
    ASSERT_EQ(fs.type, 'e');
    ASSERT_EQ(fs.precision, 3);
    ASSERT(!fs.upper);
}

TEST(test_parse_scientific_upper)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("E", 1);
    ASSERT_EQ(fs.type, 'e');
    ASSERT(fs.upper);
}

TEST(test_parse_hex_upper)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("X", 1);
    ASSERT_EQ(fs.type, 'x');
    ASSERT(fs.upper);
}

TEST(test_parse_bool_word)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("b", 1);
    ASSERT_EQ(fs.type, 'b');
    ASSERT(fs.word);
    ASSERT(!fs.upper);
}

TEST(test_parse_bool_letter)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("t", 1);
    ASSERT_EQ(fs.type, 'b');
    ASSERT(!fs.word);
    ASSERT(!fs.upper);
}

// ===================================================================
// Formatting tests
// ===================================================================

TEST(test_fmt_int_comma)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(",d", 2);
    n00b_string_t r       = n00b_str_fmt_int_ex(1234567, &fs);
    ASSERT_STR_EQ(r.data, "1,234,567");
}

TEST(test_fmt_int_zero_pad)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("08d", 3);
    n00b_string_t r       = n00b_str_fmt_int_ex(42, &fs);
    ASSERT_STR_EQ(r.data, "00000042");
}

TEST(test_fmt_int_left_align)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("-8d", 3);
    n00b_string_t r       = n00b_str_fmt_int_ex(42, &fs);
    // "42      " (42 + 6 spaces)
    ASSERT_EQ(r.codepoints, 8);
    ASSERT(r.data[0] == '4' && r.data[1] == '2');
    ASSERT(r.data[7] == ' ');
}

TEST(test_fmt_int_sign_plus)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("+d", 2);
    n00b_string_t r       = n00b_str_fmt_int_ex(42, &fs);
    ASSERT(r.data[0] == '+');
}

TEST(test_fmt_float_precision)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(".2f", 3);
    n00b_string_t r       = n00b_str_fmt_float_ex(3.14159, &fs);
    ASSERT_STR_EQ(r.data, "3.14");
}

TEST(test_fmt_float_scientific)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(".2e", 3);
    n00b_string_t r       = n00b_str_fmt_float_ex(12345.0, &fs);
    // Should contain 'e' and exponent.
    ASSERT(strchr(r.data, 'e') != nullptr || strchr(r.data, 'E') != nullptr);
}

TEST(test_fmt_string_width)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("10s", 3);
    n00b_string_t val     = *r"hi";
    n00b_string_t r       = n00b_str_fmt_string_ex(val, &fs);
    ASSERT_EQ(r.codepoints, 10);
}

TEST(test_fmt_string_truncate)
{
    n00b_format_spec_t fs = n00b_format_spec_parse(".3s", 3);
    n00b_string_t val     = *r"hello";
    n00b_string_t r       = n00b_str_fmt_string_ex(val, &fs);
    ASSERT_EQ(r.codepoints, 3);
}

TEST(test_fmt_hex)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("x", 1);
    n00b_string_t r       = n00b_str_fmt_int_ex(255, &fs);
    ASSERT_STR_EQ(r.data, "ff");
}

TEST(test_fmt_hex_upper)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("X", 1);
    n00b_string_t r       = n00b_str_fmt_int_ex(255, &fs);
    ASSERT_STR_EQ(r.data, "FF");
}

TEST(test_fmt_octal)
{
    n00b_format_spec_t fs = n00b_format_spec_parse("o", 1);
    n00b_string_t r       = n00b_str_fmt_int_ex(8, &fs);
    ASSERT_STR_EQ(r.data, "10");
}

// ===================================================================
// Runner
// ===================================================================

static void
run_tests(void)
{
    // Parsing
    RUN_TEST(test_parse_simple_d);
    RUN_TEST(test_parse_comma_d);
    RUN_TEST(test_parse_width);
    RUN_TEST(test_parse_precision);
    RUN_TEST(test_parse_zero_pad);
    RUN_TEST(test_parse_left_align);
    RUN_TEST(test_parse_sign_plus);
    RUN_TEST(test_parse_scientific);
    RUN_TEST(test_parse_scientific_upper);
    RUN_TEST(test_parse_hex_upper);
    RUN_TEST(test_parse_bool_word);
    RUN_TEST(test_parse_bool_letter);
    // Formatting
    RUN_TEST(test_fmt_int_comma);
    RUN_TEST(test_fmt_int_zero_pad);
    RUN_TEST(test_fmt_int_left_align);
    RUN_TEST(test_fmt_int_sign_plus);
    RUN_TEST(test_fmt_float_precision);
    RUN_TEST(test_fmt_float_scientific);
    RUN_TEST(test_fmt_string_width);
    RUN_TEST(test_fmt_string_truncate);
    RUN_TEST(test_fmt_hex);
    RUN_TEST(test_fmt_hex_upper);
    RUN_TEST(test_fmt_octal);
}

TEST_MAIN()
