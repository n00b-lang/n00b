#include "test_unicode_helpers.h"
#include "unicode/bidi.h"
#include "unicode/encoding.h"

// ---------------------------------------------------------------------------
// Manual unit tests
// ---------------------------------------------------------------------------

TEST(test_ltr_text)
{
    n00b_unicode_bidi_para_t *p = n00b_unicode_bidi_open(*r"Hello");
    ASSERT_EQ(n00b_unicode_bidi_paragraph_level(p), 0);

    n00b_array_t(uint8_t) levels = n00b_unicode_bidi_levels(p);
    ASSERT_EQ(levels.len, 5);
    for (uint32_t i = 0; i < levels.len; i++) {
        ASSERT_EQ(levels.data[i], 0);
    }

    n00b_unicode_bidi_free(p);
}

TEST(test_rtl_arabic)
{
    n00b_unicode_bidi_para_t *p = n00b_unicode_bidi_open(*r"\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7");
    ASSERT_EQ(n00b_unicode_bidi_paragraph_level(p), 1);

    n00b_array_t(uint8_t) levels = n00b_unicode_bidi_levels(p);
    ASSERT_EQ(levels.len, 5);
    for (uint32_t i = 0; i < levels.len; i++) {
        ASSERT_EQ(levels.data[i], 1);
    }

    n00b_unicode_bidi_free(p);
}

TEST(test_visual_reorder)
{
    n00b_unicode_bidi_para_t *p = n00b_unicode_bidi_open(*r"Hello");

    n00b_array_t(int32_t) map = n00b_unicode_bidi_reorder_visual(p);
    ASSERT_EQ(map.len, 5);
    for (int i = 0; i < 5; i++) {
        ASSERT_EQ(map.data[i], i);
    }

    n00b_unicode_bidi_free(p);
}

// ---------------------------------------------------------------------------
// BidiCharacterTest.txt conformance harness
// ---------------------------------------------------------------------------

static void
run_bidi_conformance(void)
{
    FILE *f = fopen("test/data/BidiCharacterTest.txt", "r");
    if (!f) {
        printf("  [SKIP] BidiCharacterTest.txt not found\n");
        return;
    }

    char line[8192];
    int total = 0, passed_count = 0, fail_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Parse 5 semicolon-separated fields
        char *fields[5] = {nullptr};
        char *p = line;
        for (int i = 0; i < 5; i++) {
            fields[i] = p;
            char *semi = strchr(p, ';');
            if (semi) {
                *semi = '\0';
                p = semi + 1;
            }
        }

        if (!fields[0] || !fields[1] || !fields[2] || !fields[3]) continue;

        // Field 1: paragraph direction (0=LTR, 1=RTL, 2=auto)
        int para_dir = atoi(fields[1]);

        // Our n00b_unicode_bidi_open always does auto-detection (rule P2/P3).
        // Only test auto (2) cases to avoid mismatch with explicit LTR/RTL.
        if (para_dir != 2) continue;

        // Parse codepoints from field 0
        char cp_buf[4096];
        strncpy(cp_buf, fields[0], sizeof(cp_buf) - 1);
        cp_buf[sizeof(cp_buf) - 1] = '\0';
        n00b_codepoint_t cps[512];
        uint32_t n = parse_codepoints(cp_buf, cps, 512);
        if (n == 0) continue;

        // Expected resolved paragraph level
        int expected_para_level = atoi(fields[2]);

        // Parse expected levels
        uint8_t expected_levels[512];
        bool    level_is_x[512];
        uint32_t n_levels = 0;
        {
            char level_buf[4096];
            strncpy(level_buf, fields[3], sizeof(level_buf) - 1);
            level_buf[sizeof(level_buf) - 1] = '\0';
            char *tok = strtok(level_buf, " \t\n");
            while (tok && n_levels < 512) {
                if (tok[0] == 'x') {
                    level_is_x[n_levels] = true;
                    expected_levels[n_levels] = 0;
                } else {
                    level_is_x[n_levels] = false;
                    expected_levels[n_levels] = (uint8_t)atoi(tok);
                }
                n_levels++;
                tok = strtok(nullptr, " \t\n");
            }
        }

        // Build string and run bidi
        n00b_string_t s = cps_to_str(cps, n);
        n00b_unicode_bidi_para_t *bidi = n00b_unicode_bidi_open(s);

        bool ok = true;

        // Check paragraph level
        if (n00b_unicode_bidi_paragraph_level(bidi) != expected_para_level) {
            ok = false;
        }

        // Check resolved levels
        n00b_array_t(uint8_t) actual_levels = n00b_unicode_bidi_levels(bidi);
        uint32_t actual_len = (uint32_t)actual_levels.len;

        if (actual_len != n_levels) {
            ok = false;
        } else {
            for (uint32_t i = 0; i < n_levels; i++) {
                if (level_is_x[i]) continue; // X9-removed chars, skip
                if (actual_levels.data[i] != expected_levels[i]) {
                    ok = false;
                    break;
                }
            }
        }

        total++;
        if (ok) {
            passed_count++;
        } else {
            fail_count++;
            if (fail_count <= 20) {
                printf("    FAIL line %d: n=%d cps:", total, n);
                for (uint32_t i = 0; i < n && i < 10; i++) {
                    printf(" %04X", cps[i]);
                }
                if (n > 10) printf(" ...");
                printf("\n      expected_pl=%d got_pl=%d\n", expected_para_level,
                       n00b_unicode_bidi_paragraph_level(bidi));
                printf("      exp_levels:");
                for (uint32_t i = 0; i < n_levels; i++) {
                    if (level_is_x[i]) printf(" x");
                    else printf(" %d", expected_levels[i]);
                }
                printf("\n      got_levels:");
                for (uint32_t i = 0; i < actual_len; i++) {
                    printf(" %d", actual_levels.data[i]);
                }
                printf("\n");
            }
        }

        n00b_unicode_bidi_free(bidi);
    }

    fclose(f);

    CONFORMANCE_RESULT("BidiCharacterTest.txt:", passed_count, total, fail_count);
}

static void run_tests(void)
{
    RUN_TEST(test_ltr_text);
    RUN_TEST(test_rtl_arabic);
    RUN_TEST(test_visual_reorder);
    run_bidi_conformance();
}

TEST_MAIN()
