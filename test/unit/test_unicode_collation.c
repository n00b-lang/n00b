#include "test_unicode_helpers.h"
#include "unicode/collation.h"
#include "unicode/encoding.h"

// ---------------------------------------------------------------------------
// Manual unit tests
// ---------------------------------------------------------------------------

TEST(test_sort_key)
{
    n00b_unicode_sort_key_t key = n00b_unicode_sort_key(*r"hello");
    ASSERT(key.data != nullptr);
    ASSERT(key.len > 0);
    n00b_unicode_sort_key_free(&key);
}

TEST(test_collate_same)
{
    ASSERT_EQ(n00b_unicode_collate(*r"hello", *r"hello"), 0);
}

TEST(test_collate_order)
{
    ASSERT(n00b_unicode_collate(*r"apple", *r"banana") < 0);
    ASSERT(n00b_unicode_collate(*r"banana", *r"apple") > 0);
}

TEST(test_collate_case)
{
    int result = n00b_unicode_collate(*r"a", *r"A");
    ASSERT(result != 0);
}

// ---------------------------------------------------------------------------
// CollationTest_NON_IGNORABLE_SHORT.txt conformance harness
// ---------------------------------------------------------------------------

static void
run_collation_conformance(void)
{
    FILE *f = fopen("test/data/CollationTest_NON_IGNORABLE_SHORT.txt", "r");
    if (!f) {
        printf("  [SKIP] CollationTest_NON_IGNORABLE_SHORT.txt not found\n");
        return;
    }

    char line[4096];
    int total = 0, passed_count = 0, fail_count = 0;

    bool have_prev = false;
    n00b_string_t prev = {};

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) continue;

        // Parse hex codepoints
        char tmp[4096];
        strncpy(tmp, line, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        n00b_codepoint_t cps[256];
        uint32_t n = parse_codepoints(tmp, cps, 256);
        if (n == 0) continue;

        // Skip lines with surrogate codepoints (can't encode in UTF-8)
        bool has_surrogate = false;
        for (uint32_t j = 0; j < n; j++) {
            if (cps[j] >= 0xD800 && cps[j] <= 0xDFFF) {
                has_surrogate = true;
                break;
            }
        }
        if (has_surrogate) {
            have_prev = false;
            continue;
        }

        n00b_string_t cur = cps_to_str(cps, n);

        if (have_prev) {
            int cmp = n00b_unicode_collate(prev, cur);
            total++;
            if (cmp <= 0) {
                passed_count++;
            } else {
                fail_count++;
                if (fail_count <= 5) {
                    printf("    FAIL line %d: prev > cur\n", total);
                }
            }
        }

        prev = cur;
        have_prev = true;
    }

    fclose(f);

    CONFORMANCE_RESULT("CollationTest conformance:", passed_count, total, fail_count);
}

static void run_tests(void)
{
    RUN_TEST(test_sort_key);
    RUN_TEST(test_collate_same);
    RUN_TEST(test_collate_order);
    RUN_TEST(test_collate_case);
    run_collation_conformance();
}

TEST_MAIN()
