#include "test_unicode_helpers.h"
#include "text/unicode/encoding.h"

// ===================================================================
// Encode / decode roundtrips
// ===================================================================

TEST(test_ascii_roundtrip)
{
    char     buf[4];
    uint32_t n = n00b_unicode_utf8_encode('A', buf);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(buf[0], 'A');

    uint32_t pos = 0;
    int32_t  cp  = n00b_unicode_utf8_decode(buf, n, &pos);
    ASSERT_EQ(cp, 'A');
    ASSERT_EQ(pos, 1);
}

TEST(test_2byte_roundtrip)
{
    char     buf[4];
    uint32_t n = n00b_unicode_utf8_encode(0x00E9, buf); // é
    ASSERT_EQ(n, 2);

    uint32_t pos = 0;
    int32_t  cp  = n00b_unicode_utf8_decode(buf, n, &pos);
    ASSERT_EQ(cp, 0x00E9);
    ASSERT_EQ(pos, 2);
}

TEST(test_3byte_roundtrip)
{
    char     buf[4];
    uint32_t n = n00b_unicode_utf8_encode(0x4E16, buf); // 世
    ASSERT_EQ(n, 3);

    uint32_t pos = 0;
    int32_t  cp  = n00b_unicode_utf8_decode(buf, n, &pos);
    ASSERT_EQ(cp, 0x4E16);
}

TEST(test_4byte_roundtrip)
{
    char     buf[4];
    uint32_t n = n00b_unicode_utf8_encode(0x1F600, buf); // 😀
    ASSERT_EQ(n, 4);

    uint32_t pos = 0;
    int32_t  cp  = n00b_unicode_utf8_decode(buf, n, &pos);
    ASSERT_EQ(cp, 0x1F600);
}

// ===================================================================
// Validation — basic
// ===================================================================

TEST(test_validate_valid)
{
    ASSERT(n00b_unicode_utf8_validate("Hello", 5));
    ASSERT(n00b_unicode_utf8_validate("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 9));
    ASSERT(n00b_unicode_utf8_validate("\xF0\x9F\x98\x80", 4)); // 😀
}

TEST(test_validate_invalid)
{
    ASSERT(!n00b_unicode_utf8_validate("\xFF\xFE", 2));
    ASSERT(!n00b_unicode_utf8_validate("\xC0\x80", 2));     // overlong NUL
    ASSERT(!n00b_unicode_utf8_validate("\xED\xA0\x80", 3)); // surrogate
}

// ===================================================================
// Validation — edge cases (exercises word-at-a-time and multibyte paths)
// ===================================================================

TEST(test_validate_empty)
{
    ASSERT(n00b_unicode_utf8_validate("", 0));
}

TEST(test_validate_all_ascii_short)
{
    ASSERT(n00b_unicode_utf8_validate("abc", 3));
}

TEST(test_validate_all_ascii_long)
{
    // Long enough to exercise the word-at-a-time fast path (>= 16 bytes).
    const char *text = "This is a longer ASCII string for testing the word path.";
    ASSERT(n00b_unicode_utf8_validate(text, (uint32_t)strlen(text)));
}

TEST(test_validate_mixed_ascii_multibyte)
{
    // Transitions between ASCII fast path and byte-level validation.
    const char *text = "Hello, \xE4\xB8\x96\xE7\x95\x8C! Caf\xC3\xA9 \xF0\x9F\x98\x80 end";
    ASSERT(n00b_unicode_utf8_validate(text, (uint32_t)strlen(text)));
}

TEST(test_validate_all_2byte)
{
    const char data[] = "\xC3\x80\xC3\x81\xC3\x82\xC3\x83";
    ASSERT(n00b_unicode_utf8_validate(data, sizeof(data) - 1));
}

TEST(test_validate_all_3byte)
{
    const char *text = "\xE4\xB8\x96\xE7\x95\x8C\xE4\xBA\xBA\xE6\xB0\x91";
    ASSERT(n00b_unicode_utf8_validate(text, (uint32_t)strlen(text)));
}

TEST(test_validate_all_4byte)
{
    const char *text = "\xF0\x9F\x98\x80\xF0\x9F\x98\x81\xF0\x9F\x98\x82";
    ASSERT(n00b_unicode_utf8_validate(text, (uint32_t)strlen(text)));
}

TEST(test_validate_truncated_2byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xC3", 1));
}

TEST(test_validate_truncated_3byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xE4\xB8", 2));
}

TEST(test_validate_truncated_4byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xF0\x9F\x98", 3));
}

TEST(test_validate_overlong_2byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xC0\x80", 2)); // Overlong NUL
    ASSERT(!n00b_unicode_utf8_validate("\xC1\xBF", 2)); // Overlong U+007F
}

TEST(test_validate_overlong_3byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xE0\x80\x80", 3)); // Overlong NUL
    ASSERT(!n00b_unicode_utf8_validate("\xE0\x9F\xBF", 3)); // Overlong U+07FF
}

TEST(test_validate_overlong_4byte)
{
    ASSERT(!n00b_unicode_utf8_validate("\xF0\x80\x80\x80", 4)); // Overlong
    ASSERT(!n00b_unicode_utf8_validate("\xF0\x8F\xBF\xBF", 4)); // Overlong U+FFFF
}

TEST(test_validate_surrogates)
{
    ASSERT(!n00b_unicode_utf8_validate("\xED\xA0\x80", 3)); // U+D800
    ASSERT(!n00b_unicode_utf8_validate("\xED\xAF\xBF", 3)); // U+DBFF
    ASSERT(!n00b_unicode_utf8_validate("\xED\xB0\x80", 3)); // U+DC00
    ASSERT(!n00b_unicode_utf8_validate("\xED\xBF\xBF", 3)); // U+DFFF
}

TEST(test_validate_too_high)
{
    ASSERT(!n00b_unicode_utf8_validate("\xF4\x90\x80\x80", 4)); // U+110000
    ASSERT(!n00b_unicode_utf8_validate("\xF5\x80\x80\x80", 4)); // F5 lead byte
    ASSERT(!n00b_unicode_utf8_validate("\xFE\x80\x80\x80", 4)); // FE lead byte
    ASSERT(!n00b_unicode_utf8_validate("\xFF\x80\x80\x80", 4)); // FF lead byte
}

TEST(test_validate_bad_continuation)
{
    ASSERT(!n00b_unicode_utf8_validate("\xC3\x00", 2));     // NUL as cont
    ASSERT(!n00b_unicode_utf8_validate("\xC3\xFF", 2));     // 0xFF as cont
    ASSERT(!n00b_unicode_utf8_validate("\xE4\xB8\x00", 3)); // 3-byte, bad 3rd
}

TEST(test_validate_boundary_codepoints)
{
    // U+007F (max 1-byte)
    ASSERT(n00b_unicode_utf8_validate("\x7F", 1));
    // U+0080 (min 2-byte)
    ASSERT(n00b_unicode_utf8_validate("\xC2\x80", 2));
    // U+07FF (max 2-byte)
    ASSERT(n00b_unicode_utf8_validate("\xDF\xBF", 2));
    // U+0800 (min 3-byte)
    ASSERT(n00b_unicode_utf8_validate("\xE0\xA0\x80", 3));
    // U+D7FF (just before surrogates)
    ASSERT(n00b_unicode_utf8_validate("\xED\x9F\xBF", 3));
    // U+E000 (just after surrogates)
    ASSERT(n00b_unicode_utf8_validate("\xEE\x80\x80", 3));
    // U+FFFF (max 3-byte)
    ASSERT(n00b_unicode_utf8_validate("\xEF\xBF\xBF", 3));
    // U+10000 (min 4-byte)
    ASSERT(n00b_unicode_utf8_validate("\xF0\x90\x80\x80", 4));
    // U+10FFFF (max valid codepoint)
    ASSERT(n00b_unicode_utf8_validate("\xF4\x8F\xBF\xBF", 4));
}

// ===================================================================
// Counting
// ===================================================================

TEST(test_count_empty)
{
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw("", 0), 0);
}

TEST(test_count_pure_ascii)
{
    const char *text = "Hello, world!";
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw(text, (uint32_t)strlen(text)), 13);
}

TEST(test_count_long_ascii)
{
    // Exercise word-at-a-time path.
    const char *text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnop";
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw(text, (uint32_t)strlen(text)),
              (int64_t)strlen(text));
}

TEST(test_count_mixed)
{
    // "Aé世😀" = 1 + 2 + 3 + 4 = 10 bytes, 4 codepoints
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw(
                  "A\xC3\xA9\xE4\xB8\x96\xF0\x9F\x98\x80", 10),
              4);
}

TEST(test_count_invalid_returns_negative)
{
    ASSERT(n00b_unicode_utf8_count_codepoints_raw("\xFF\xFE", 2) < 0);
    ASSERT(n00b_unicode_utf8_count_codepoints_raw("\xC0\x80", 2) < 0);
    ASSERT(n00b_unicode_utf8_count_codepoints_raw("\xED\xA0\x80", 3) < 0);
}

// ===================================================================
// String construction
// ===================================================================

TEST(test_str_from_cstr)
{
    n00b_string_t *s = r"Hello";
    ASSERT_EQ(s->u8_bytes, 5);
    ASSERT_EQ(s->codepoints, 5);

    n00b_string_t *s2 = r"\xE6\x97\xA5\xE6\x9C\xAC";
    ASSERT_EQ(s2->u8_bytes, 6);
    ASSERT_EQ(s2->codepoints, 2);
}

TEST(test_str_copy)
{
    n00b_string_t *s = r"test";
    n00b_string_t *c = n00b_string_from_raw(s->data, s->u8_bytes);
    ASSERT_EQ(c->u8_bytes, 4);
    ASSERT_STR_EQ(c->data, "test");
    ASSERT(c->data != s->data);
}

TEST(test_next_cp)
{
    n00b_string_t *s = r"A\xC3\xA9\xE4\xB8\x96";
    uint32_t      pos = 0;

    ASSERT_EQ(n00b_unicode_utf8_decode(s->data, (uint32_t)s->u8_bytes, &pos), 'A');
    ASSERT_EQ(n00b_unicode_utf8_decode(s->data, (uint32_t)s->u8_bytes, &pos), 0x00E9);
    ASSERT_EQ(n00b_unicode_utf8_decode(s->data, (uint32_t)s->u8_bytes, &pos), 0x4E16);
    ASSERT_EQ(n00b_unicode_utf8_decode(s->data, (uint32_t)s->u8_bytes, &pos), -1);
}

// ===================================================================
// UTF-16 conversion
// ===================================================================

TEST(test_utf16_roundtrip)
{
    n00b_string_t *s = r"Hello \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80";
    n00b_array_t(uint16_t) u16 = n00b_unicode_to_utf16(s);
    ASSERT(u16.data != nullptr);

    n00b_string_t *back = n00b_unicode_from_utf16(u16.data, (uint32_t)u16.len, .allocator = nullptr);
    ASSERT_EQ(back->u8_bytes, s->u8_bytes);
    ASSERT_STR_EQ(back->data, s->data);
}

TEST(test_utf16_all_bmp)
{
    n00b_string_t *s = r"ABC\xC3\xA9\xE4\xB8\x96";
    n00b_array_t(uint16_t) u16 = n00b_unicode_to_utf16(s);
    ASSERT(u16.data != nullptr);
    ASSERT_EQ(u16.len, 5); // A, B, C, e-acute, CJK
    ASSERT_EQ(u16.data[0], 'A');
    ASSERT_EQ(u16.data[1], 'B');
    ASSERT_EQ(u16.data[2], 'C');
    ASSERT_EQ(u16.data[3], 0x00E9);
    ASSERT_EQ(u16.data[4], 0x4E16);
}

TEST(test_utf16_surrogate_pairs)
{
    // U+1F600 = surrogate pair D83D DE00
    n00b_string_t *s = r"\xF0\x9F\x98\x80";
    n00b_array_t(uint16_t) u16 = n00b_unicode_to_utf16(s);
    ASSERT(u16.data != nullptr);
    ASSERT_EQ(u16.len, 2);
    ASSERT_EQ(u16.data[0], 0xD83D);
    ASSERT_EQ(u16.data[1], 0xDE00);
}

TEST(test_utf16_empty)
{
    n00b_string_t *s = r"";
    n00b_array_t(uint16_t) u16 = n00b_unicode_to_utf16(s);
    ASSERT(u16.data != nullptr);
    ASSERT_EQ(u16.len, 0);
}

// ===================================================================
// UTF-32 conversion
// ===================================================================

TEST(test_utf32_roundtrip)
{
    n00b_string_t *s = r"Test \xE6\x97\xA5\xE6\x9C\xAC";
    n00b_array_t(n00b_codepoint_t) u32 = n00b_unicode_to_utf32(s);
    ASSERT(u32.data != nullptr);
    ASSERT_EQ(u32.len, s->codepoints);

    n00b_string_t *back = n00b_unicode_from_utf32(u32.data, (uint32_t)u32.len, .allocator = nullptr);
    ASSERT_STR_EQ(back->data, s->data);
}

TEST(test_utf32_count_matches_codepoints)
{
    // "Aé世😀" = 4 codepoints
    n00b_string_t *s = r"A\xC3\xA9\xE4\xB8\x96\xF0\x9F\x98\x80";
    n00b_array_t(n00b_codepoint_t) u32 = n00b_unicode_to_utf32(s);
    ASSERT(u32.data != nullptr);
    ASSERT_EQ(u32.len, 4);
    ASSERT_EQ(u32.data[0], 'A');
    ASSERT_EQ(u32.data[1], 0x00E9);
    ASSERT_EQ(u32.data[2], 0x4E16);
    ASSERT_EQ(u32.data[3], 0x1F600);
}

TEST(test_utf32_empty)
{
    n00b_string_t *s = r"";
    n00b_array_t(n00b_codepoint_t) u32 = n00b_unicode_to_utf32(s);
    ASSERT(u32.data != nullptr);
    ASSERT_EQ(u32.len, 0);
}

// ===================================================================
// BOM detection
// ===================================================================

TEST(test_bom_detection)
{
    uint32_t bom_len;

    ASSERT_EQ(n00b_unicode_detect_bom("\xEF\xBB\xBF"
                                 "test",
                                 7,
                                 &bom_len),
              N00B_UNICODE_BOM_UTF8);
    ASSERT_EQ(bom_len, 3);

    ASSERT_EQ(n00b_unicode_detect_bom("\xFF\xFE"
                                 "ab",
                                 4,
                                 &bom_len),
              N00B_UNICODE_BOM_UTF16_LE);
    ASSERT_EQ(bom_len, 2);

    ASSERT_EQ(n00b_unicode_detect_bom("\xFE\xFF"
                                 "ab",
                                 4,
                                 &bom_len),
              N00B_UNICODE_BOM_UTF16_BE);
    ASSERT_EQ(bom_len, 2);

    ASSERT_EQ(n00b_unicode_detect_bom("test", 4, &bom_len), N00B_UNICODE_BOM_NONE);
    ASSERT_EQ(bom_len, 0);
}

// ===================================================================
// Large-buffer tests (exercise SIMD paths when available)
// ===================================================================

TEST(test_validate_large_ascii)
{
    char big[4096];
    memset(big, 'x', sizeof(big));
    ASSERT(n00b_unicode_utf8_validate(big, sizeof(big)));
}

TEST(test_validate_large_invalid)
{
    char big[4096];
    memset(big, 'x', sizeof(big));
    big[4090] = (char)0xFF;
    ASSERT(!n00b_unicode_utf8_validate(big, sizeof(big)));
}

TEST(test_validate_large_mixed)
{
    // ASCII with a 3-byte CJK sequence in the middle.
    char big[4096];
    memset(big, 'A', sizeof(big));
    big[2000] = '\xE4'; big[2001] = '\xB8'; big[2002] = '\x96';
    ASSERT(n00b_unicode_utf8_validate(big, sizeof(big)));
}

TEST(test_count_large_ascii)
{
    char big[4096];
    memset(big, 'B', sizeof(big));
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw(big, sizeof(big)), 4096);
}

TEST(test_count_large_mixed)
{
    // 4096 bytes, mostly ASCII, one 3-byte CJK -> 4094 codepoints.
    char big[4096];
    memset(big, 'B', sizeof(big));
    big[100] = '\xE4'; big[101] = '\xB8'; big[102] = '\x96';
    ASSERT_EQ(n00b_unicode_utf8_count_codepoints_raw(big, sizeof(big)), 4094);
}

TEST(test_validate_large_trailing_multibyte)
{
    // Multi-byte sequence right at the SIMD/scalar boundary.
    char big[4096];
    memset(big, 'A', sizeof(big));
    // Place 4-byte emoji at the very end.
    big[4092] = '\xF0'; big[4093] = '\x9F';
    big[4094] = '\x98'; big[4095] = '\x80';
    ASSERT(n00b_unicode_utf8_validate(big, sizeof(big)));
}

TEST(test_utf32_large_trusted)
{
    // Verify UTF-32 conversion uses the trusted path on large valid input.
    char big[256];
    memset(big, 'Z', sizeof(big));
    n00b_string_t *s = n00b_string_from_raw(big, sizeof(big));
    n00b_array_t(n00b_codepoint_t) u32 = n00b_unicode_to_utf32(s);
    ASSERT(u32.data != nullptr);
    ASSERT_EQ(u32.len, 256);
    ASSERT_EQ(u32.data[0], 'Z');
    ASSERT_EQ(u32.data[255], 'Z');
}

// ===================================================================
// Test runner
// ===================================================================

static void
run_tests(void)
{
    // Encode / decode roundtrips
    RUN_TEST(test_ascii_roundtrip);
    RUN_TEST(test_2byte_roundtrip);
    RUN_TEST(test_3byte_roundtrip);
    RUN_TEST(test_4byte_roundtrip);

    // Validation — basic
    RUN_TEST(test_validate_valid);
    RUN_TEST(test_validate_invalid);

    // Validation — edge cases
    RUN_TEST(test_validate_empty);
    RUN_TEST(test_validate_all_ascii_short);
    RUN_TEST(test_validate_all_ascii_long);
    RUN_TEST(test_validate_mixed_ascii_multibyte);
    RUN_TEST(test_validate_all_2byte);
    RUN_TEST(test_validate_all_3byte);
    RUN_TEST(test_validate_all_4byte);
    RUN_TEST(test_validate_truncated_2byte);
    RUN_TEST(test_validate_truncated_3byte);
    RUN_TEST(test_validate_truncated_4byte);
    RUN_TEST(test_validate_overlong_2byte);
    RUN_TEST(test_validate_overlong_3byte);
    RUN_TEST(test_validate_overlong_4byte);
    RUN_TEST(test_validate_surrogates);
    RUN_TEST(test_validate_too_high);
    RUN_TEST(test_validate_bad_continuation);
    RUN_TEST(test_validate_boundary_codepoints);

    // Counting
    RUN_TEST(test_count_empty);
    RUN_TEST(test_count_pure_ascii);
    RUN_TEST(test_count_long_ascii);
    RUN_TEST(test_count_mixed);
    RUN_TEST(test_count_invalid_returns_negative);

    // String construction
    RUN_TEST(test_str_from_cstr);
    RUN_TEST(test_str_copy);
    RUN_TEST(test_next_cp);

    // UTF-16
    RUN_TEST(test_utf16_roundtrip);
    RUN_TEST(test_utf16_all_bmp);
    RUN_TEST(test_utf16_surrogate_pairs);
    RUN_TEST(test_utf16_empty);

    // UTF-32
    RUN_TEST(test_utf32_roundtrip);
    RUN_TEST(test_utf32_count_matches_codepoints);
    RUN_TEST(test_utf32_empty);

    // BOM
    RUN_TEST(test_bom_detection);

    // Large-buffer / SIMD
    RUN_TEST(test_validate_large_ascii);
    RUN_TEST(test_validate_large_invalid);
    RUN_TEST(test_validate_large_mixed);
    RUN_TEST(test_count_large_ascii);
    RUN_TEST(test_count_large_mixed);
    RUN_TEST(test_validate_large_trailing_multibyte);
    RUN_TEST(test_utf32_large_trusted);
}

TEST_MAIN()
