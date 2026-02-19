#include "test_unicode_helpers.h"
#include "unicode/encoding.h"

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

TEST(test_validate_valid)
{
    ASSERT(n00b_unicode_utf8_validate("Hello", 5));
    ASSERT(n00b_unicode_utf8_validate("日本語", 9));
    ASSERT(n00b_unicode_utf8_validate("\xF0\x9F\x98\x80", 4)); // 😀
}

TEST(test_validate_invalid)
{
    ASSERT(!n00b_unicode_utf8_validate("\xFF\xFE", 2));
    ASSERT(!n00b_unicode_utf8_validate("\xC0\x80", 2));     // overlong NUL
    ASSERT(!n00b_unicode_utf8_validate("\xED\xA0\x80", 3)); // surrogate
}

TEST(test_str_from_cstr)
{
    n00b_string_t s = STR("Hello");
    ASSERT_EQ(s.u8_bytes, 5);
    ASSERT_EQ(s.codepoints, 5);

    n00b_string_t s2 = STR("日本");
    ASSERT_EQ(s2.u8_bytes, 6);
    ASSERT_EQ(s2.codepoints, 2);
}

TEST(test_str_copy)
{
    n00b_string_t s = STR("test");
    n00b_string_t c = n00b_string_from_raw(NULL, s.data, s.u8_bytes, s.codepoints);
    ASSERT_EQ(c.u8_bytes, 4);
    ASSERT_STR_EQ(c.data, "test");
    ASSERT(c.data != s.data);
}

TEST(test_next_cp)
{
    n00b_string_t s   = STR("Aé世");
    uint32_t   pos = 0;

    ASSERT_EQ(n00b_unicode_utf8_decode(s.data, (uint32_t)s.u8_bytes, &pos), 'A');
    ASSERT_EQ(n00b_unicode_utf8_decode(s.data, (uint32_t)s.u8_bytes, &pos), 0x00E9);
    ASSERT_EQ(n00b_unicode_utf8_decode(s.data, (uint32_t)s.u8_bytes, &pos), 0x4E16);
    ASSERT_EQ(n00b_unicode_utf8_decode(s.data, (uint32_t)s.u8_bytes, &pos), -1);
}

TEST(test_utf16_roundtrip)
{
    n00b_string_t s = STR("Hello 世界 \xF0\x9F\x98\x80");
    uint32_t   u16_len;
    uint16_t  *u16 = n00b_unicode_to_utf16(s, &u16_len);
    ASSERT(u16 != NULL);

    n00b_string_t back = n00b_unicode_from_utf16(u16, u16_len, .allocator = NULL);
    ASSERT_EQ(back.u8_bytes, s.u8_bytes);
    ASSERT_STR_EQ(back.data, s.data);

    n00b_free(u16);
}

TEST(test_utf32_roundtrip)
{
    n00b_string_t s = STR("Test 日本");
    uint32_t   u32_len;
    uint32_t  *u32 = n00b_unicode_to_utf32(s, &u32_len);
    ASSERT(u32 != NULL);
    ASSERT_EQ(u32_len, s.codepoints);

    n00b_string_t back = n00b_unicode_from_utf32(u32, u32_len, .allocator = NULL);
    ASSERT_STR_EQ(back.data, s.data);

    n00b_free(u32);
}

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

static void
run_tests(void)
{
    RUN_TEST(test_ascii_roundtrip);
    RUN_TEST(test_2byte_roundtrip);
    RUN_TEST(test_3byte_roundtrip);
    RUN_TEST(test_4byte_roundtrip);
    RUN_TEST(test_validate_valid);
    RUN_TEST(test_validate_invalid);
    RUN_TEST(test_str_from_cstr);
    RUN_TEST(test_str_copy);
    RUN_TEST(test_next_cp);
    RUN_TEST(test_utf16_roundtrip);
    RUN_TEST(test_utf32_roundtrip);
    RUN_TEST(test_bom_detection);
}

TEST_MAIN()
