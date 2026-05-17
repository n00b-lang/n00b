// Phase 11 typed translation of resharp-c/tests/parser.c.
//
// The original test exercised the resharp-engine C API (Regex / regex_new /
// regex_free) and used REQUIRE/ASSERT/xmalloc.  The translated form uses the
// public n00b regex API (`n00b_regex_t *`, `n00b_regex_new`) per § 7.5 — the
// data values (pattern strings, accept/reject membership) are byte-identical
// oracles and must not be paraphrased.

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "text/regex/regex.h"

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static bool
pattern_is_err(const char *p)
{
    n00b_string_t *pat = n00b_string_from_cstr(p);
    n00b_result_t(n00b_regex_t *) r = n00b_regex_new(pat);
    return n00b_result_is_err(r);
}

static bool
pattern_is_ok(const char *p)
{
    n00b_string_t *pat = n00b_string_from_cstr(p);
    n00b_result_t(n00b_regex_t *) r = n00b_regex_new(pat);
    return n00b_result_is_ok(r);
}

// Repeats `c` `n` times into a freshly-allocated, NUL-terminated string.
[[nodiscard]] static char *
str_repeat(char c, size_t n)
{
    char *s = n00b_alloc_array(char, n + 1);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void
test_huge_repetitions_are_rejected(void)
{
    static const char *const reject[] = {
        "a{2001}",
        "a{1000000}",
        ".{1,8191}",
        ".{1,7168}",
        "a{2147483647,2147483647}",
        "a{2147483648,2147483648}",
        "([0-9]{1,9999}):([0-9]{1,9999})",
    };
    static const char *const accept[] = {
        "a{500}",
        "a{0,500}",
        "a{1,499}",
    };

    for (size_t i = 0; i < sizeof(reject) / sizeof(reject[0]); i++) {
        const char *p = reject[i];
        assert(pattern_is_err(p));
    }
    for (size_t i = 0; i < sizeof(accept) / sizeof(accept[0]); i++) {
        const char *p = accept[i];
        assert(pattern_is_ok(p));
    }

    printf("  [PASS] huge_repetitions_are_rejected\n");
}

static void
test_deeply_nested_repetitions_rejected(void)
{
    static const char *const reject[] = {
        // 10 levels of {3,6}: ~6^10 expanded nodes.
        "(?:a(?:b(?:c(?:d(?:e(?:f(?:g(?:h(?:i(?:FooBar){3,6}){3,6}){3,6}){3,6}){3,6}){3,6}){3,6}){3,6}){3,6}){3,6}",
        // 13 levels of {2}: ~2^13 * 6 expanded.
        "(?:a(?:b(?:c(?:d(?:e(?:f(?:g(?:h(?:i(?:j(?:k(?:l(?:FooBar){2}){2}){2}){2}){2}){2}){2}){2}){2}){2}){2}){2}){2}){2}",
    };
    for (size_t i = 0; i < sizeof(reject) / sizeof(reject[0]); i++) {
        const char *p = reject[i];
        assert(pattern_is_err(p));
    }

    // long_alt = format!("{}|{}", "a".repeat(5000), "b".repeat(5000));
    constexpr size_t REPEAT = 5000;
    char *as = str_repeat('a', REPEAT);
    char *bs = str_repeat('b', REPEAT);
    size_t alt_len = REPEAT + 1 + REPEAT;
    char *long_alt = n00b_alloc_array(char, alt_len + 1);
    memcpy(long_alt, as, REPEAT);
    long_alt[REPEAT] = '|';
    memcpy(long_alt + REPEAT + 1, bs, REPEAT);
    long_alt[alt_len] = '\0';
    n00b_free(as);
    n00b_free(bs);
    assert(pattern_is_err(long_alt));
    n00b_free(long_alt);

    static const char *const accept[] = {
        "(?:a(?:b(?:c(?:FooBar){2}){2}){2}){2}",
        "a{100}",
        "[a-z]{50,200}",
    };
    for (size_t i = 0; i < sizeof(accept) / sizeof(accept[0]); i++) {
        const char *p = accept[i];
        assert(pattern_is_ok(p));
    }

    printf("  [PASS] deeply_nested_repetitions_rejected\n");
}

static void
test_mixed_alt_and_intersection_top_level_does_not_panic(void)
{
    static const char *const cases[] = {
        "^&|&$",
        "\\s|&nbsp;",
        "&|x",
        "&&|\\|\\|",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *p = cases[i];
        assert(pattern_is_err(p));
    }

    printf("  [PASS] mixed_alt_and_intersection_top_level_does_not_panic\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex parser tests...\n");

    test_huge_repetitions_are_rejected();
    test_deeply_nested_repetitions_rejected();
    test_mixed_alt_and_intersection_top_level_does_not_panic();

    printf("All regex parser tests passed.\n");
    n00b_shutdown();
    return 0;
}
