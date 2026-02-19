#include "test_unicode_helpers.h"
#include "unicode/normalization.h"
#include "unicode/encoding.h"

// ---------------------------------------------------------------------------
// Manual unit tests
// ---------------------------------------------------------------------------

TEST(test_nfc_composed)
{
    n00b_string_t nfc = n00b_unicode_nfc(STR("e\xCC\x81"), .allocator = nullptr);
    ASSERT_EQ(nfc.codepoints, 1);
    ASSERT_STR_EQ(nfc.data, "\xC3\xA9");
}

TEST(test_nfd_decomposed)
{
    n00b_string_t nfd = n00b_unicode_nfd(STR("\xC3\xA9"), .allocator = nullptr);
    ASSERT_EQ(nfd.codepoints, 2);
    uint32_t pos = 0;
    int32_t  cp1 = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    int32_t  cp2 = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    ASSERT_EQ(cp1, 'e');
    ASSERT_EQ(cp2, 0x0301);
}

TEST(test_nfc_already_composed)
{
    n00b_string_t nfc = n00b_unicode_nfc(STR("Hello"), .allocator = nullptr);
    ASSERT_STR_EQ(nfc.data, "Hello");
}

TEST(test_nfkc_compat)
{
    n00b_string_t nfkc
        = n00b_unicode_nfkc(STR("\xEF\xAC\x81"), .allocator = nullptr); // ﬁ ligature
    ASSERT_STR_EQ(nfkc.data, "fi");
}

TEST(test_nfkd_compat)
{
    n00b_string_t nfkd = n00b_unicode_nfkd(STR("\xEF\xAC\x81"), .allocator = nullptr);
    ASSERT_STR_EQ(nfkd.data, "fi");
}

TEST(test_hangul_decompose_compose)
{
    n00b_string_t nfd = n00b_unicode_nfd(STR("\xED\x95\x9C"), .allocator = nullptr);
    ASSERT_EQ(nfd.codepoints, 3);

    n00b_string_t nfc = n00b_unicode_nfc(nfd, .allocator = nullptr);
    ASSERT_STR_EQ(nfc.data, "\xED\x95\x9C");
    ASSERT_EQ(nfc.codepoints, 1);
}

TEST(test_is_nfc)
{
    ASSERT(n00b_unicode_is_nfc(STR("Hello")));
    ASSERT(n00b_unicode_is_nfc(STR("\xC3\xA9")));
}

TEST(test_canonical_ordering)
{
    n00b_string_t nfd = n00b_unicode_nfd(STR("e\xCC\xA7\xCC\x81"), .allocator = nullptr);

    uint32_t pos = 0;
    int32_t  cp1 = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    int32_t  cp2 = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    int32_t  cp3 = n00b_unicode_utf8_decode(nfd.data, nfd.u8_bytes, &pos);
    ASSERT_EQ(cp1, 'e');
    ASSERT_EQ(cp2, 0x0327);
    ASSERT_EQ(cp3, 0x0301);
}

TEST(test_streaming_normalizer)
{
    n00b_unicode_normalizer_t *n = n00b_unicode_normalizer_new(N00B_UNICODE_NFC);

    n00b_unicode_normalizer_feed(n, 'e');
    n00b_unicode_normalizer_feed(n, 0x0301);
    n00b_unicode_normalizer_feed(n, 'x');

    uint32_t out[10];
    size_t   count = n00b_unicode_normalizer_read(n, out, 10);
    ASSERT(count >= 1);
    ASSERT_EQ(out[0], 0x00E9);

    size_t rest = n00b_unicode_normalizer_flush(n, out, 10);
    ASSERT_EQ(rest, 1);
    ASSERT_EQ(out[0], 'x');

    n00b_unicode_normalizer_free(n);
}

// ---------------------------------------------------------------------------
// NormalizationTest.txt conformance harness
// ---------------------------------------------------------------------------

static bool
str_matches_cps(n00b_string_t s, const n00b_codepoint_t *cps, uint32_t n)
{
    if ((uint32_t)s.codepoints != n)
        return false;
    uint32_t pos = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t cp = n00b_unicode_utf8_decode(s.data, s.u8_bytes, &pos);
        if (cp < 0 || (n00b_codepoint_t)cp != cps[i])
            return false;
    }
    return true;
}

static void
run_normalization_conformance(void)
{
    FILE *f = fopen("test/data/NormalizationTest.txt", "r");
    if (!f) {
        printf("  [SKIP] NormalizationTest.txt not found\n");
        return;
    }

    char line[4096];
    int  total = 0, passed = 0, failed = 0;

    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '@' || line[0] == '\n')
            continue;

        // Parse 5 columns separated by semicolons
        char *cols[5];
        char *p = line;
        for (int i = 0; i < 5; i++) {
            cols[i]    = p;
            char *semi = strchr(p, ';');
            if (!semi)
                break;
            *semi = '\0';
            p     = semi + 1;
        }

        uint32_t c[5][32];
        uint32_t cn[5];
        for (int i = 0; i < 5; i++) {
            char tmp[256];
            strncpy(tmp, cols[i], sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            cn[i]                = parse_codepoints(tmp, c[i], 32);
        }

        // Build n00b_string_t for each column
        n00b_string_t s[5];
        for (int i = 0; i < 5; i++) {
            s[i] = cps_to_str(c[i], cn[i]);
        }

        bool ok = true;

        // NFC invariants: c2 == toNFC(c1) == toNFC(c2) == toNFC(c3)
        {
            n00b_string_t r1 = n00b_unicode_nfc(s[0], .allocator = nullptr);
            n00b_string_t r2 = n00b_unicode_nfc(s[1], .allocator = nullptr);
            n00b_string_t r3 = n00b_unicode_nfc(s[2], .allocator = nullptr);
            if (!str_matches_cps(r1, c[1], cn[1]))
                ok = false;
            if (!str_matches_cps(r2, c[1], cn[1]))
                ok = false;
            if (!str_matches_cps(r3, c[1], cn[1]))
                ok = false;
        }

        // NFC: c4 == toNFC(c4) == toNFC(c5)
        {
            n00b_string_t r4 = n00b_unicode_nfc(s[3], .allocator = nullptr);
            n00b_string_t r5 = n00b_unicode_nfc(s[4], .allocator = nullptr);
            if (!str_matches_cps(r4, c[3], cn[3]))
                ok = false;
            if (!str_matches_cps(r5, c[3], cn[3]))
                ok = false;
        }

        // NFD invariants: c3 == toNFD(c1) == toNFD(c2) == toNFD(c3)
        {
            n00b_string_t r1 = n00b_unicode_nfd(s[0], .allocator = nullptr);
            n00b_string_t r2 = n00b_unicode_nfd(s[1], .allocator = nullptr);
            n00b_string_t r3 = n00b_unicode_nfd(s[2], .allocator = nullptr);
            if (!str_matches_cps(r1, c[2], cn[2]))
                ok = false;
            if (!str_matches_cps(r2, c[2], cn[2]))
                ok = false;
            if (!str_matches_cps(r3, c[2], cn[2]))
                ok = false;
        }

        // NFD: c5 == toNFD(c4) == toNFD(c5)
        {
            n00b_string_t r4 = n00b_unicode_nfd(s[3], .allocator = nullptr);
            n00b_string_t r5 = n00b_unicode_nfd(s[4], .allocator = nullptr);
            if (!str_matches_cps(r4, c[4], cn[4]))
                ok = false;
            if (!str_matches_cps(r5, c[4], cn[4]))
                ok = false;
        }

        // NFKC: c4 == toNFKC(c1..c5)
        {
            for (int i = 0; i < 5; i++) {
                n00b_string_t r = n00b_unicode_nfkc(s[i], .allocator = nullptr);
                if (!str_matches_cps(r, c[3], cn[3]))
                    ok = false;
            }
        }

        // NFKD: c5 == toNFKD(c1..c5)
        {
            for (int i = 0; i < 5; i++) {
                n00b_string_t r = n00b_unicode_nfkd(s[i], .allocator = nullptr);
                if (!str_matches_cps(r, c[4], cn[4]))
                    ok = false;
            }
        }

        total++;
        if (ok) {
            passed++;
        }
        else {
            failed++;
            if (failed <= 10) {
                printf("    FAIL line: %s\n", cols[0]);
            }
        }
    }

    fclose(f);

    CONFORMANCE_RESULT("NormalizationTest.txt:", passed, total, failed);
}

static void
run_tests(void)
{
    RUN_TEST(test_nfc_composed);
    RUN_TEST(test_nfd_decomposed);
    RUN_TEST(test_nfc_already_composed);
    RUN_TEST(test_nfkc_compat);
    RUN_TEST(test_nfkd_compat);
    RUN_TEST(test_hangul_decompose_compose);
    RUN_TEST(test_is_nfc);
    RUN_TEST(test_canonical_ordering);
    RUN_TEST(test_streaming_normalizer);
    run_normalization_conformance();
}

TEST_MAIN()
