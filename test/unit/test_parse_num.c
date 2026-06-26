// Unit tests for the libc-free numeric parsers in util/parse_num.c, with a
// focus on n00b_parse_i64_base_span (the base-aware integer parser added to
// replace strtol/strtoll base-0/base-16 calls on off-libc worker threads).
//
// Harness: test/unit/test_unicode_helpers.h (TEST / ASSERT / ASSERT_EQ /
// run_tests / TEST_MAIN). String literals are plain C "..." here (test scaffolding),
// not n00b rich literals.

#include "test_unicode_helpers.h"
#include "util/parse_num.h"

#include <limits.h>

// Convenience: assert a base-parse of a NUL-terminated literal succeeds with an
// expected value. `lit` is a string literal so sizeof-1 is its byte length.
#define ASSERT_BASE_OK(lit, base, expect)                                      \
    do {                                                                       \
        n00b_result_t(int64_t) _r = n00b_parse_i64_base_span((lit),            \
                                                             sizeof(lit) - 1,  \
                                                             (base));          \
        ASSERT(n00b_result_is_ok(_r));                                         \
        ASSERT_EQ(n00b_result_get(_r), (int64_t)(expect));                     \
    } while (0)

#define ASSERT_BASE_ERR(lit, base)                                             \
    do {                                                                       \
        n00b_result_t(int64_t) _r = n00b_parse_i64_base_span((lit),            \
                                                             sizeof(lit) - 1,  \
                                                             (base));          \
        ASSERT(n00b_result_is_err(_r));                                        \
    } while (0)

// ===================================================================
// n00b_parse_i64_base_span — base 10
// ===================================================================

TEST(base10_simple) { ASSERT_BASE_OK("42", 10, 42); }
TEST(base10_negative) { ASSERT_BASE_OK("-42", 10, -42); }
TEST(base10_plus_sign) { ASSERT_BASE_OK("+7", 10, 7); }
TEST(base10_zero) { ASSERT_BASE_OK("0", 10, 0); }
TEST(base10_leading_ws) { ASSERT_BASE_OK("   123", 10, 123); }
TEST(base10_stops_at_nondigit) { ASSERT_BASE_OK("12abc", 10, 12); }
TEST(base10_int64_max) { ASSERT_BASE_OK("9223372036854775807", 10, INT64_MAX); }
TEST(base10_int64_min) { ASSERT_BASE_OK("-9223372036854775808", 10, INT64_MIN); }

// ===================================================================
// base 16 (with and without an 0x prefix)
// ===================================================================

TEST(base16_bare) { ASSERT_BASE_OK("ff", 16, 255); }
TEST(base16_upper) { ASSERT_BASE_OK("1A", 16, 26); }
TEST(base16_prefix_lower) { ASSERT_BASE_OK("0xff", 16, 255); }
TEST(base16_prefix_upper) { ASSERT_BASE_OK("0XFF", 16, 255); }
TEST(base16_negative) { ASSERT_BASE_OK("-ff", 16, -255); }
TEST(base16_stops) { ASSERT_BASE_OK("dead!", 16, 0xdead); }

// ===================================================================
// base 0 — strtol auto-detection: 0x -> 16, leading 0 -> 8, else 10
// ===================================================================

TEST(base0_hex) { ASSERT_BASE_OK("0x1f", 0, 31); }
TEST(base0_hex_upper) { ASSERT_BASE_OK("0X10", 0, 16); }
TEST(base0_octal) { ASSERT_BASE_OK("010", 0, 8); }
TEST(base0_octal_value) { ASSERT_BASE_OK("0123", 0, 83); }
TEST(base0_decimal) { ASSERT_BASE_OK("42", 0, 42); }
TEST(base0_zero) { ASSERT_BASE_OK("0", 0, 0); }
TEST(base0_neg_hex) { ASSERT_BASE_OK("-0x10", 0, -16); }

// ===================================================================
// other bases
// ===================================================================

TEST(base8_plain) { ASSERT_BASE_OK("17", 8, 15); }
TEST(base2_binary) { ASSERT_BASE_OK("101", 2, 5); }
TEST(base36_max_digit) { ASSERT_BASE_OK("z", 36, 35); }
TEST(base16_digit_out_of_range_stops)
{
    // 'g' is not a base-16 digit -> parse stops after "ab".
    ASSERT_BASE_OK("abg", 16, 0xab);
}

// ===================================================================
// error paths
// ===================================================================

TEST(err_empty) { ASSERT_BASE_ERR("", 10); }
TEST(err_no_digits) { ASSERT_BASE_ERR("xyz", 10); }
TEST(err_sign_only) { ASSERT_BASE_ERR("-", 10); }
TEST(err_bad_base_low) { ASSERT_BASE_ERR("10", 1); }
TEST(err_bad_base_high) { ASSERT_BASE_ERR("10", 37); }
TEST(err_overflow)
{
    // 20 nines > INT64_MAX (9.22e18) -> OVERFLOW.
    ASSERT_BASE_ERR("99999999999999999999", 10);
}
TEST(err_hex_prefix_no_digits)
{
    // base 16 "0x" with no hex digit after the prefix -> NO_DIGITS.
    ASSERT_BASE_ERR("0x", 16);
}

// ===================================================================
// sibling base-10 span/float parsers (basic coverage — no prior tests existed)
// ===================================================================

TEST(i64_span_basic)
{
    n00b_result_t(int64_t) r = n00b_parse_i64_span("2025", 4);
    ASSERT(n00b_result_is_ok(r));
    ASSERT_EQ(n00b_result_get(r), 2025);
}

TEST(f64_span_basic)
{
    n00b_result_t(double) r = n00b_parse_f64_span("3.5", 3);
    ASSERT(n00b_result_is_ok(r));
    ASSERT(n00b_result_get(r) == 3.5);
}

TEST(f64_span_negative_exp)
{
    n00b_result_t(double) r = n00b_parse_f64_span("-1.5e2", 6);
    ASSERT(n00b_result_is_ok(r));
    ASSERT(n00b_result_get(r) == -150.0);
}

static void
run_tests(void)
{
    RUN_TEST(base10_simple);
    RUN_TEST(base10_negative);
    RUN_TEST(base10_plus_sign);
    RUN_TEST(base10_zero);
    RUN_TEST(base10_leading_ws);
    RUN_TEST(base10_stops_at_nondigit);
    RUN_TEST(base10_int64_max);
    RUN_TEST(base10_int64_min);
    RUN_TEST(base16_bare);
    RUN_TEST(base16_upper);
    RUN_TEST(base16_prefix_lower);
    RUN_TEST(base16_prefix_upper);
    RUN_TEST(base16_negative);
    RUN_TEST(base16_stops);
    RUN_TEST(base0_hex);
    RUN_TEST(base0_hex_upper);
    RUN_TEST(base0_octal);
    RUN_TEST(base0_octal_value);
    RUN_TEST(base0_decimal);
    RUN_TEST(base0_zero);
    RUN_TEST(base0_neg_hex);
    RUN_TEST(base8_plain);
    RUN_TEST(base2_binary);
    RUN_TEST(base36_max_digit);
    RUN_TEST(base16_digit_out_of_range_stops);
    RUN_TEST(err_empty);
    RUN_TEST(err_no_digits);
    RUN_TEST(err_sign_only);
    RUN_TEST(err_bad_base_low);
    RUN_TEST(err_bad_base_high);
    RUN_TEST(err_overflow);
    RUN_TEST(err_hex_prefix_no_digits);
    RUN_TEST(i64_span_basic);
    RUN_TEST(f64_span_basic);
    RUN_TEST(f64_span_negative_exp);
}

TEST_MAIN()
