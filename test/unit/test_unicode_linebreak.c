#include "test_unicode_helpers.h"
#include "unicode/linebreak.h"
#include "unicode/encoding.h"

// ---------------------------------------------------------------------------
// Manual unit tests
// ---------------------------------------------------------------------------

TEST(test_mandatory_breaks)
{
    n00b_unicode_lb_action_t actions[3];
    n00b_unicode_linebreaks(STR("a\nb"), actions);
    ASSERT_EQ(actions[0], N00B_UNICODE_LB_ACTION_NONE);
    ASSERT_EQ(actions[2], N00B_UNICODE_LB_ACTION_MANDATORY);
}

TEST(test_no_break_crlf)
{
    n00b_unicode_lb_action_t actions[4];
    n00b_unicode_linebreaks(STR("a\r\nb"), actions);
    ASSERT_EQ(actions[2], N00B_UNICODE_LB_ACTION_NONE);
    ASSERT_EQ(actions[3], N00B_UNICODE_LB_ACTION_MANDATORY);
}

TEST(test_space_break)
{
    n00b_unicode_lb_action_t actions[11];
    n00b_unicode_linebreaks(STR("hello world"), actions);
    ASSERT_EQ(actions[6], N00B_UNICODE_LB_ACTION_ALLOWED);
}

TEST(test_wrap)
{
    n00b_array_t(uint32_t) breaks = n00b_unicode_linebreak_wrap(STR("hello world test"), .width = 10);
    ASSERT(breaks.len >= 1);
}

// ---------------------------------------------------------------------------
// LineBreakTest.txt conformance harness
// ---------------------------------------------------------------------------

static void
run_linebreak_conformance(void)
{
    FILE *f = fopen("test/data/LineBreakTest.txt", "r");
    if (!f) {
        printf("  [SKIP] LineBreakTest.txt not found\n");
        return;
    }

    char line[4096];
    int total = 0, passed_count = 0, fail_count = 0;

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        n00b_codepoint_t cps[256];
        int breaks[257];
        memset(breaks, 0, sizeof(breaks));

        uint32_t n = parse_break_test_line(line, cps, breaks, 256);
        if (n == 0) continue;

        n00b_string_t s = cps_to_str(cps, n);

        // Run n00b_unicode_linebreaks
        n00b_unicode_lb_action_t *actions = n00b_alloc_array(n00b_unicode_lb_action_t, n);
        n00b_unicode_linebreaks(s, actions);

        // Compare: breaks[i] == 1 means break BEFORE codepoint i.
        // actions[i] = NONE/ALLOWED/MANDATORY before codepoint i.
        // breaks[0] is always 1 (start of text) — we skip it.
        bool ok = true;
        for (uint32_t i = 1; i < n; i++) {
            bool expected_break = (breaks[i] == 1);
            bool actual_break = (actions[i] == N00B_UNICODE_LB_ACTION_ALLOWED ||
                                 actions[i] == N00B_UNICODE_LB_ACTION_MANDATORY);
            if (expected_break != actual_break) {
                ok = false;
                break;
            }
        }

        total++;
        if (ok) {
            passed_count++;
        } else {
            fail_count++;
            if (fail_count <= 5) {
                printf("    FAIL line %d: cps:", total);
                for (uint32_t i = 0; i < n; i++) {
                    printf(" %04X", cps[i]);
                }
                printf(" | expected:");
                for (uint32_t i = 0; i < n; i++) {
                    printf(" %c", breaks[i] ? '/' : 'x');
                }
                printf(" | got:");
                for (uint32_t i = 0; i < n; i++) {
                    printf(" %c", (actions[i] != N00B_UNICODE_LB_ACTION_NONE) ? '/' : 'x');
                }
                printf("\n");
            }
        }

        n00b_free(actions);
    }

    fclose(f);

    CONFORMANCE_RESULT("LineBreakTest.txt:", passed_count, total, fail_count);
}

static void run_tests(void)
{
    RUN_TEST(test_mandatory_breaks);
    RUN_TEST(test_no_break_crlf);
    RUN_TEST(test_space_break);
    RUN_TEST(test_wrap);
    run_linebreak_conformance();
}

TEST_MAIN()
