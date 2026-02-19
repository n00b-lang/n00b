#include "test_unicode_helpers.h"
#include "strings/fmt_numbers.h"
#include "unicode/properties.h"

#include <math.h>
#include <limits.h>

#define S(lit) STR(lit)

// ===================================================================
// fmt_hex
// ===================================================================

TEST(test_hex_zero)
{
    n00b_string_t r = n00b_fmt_hex(0, false);
    ASSERT_STR_EQ(r.data, "0");
}

TEST(test_hex_lower)
{
    n00b_string_t r = n00b_fmt_hex(0xdead, false);
    ASSERT_STR_EQ(r.data, "dead");
}

TEST(test_hex_upper)
{
    n00b_string_t r = n00b_fmt_hex(0xdead, true);
    ASSERT_STR_EQ(r.data, "DEAD");
}

TEST(test_hex_max)
{
    n00b_string_t r = n00b_fmt_hex(UINT64_MAX, false);
    ASSERT_STR_EQ(r.data, "ffffffffffffffff");
}

// ===================================================================
// fmt_int
// ===================================================================

TEST(test_int_zero)
{
    n00b_string_t r = n00b_fmt_int(0, false);
    ASSERT_STR_EQ(r.data, "0");
}

TEST(test_int_positive)
{
    n00b_string_t r = n00b_fmt_int(12345, false);
    ASSERT_STR_EQ(r.data, "12345");
}

TEST(test_int_negative)
{
    n00b_string_t r = n00b_fmt_int(-42, false);
    ASSERT_STR_EQ(r.data, "-42");
}

TEST(test_int_commas)
{
    n00b_string_t r = n00b_fmt_int(1234567, true);
    ASSERT_STR_EQ(r.data, "1,234,567");
}

TEST(test_int_commas_negative)
{
    n00b_string_t r = n00b_fmt_int(-1234567, true);
    ASSERT_STR_EQ(r.data, "-1,234,567");
}

TEST(test_int_min)
{
    n00b_string_t r = n00b_fmt_int(INT64_MIN, false);
    ASSERT_STR_EQ(r.data, "-9223372036854775808");
}

// ===================================================================
// fmt_uint
// ===================================================================

TEST(test_uint_zero)
{
    n00b_string_t r = n00b_fmt_uint(0, false);
    ASSERT_STR_EQ(r.data, "0");
}

TEST(test_uint_large)
{
    n00b_string_t r = n00b_fmt_uint(18446744073709551615ULL, true);
    ASSERT_STR_EQ(r.data, "18,446,744,073,709,551,615");
}

// ===================================================================
// fmt_float
// ===================================================================

TEST(test_float_zero)
{
    n00b_string_t r = n00b_fmt_float(0.0, 0, false);
    ASSERT_STR_EQ(r.data, "0");
}

TEST(test_float_pi)
{
    n00b_string_t r = n00b_fmt_float(3.14, 0, false);
    ASSERT_STR_EQ(r.data, "3.14");
}

TEST(test_float_nan)
{
    n00b_string_t r = n00b_fmt_float(NAN, 0, false);
    // NAN may carry sign bit
    ASSERT(strcmp(r.data, "nan") == 0 || strcmp(r.data, "-nan") == 0);
}

TEST(test_float_inf)
{
    n00b_string_t r = n00b_fmt_float(INFINITY, 0, false);
    ASSERT_STR_EQ(r.data, "inf");
}

TEST(test_float_width_fill)
{
    n00b_string_t r = n00b_fmt_float(1.5, 8, true);
    // Should be "000001.5"
    ASSERT_EQ(r.u8_bytes, 8);
    ASSERT(r.data[0] == '0');
}

// ===================================================================
// fmt_bool
// ===================================================================

TEST(test_bool_all_combos)
{
    // value=false, upper=false, word=false, yn=false → "f"
    ASSERT_STR_EQ(n00b_fmt_bool(false, false, false, false).data, "f");
    // value=true, upper=false, word=false, yn=false → "t"
    ASSERT_STR_EQ(n00b_fmt_bool(true, false, false, false).data, "t");
    // value=false, upper=true, word=false, yn=false → "F"
    ASSERT_STR_EQ(n00b_fmt_bool(false, true, false, false).data, "F");
    // value=true, upper=true, word=false, yn=false → "T"
    ASSERT_STR_EQ(n00b_fmt_bool(true, true, false, false).data, "T");
    // value=false, upper=false, word=true, yn=false → "false"
    ASSERT_STR_EQ(n00b_fmt_bool(false, false, true, false).data, "false");
    // value=true, upper=false, word=true, yn=false → "true"
    ASSERT_STR_EQ(n00b_fmt_bool(true, false, true, false).data, "true");
    // value=false, upper=true, word=true, yn=false → "False"
    ASSERT_STR_EQ(n00b_fmt_bool(false, true, true, false).data, "False");
    // value=true, upper=true, word=true, yn=false → "True"
    ASSERT_STR_EQ(n00b_fmt_bool(true, true, true, false).data, "True");
    // yn variants
    ASSERT_STR_EQ(n00b_fmt_bool(false, false, false, true).data, "n");
    ASSERT_STR_EQ(n00b_fmt_bool(true, false, false, true).data, "y");
    ASSERT_STR_EQ(n00b_fmt_bool(false, true, false, true).data, "N");
    ASSERT_STR_EQ(n00b_fmt_bool(true, true, false, true).data, "Y");
    ASSERT_STR_EQ(n00b_fmt_bool(false, false, true, true).data, "no");
    ASSERT_STR_EQ(n00b_fmt_bool(true, false, true, true).data, "yes");
    ASSERT_STR_EQ(n00b_fmt_bool(false, true, true, true).data, "No");
    ASSERT_STR_EQ(n00b_fmt_bool(true, true, true, true).data, "Yes");
}

// ===================================================================
// fmt_codepoint
// ===================================================================

TEST(test_codepoint_ascii)
{
    n00b_string_t r = n00b_fmt_codepoint('A');
    ASSERT_STR_EQ(r.data, "A");
}

TEST(test_codepoint_control)
{
    // NUL is a control character → U+0000
    n00b_string_t r = n00b_fmt_codepoint(0);
    ASSERT(r.data[0] == 'U');
    ASSERT(r.data[1] == '+');
}

TEST(test_codepoint_emoji)
{
    // U+1F600 (grinning face) is printable → UTF-8 encoding
    n00b_string_t r = n00b_fmt_codepoint(0x1F600);
    ASSERT_EQ(r.u8_bytes, 4);
    ASSERT_EQ(r.codepoints, 1);
}

TEST(test_codepoint_invalid)
{
    // > U+10FFFF → replaced with U+FFFD (also a control-like category)
    n00b_string_t r = n00b_fmt_codepoint(0x200000);
    // FFFD is in "So" (Symbol, other) — so it's printable.
    // Just verify it doesn't crash and returns something.
    ASSERT(r.u8_bytes > 0);
}

// ===================================================================
// fmt_pointer
// ===================================================================

TEST(test_pointer_null)
{
    n00b_string_t r = n00b_fmt_pointer(nullptr, false);
    ASSERT_STR_EQ(r.data, "@0000000000000000");
}

TEST(test_pointer_nonnull)
{
    n00b_string_t r = n00b_fmt_pointer((void *)0xDEADBEEF, true);
    // Should be "@00000000DEADBEEF"
    ASSERT(r.data[0] == '@');
    ASSERT_EQ(r.u8_bytes, 17);
}

TEST(test_pointer_caps)
{
    n00b_string_t r = n00b_fmt_pointer((void *)0xabc, true);
    ASSERT(r.data[0] == '@');
    // Check that hex digits are uppercase
    ASSERT(r.data[16] == 'C');
}

static void run_tests(void)
{
    RUN_TEST(test_hex_zero);
    RUN_TEST(test_hex_lower);
    RUN_TEST(test_hex_upper);
    RUN_TEST(test_hex_max);
    RUN_TEST(test_int_zero);
    RUN_TEST(test_int_positive);
    RUN_TEST(test_int_negative);
    RUN_TEST(test_int_commas);
    RUN_TEST(test_int_commas_negative);
    RUN_TEST(test_int_min);
    RUN_TEST(test_uint_zero);
    RUN_TEST(test_uint_large);
    RUN_TEST(test_float_zero);
    RUN_TEST(test_float_pi);
    RUN_TEST(test_float_nan);
    RUN_TEST(test_float_inf);
    RUN_TEST(test_float_width_fill);
    RUN_TEST(test_bool_all_combos);
    RUN_TEST(test_codepoint_ascii);
    RUN_TEST(test_codepoint_control);
    RUN_TEST(test_codepoint_emoji);
    RUN_TEST(test_codepoint_invalid);
    RUN_TEST(test_pointer_null);
    RUN_TEST(test_pointer_nonnull);
    RUN_TEST(test_pointer_caps);
}

TEST_MAIN()
