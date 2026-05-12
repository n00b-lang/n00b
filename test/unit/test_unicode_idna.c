#include "test_unicode_helpers.h"
#include "text/unicode/idna.h"
#include "text/unicode/encoding.h"

TEST(test_ascii_passthrough)
{
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(r"example.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "example.com");
}

TEST(test_unicode_domain)
{
    // münchen.de → xn--mnchen-3ya.de
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(r"m\xC3\xBCnchen.de", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT(r.value->data != nullptr);
    // Should start with xn-- for the first label
    ASSERT(strncmp(r.value->data, "xn--", 4) == 0);
}

TEST(test_to_unicode)
{
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(r"example.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "example.com");
}

// --- CONTEXTJ tests ---

TEST(test_zwj_after_virama)
{
    // Devanagari KA (U+0915) + Virama (U+094D, CCC=9) + ZWJ (U+200D) + Devanagari KA
    // This is a valid use of ZWJ after Virama
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"\xE0\xA4\x95\xE0\xA5\x8D\xE2\x80\x8D\xE0\xA4\x95.example.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT(r.value->u8_bytes > 0);
}

TEST(test_zwj_invalid)
{
    // ZWJ without preceding Virama → CONTEXTJ error
    // Latin 'a' + ZWJ (U+200D) + Latin 'b'
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"a\xE2\x80\x8Db.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_CONTEXTJ_ERROR);
}

TEST(test_zwnj_joining_context)
{
    // Arabic BEH (U+0628, Dual_Joining) + ZWNJ (U+200C) + Arabic BEH (U+0628, Dual_Joining)
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"\xD8\xA8\xE2\x80\x8C\xD8\xA8.example.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT(r.value->u8_bytes > 0);
}

TEST(test_zwnj_invalid)
{
    // ZWNJ in bad position → CONTEXTJ error
    // Latin 'a' + ZWNJ (U+200C) + Latin 'b'
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"a\xE2\x80\x8Cb.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_CONTEXTJ_ERROR);
}

TEST(test_middle_dot_valid)
{
    // "l·l" (U+006C + U+00B7 + U+006C) in a non-ASCII label
    // We need non-ASCII to trigger Punycode path — U+00B7 is non-ASCII
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"l\xC2\xB7l.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT(r.value->u8_bytes > 0);
}

TEST(test_middle_dot_invalid)
{
    // "a·b" → CONTEXTO error (not between two 'l's)
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"a\xC2\xB7b.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_CONTEXTO_ERROR);
}

TEST(test_arabic_digit_mix)
{
    // Arabic-Indic digit (U+0660) + Extended Arabic-Indic digit (U+06F0) → CONTEXTO error
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"\xD9\xA0\xDB\xB0.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_CONTEXTO_ERROR);
}

TEST(test_hebrew_geresh)
{
    // Hebrew letter (U+05D0 ALEF) + Geresh (U+05F3) → valid
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"\xD7\x90\xD7\xB3.com",
        .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT(r.value->u8_bytes > 0);
}

// --- ToUnicode (Punycode decode) tests ---

TEST(test_to_unicode_xn_label)
{
    // xn--mnchen-3ya.de → münchen.de (Punycode round-trip)
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(
        r"xn--mnchen-3ya.de", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "m\xC3\xBCnchen.de");
}

TEST(test_ascii_roundtrip)
{
    // ASCII domain → ToASCII → ToUnicode → original
    n00b_unicode_idna_result_t a = n00b_unicode_idna_to_ascii(
        r"example.com", .allocator = nullptr);
    ASSERT_EQ(a.error, N00B_UNICODE_IDNA_OK);
    n00b_unicode_idna_result_t u = n00b_unicode_idna_to_unicode(
        a.value, .allocator = nullptr);
    ASSERT_EQ(u.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(u.value->data, "example.com");
}

TEST(test_unicode_roundtrip)
{
    // Unicode domain → ToASCII → ToUnicode → original
    n00b_unicode_idna_result_t a = n00b_unicode_idna_to_ascii(
        r"m\xC3\xBCnchen.de", .allocator = nullptr);
    ASSERT_EQ(a.error, N00B_UNICODE_IDNA_OK);
    n00b_unicode_idna_result_t u = n00b_unicode_idna_to_unicode(
        a.value, .allocator = nullptr);
    ASSERT_EQ(u.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(u.value->data, "m\xC3\xBCnchen.de");
}

TEST(test_multi_label_decode)
{
    // xn--mnchen-3ya.example.com → münchen.example.com
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(
        r"xn--mnchen-3ya.example.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "m\xC3\xBCnchen.example.com");
}

TEST(test_invalid_punycode)
{
    // xn-- followed by characters that are not valid Punycode digits.
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(
        r"xn--!!.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_PUNYCODE_ERROR);
}

TEST(test_leading_hyphen)
{
    // Label starting with hyphen is invalid.
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"-bad.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_PROCESSING_ERROR);
}

TEST(test_trailing_hyphen)
{
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"bad-.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_PROCESSING_ERROR);
}

TEST(test_double_hyphen_pos_3_4)
{
    // Labels with hyphens at positions 3 and 4 are reserved for ACE.  A
    // non-xn-- label that already has the "--" at that position is rejected.
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_ascii(
        r"ab--cd.com", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_PROCESSING_ERROR);
}

TEST(test_uppercase_xn_prefix)
{
    // "XN--mnchen-3ya" must also decode (case-insensitive ACE prefix).
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(
        r"XN--mnchen-3ya.de", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "m\xC3\xBCnchen.de");
}

TEST(test_punycode_with_no_basic_chars)
{
    // "xn--g6w" decodes to U+6E2C (測, UTF-8 0xE6 0xB8 0xAC): an all-non-ASCII
    // label exercises the no-basic-codepoint branch of the Punycode decoder.
    n00b_unicode_idna_result_t r = n00b_unicode_idna_to_unicode(
        r"xn--g6w.example", .allocator = nullptr);
    ASSERT_EQ(r.error, N00B_UNICODE_IDNA_OK);
    ASSERT_STR_EQ(r.value->data, "\xE6\xB8\xAC.example");
}

static void run_tests(void)
{
    RUN_TEST(test_ascii_passthrough);
    RUN_TEST(test_unicode_domain);
    RUN_TEST(test_to_unicode);
    RUN_TEST(test_zwj_after_virama);
    RUN_TEST(test_zwj_invalid);
    RUN_TEST(test_zwnj_joining_context);
    RUN_TEST(test_zwnj_invalid);
    RUN_TEST(test_middle_dot_valid);
    RUN_TEST(test_middle_dot_invalid);
    RUN_TEST(test_arabic_digit_mix);
    RUN_TEST(test_hebrew_geresh);
    RUN_TEST(test_to_unicode_xn_label);
    RUN_TEST(test_ascii_roundtrip);
    RUN_TEST(test_unicode_roundtrip);
    RUN_TEST(test_multi_label_decode);
    RUN_TEST(test_invalid_punycode);
    RUN_TEST(test_leading_hyphen);
    RUN_TEST(test_trailing_hyphen);
    RUN_TEST(test_double_hyphen_pos_3_4);
    RUN_TEST(test_uppercase_xn_prefix);
    RUN_TEST(test_punycode_with_no_basic_chars);
}

TEST_MAIN()
