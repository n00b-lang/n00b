#include "test_unicode_helpers.h"
#include "strings/string_convert.h"
#include "strings/string_ops.h"
#include "unicode/encoding.h"

#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

// ===================================================================
// from_int
// ===================================================================

TEST(test_from_int_zero)
{
    n00b_string_t r = n00b_unicode_str_from_int(0, .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "0");
}

TEST(test_from_int_positive)
{
    n00b_string_t r = n00b_unicode_str_from_int(42, .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "42");
}

TEST(test_from_int_negative)
{
    n00b_string_t r = n00b_unicode_str_from_int(-123, .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "-123");
}

TEST(test_from_int_max)
{
    n00b_string_t r = n00b_unicode_str_from_int(INT64_MAX, .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "9223372036854775807");
}

TEST(test_from_int_min)
{
    n00b_string_t r = n00b_unicode_str_from_int(INT64_MIN, .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "-9223372036854775808");
}

// ===================================================================
// to_hex
// ===================================================================

TEST(test_to_hex_lower)
{
    n00b_string_t r = n00b_unicode_str_to_hex(*r"AB");
    ASSERT_STR_EQ(r.data, "4142");
}

TEST(test_to_hex_upper)
{
    n00b_string_t input = n00b_string_from_raw("\xff\x0a", 2);
    n00b_string_t r = n00b_unicode_str_to_hex(input, .upper = true);
    ASSERT_STR_EQ(r.data, "FF0A");
}

TEST(test_to_hex_empty)
{
    n00b_string_t r = n00b_unicode_str_to_hex(*r"");
    ASSERT_STR_EQ(r.data, "");
    ASSERT_EQ(r.u8_bytes, 0);
}

// ===================================================================
// to_cstr
// ===================================================================

TEST(test_to_cstr_basic)
{
    char *cs = n00b_unicode_str_to_cstr(*r"hello", .allocator = nullptr);
    ASSERT_STR_EQ(cs, "hello");
    n00b_free(cs);
}

TEST(test_to_cstr_empty)
{
    char *cs = n00b_unicode_str_to_cstr(*r"", .allocator = nullptr);
    ASSERT_STR_EQ(cs, "");
    n00b_free(cs);
}

// ===================================================================
// to_literal
// ===================================================================

TEST(test_to_literal)
{
    n00b_string_t r = n00b_unicode_str_to_literal(*r"hi", .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "\"hi\"");
}

TEST(test_to_literal_with_escapes)
{
    n00b_string_t r = n00b_unicode_str_to_literal(*r"a\nb", .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "\"a\\nb\"");
}

// ===================================================================
// from_codepoint
// ===================================================================

TEST(test_from_codepoint_ascii)
{
    n00b_string_t r = n00b_unicode_str_from_codepoint('A', .allocator = nullptr);
    ASSERT_STR_EQ(r.data, "A");
    ASSERT_EQ(r.codepoints, 1);
}

TEST(test_from_codepoint_bmp)
{
    // U+00E9 = é = C3 A9
    n00b_string_t r = n00b_unicode_str_from_codepoint(0x00E9, .allocator = nullptr);
    ASSERT_EQ(r.u8_bytes, 2);
    ASSERT_EQ(r.codepoints, 1);
    ASSERT((uint8_t)r.data[0] == 0xC3);
    ASSERT((uint8_t)r.data[1] == 0xA9);
}

TEST(test_from_codepoint_supplementary)
{
    // U+1F600 = 😀 = F0 9F 98 80
    n00b_string_t r = n00b_unicode_str_from_codepoint(0x1F600, .allocator = nullptr);
    ASSERT_EQ(r.u8_bytes, 4);
    ASSERT_EQ(r.codepoints, 1);
}

TEST(test_from_codepoint_invalid_surrogate)
{
    n00b_string_t r = n00b_unicode_str_from_codepoint(0xD800, .allocator = nullptr);
    ASSERT_EQ(r.u8_bytes, 0);
    ASSERT_EQ(r.codepoints, 0);
}

TEST(test_from_codepoint_invalid_too_large)
{
    n00b_string_t r = n00b_unicode_str_from_codepoint(0x110000, .allocator = nullptr);
    ASSERT_EQ(r.u8_bytes, 0);
    ASSERT_EQ(r.codepoints, 0);
}

// ===================================================================
// from_file
// ===================================================================

TEST(test_from_file_missing)
{
    auto r = n00b_unicode_str_from_file("/nonexistent_path_12345", .allocator = nullptr);
    ASSERT(n00b_result_is_err(r));
}

TEST(test_from_file_dev_null)
{
    const char *null_path =
#if defined(_WIN32)
        "NUL";
#else
        "/dev/null";
#endif
    auto r = n00b_unicode_str_from_file(null_path, .allocator = nullptr);
    ASSERT(n00b_result_is_ok(r));
    n00b_string_t s = n00b_result_get(r);
    ASSERT_EQ(s.u8_bytes, 0);
}

// ===================================================================
// make_cstr_array
// ===================================================================

TEST(test_make_cstr_array_basic)
{
    n00b_string_t raw[]               = {S("one"), S("two"), S("three")};
    n00b_array_t(n00b_string_t) parts = n00b_array_checked_ptr(n00b_string_t, 3, raw);
    parts.len                         = 3;
    n00b_array_t(n00b_cstr_t) arr = n00b_unicode_make_cstr_array(parts, .allocator = nullptr);
    ASSERT_EQ(arr.len, 3);
    ASSERT_STR_EQ(arr.data[0], "one");
    ASSERT_STR_EQ(arr.data[1], "two");
    ASSERT_STR_EQ(arr.data[2], "three");
    for (int i = 0; i < 3; i++)
        n00b_free(arr.data[i]);
}

TEST(test_make_cstr_array_empty)
{
    n00b_array_t(n00b_string_t) parts = {};
    n00b_array_t(n00b_cstr_t) arr = n00b_unicode_make_cstr_array(parts, .allocator = nullptr);
    ASSERT_EQ(arr.len, 0);
}

static void
run_tests(void)
{
    RUN_TEST(test_from_int_zero);
    RUN_TEST(test_from_int_positive);
    RUN_TEST(test_from_int_negative);
    RUN_TEST(test_from_int_max);
    RUN_TEST(test_from_int_min);
    RUN_TEST(test_to_hex_lower);
    RUN_TEST(test_to_hex_upper);
    RUN_TEST(test_to_hex_empty);
    RUN_TEST(test_to_cstr_basic);
    RUN_TEST(test_to_cstr_empty);
    RUN_TEST(test_to_literal);
    RUN_TEST(test_to_literal_with_escapes);
    RUN_TEST(test_from_codepoint_ascii);
    RUN_TEST(test_from_codepoint_bmp);
    RUN_TEST(test_from_codepoint_supplementary);
    RUN_TEST(test_from_codepoint_invalid_surrogate);
    RUN_TEST(test_from_codepoint_invalid_too_large);
    RUN_TEST(test_from_file_missing);
    RUN_TEST(test_from_file_dev_null);
    RUN_TEST(test_make_cstr_array_basic);
    RUN_TEST(test_make_cstr_array_empty);
}

TEST_MAIN()
