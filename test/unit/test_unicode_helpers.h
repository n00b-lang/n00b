#pragma once

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "unicode/encoding.h"
#include "internal/unicode/raw.h"

// ---------------------------------------------------------------------------
// String construction helpers
// ---------------------------------------------------------------------------

/** @brief Create an n00b_string_t from a C string literal (default allocator). */
#define STR(lit)                                                               \
    n00b_string_from_raw(nullptr, (lit), (int64_t)strlen(lit),                    \
                         n00b_unicode_utf8_count_codepoints_raw(               \
                             (lit), (uint32_t)strlen(lit)))

// ---------------------------------------------------------------------------
// Test harness macros
// ---------------------------------------------------------------------------

static int test_count  = 0;
static int test_passed = 0;
static int test_failed = 0;

#define TEST(name)                                                             \
    static void name(void);                                                    \
    static void name(void)

#define RUN_TEST(name)                                                         \
    do {                                                                       \
        test_count++;                                                          \
        printf("  %-50s", #name);                                              \
        name();                                                                \
        printf(" PASS\n");                                                     \
        test_passed++;                                                         \
    } while (0)

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf(" FAIL\n    %s:%d: assertion failed: %s\n", __FILE__,       \
                   __LINE__, #cond);                                           \
            test_failed++;                                                     \
            return;                                                            \
        }                                                                      \
    } while (0)

#define ASSERT_EQ(a, b)                                                        \
    do {                                                                       \
        if ((a) != (b)) {                                                      \
            printf(" FAIL\n    %s:%d: %s == %lld, expected %lld\n", __FILE__,  \
                   __LINE__, #a, (long long)(a), (long long)(b));              \
            test_failed++;                                                     \
            return;                                                            \
        }                                                                      \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                    \
    do {                                                                       \
        if (strcmp((a), (b)) != 0) {                                           \
            printf(" FAIL\n    %s:%d: \"%s\" != \"%s\"\n", __FILE__,           \
                   __LINE__, (a), (b));                                        \
            test_failed++;                                                     \
            return;                                                            \
        }                                                                      \
    } while (0)

#define TEST_MAIN()                                                            \
    int main(int argc, char **argv)                                            \
    {                                                                          \
        n00b_runtime_t runtime;                                                \
        n00b_init(&runtime, argc, argv);                                       \
        printf("Running tests...\n");                                          \
        run_tests();                                                           \
        printf("\n%d/%d tests passed", test_passed, test_count);               \
        if (test_failed > 0)                                                   \
            printf(", %d FAILED", test_failed);                                \
        printf("\n");                                                          \
        return test_failed > 0 ? 1 : 0;                                       \
    }

// Report conformance results: any failure = test failure.
#define CONFORMANCE_RESULT(label, passed_n, total_n, failed_n)                 \
    do {                                                                       \
        printf("  %-40s %d/%d passed", label, passed_n, total_n);              \
        if (failed_n > 0)                                                      \
            printf(" (%d failed)", failed_n);                                  \
        printf("\n");                                                          \
        test_count++;                                                          \
        if ((failed_n) == 0) {                                                 \
            test_passed++;                                                     \
        }                                                                      \
        else {                                                                 \
            test_failed++;                                                     \
        }                                                                      \
    } while (0)

// ---------------------------------------------------------------------------
// Conformance test data parsing utilities
// ---------------------------------------------------------------------------

/** @brief Parse a hex codepoint string like "0041" into a uint32_t. */
static inline uint32_t
parse_hex_cp(const char *s)
{
    return (uint32_t)strtoul(s, nullptr, 16);
}

/**
 * @brief Parse a space-separated list of hex codepoints into an array.
 *
 * Returns the count.  Modifies @p str in place (strtok).
 */
static inline uint32_t
parse_codepoints(char *str, n00b_codepoint_t *cps, uint32_t max_cps)
{
    uint32_t n   = 0;
    char    *tok = strtok(str, " \t");
    while (tok && n < max_cps) {
        cps[n++] = parse_hex_cp(tok);
        tok      = strtok(nullptr, " \t");
    }
    return n;
}

/**
 * @brief Encode codepoints to an n00b_string_t (default allocator).
 */
static inline n00b_string_t
cps_to_str(const n00b_codepoint_t *cps, uint32_t count)
{
    // Worst case: 4 bytes per codepoint
    char     buf[count * 4 + 1];
    uint32_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        pos += n00b_unicode_utf8_encode(cps[i], buf + pos);
    }
    buf[pos] = '\0';
    int64_t ncp = n00b_unicode_utf8_count_codepoints_raw(buf, pos);
    return n00b_string_from_raw(nullptr, buf, pos, ncp >= 0 ? ncp : 0);
}

/**
 * @brief Parse a break test line in the standard Unicode break test format.
 *
 * Format: `÷ 0020 × 0308 ÷ 000A ÷`
 * Fills @p cps with codepoints and @p breaks with break flags
 * (1 = break before cp[i]).  Returns number of codepoints.
 *
 * Note: ÷ is U+00F7, encoded as C3 B7 in UTF-8.
 *       × is U+00D7, encoded as C3 97 in UTF-8.
 */
static inline uint32_t
parse_break_test_line(const char *line, n00b_codepoint_t *cps, int *breaks,
                      uint32_t max_cps)
{
    uint32_t    n = 0;
    const char *p = line;

    while (*p && n < max_cps) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0')
            break;

        // Check for ÷ (C3 B7) or × (C3 97)
        if ((uint8_t)p[0] == 0xC3 && (uint8_t)p[1] == 0xB7) {
            // ÷ = break
            if (n < max_cps)
                breaks[n] = 1;
            p += 2;
            continue;
        }
        if ((uint8_t)p[0] == 0xC3 && (uint8_t)p[1] == 0x97) {
            // × = no break
            if (n < max_cps)
                breaks[n] = 0;
            p += 2;
            continue;
        }

        // Parse hex codepoint
        char               *end;
        n00b_codepoint_t cp = (n00b_codepoint_t)strtoul(p, &end, 16);
        if (end == p)
            break; // not a hex number
        cps[n++] = cp;
        p        = end;
    }

    return n;
}
