#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "text/regex/regex.h"
#include "adt/result.h"

// ============================================================================
// Helper
// ============================================================================

static n00b_regex_t *
must_compile(const char *pat)
{
    n00b_string_t *s = n00b_string_from_cstr(pat);
    auto r = n00b_regex_new(s);
    if (n00b_result_is_err(r)) {
        fprintf(stderr, "Failed to compile pattern: %s\n", pat);
        assert(0);
    }
    return n00b_result_get(r);
}

// ============================================================================
// is_match tests
// ============================================================================

static void
test_literal_match(void)
{
    n00b_regex_t *re = must_compile("hello");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("hello")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("say hello world")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("HELLO")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("hell")));

    printf("  [PASS] literal_match\n");
}

static void
test_alternation_match(void)
{
    n00b_regex_t *re = must_compile("cat|dog");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("cat")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("dog")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("bird")));

    printf("  [PASS] alternation_match\n");
}

static void
test_quantifier_match(void)
{
    n00b_regex_t *re = must_compile("ab*c");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("ac")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abc")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abbc")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("adc")));

    printf("  [PASS] quantifier_match\n");
}

static void
test_plus_match(void)
{
    n00b_regex_t *re = must_compile("a+");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("a")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("aaa")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("bbb")));

    printf("  [PASS] plus_match\n");
}

static void
test_dot_match(void)
{
    n00b_regex_t *re = must_compile("a.c");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abc")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("axc")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("ac")));

    printf("  [PASS] dot_match\n");
}

static void
test_charclass_match(void)
{
    n00b_regex_t *re = must_compile("[0-9]+");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("123")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abc123def")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("abcdef")));

    printf("  [PASS] charclass_match\n");
}

static void
test_word_escape_match(void)
{
    n00b_regex_t *re = must_compile("\\w+");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("hello")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("test123")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("   ")));

    printf("  [PASS] word_escape_match\n");
}

// ============================================================================
// count tests
// ============================================================================

static void
test_count_basic(void)
{
    n00b_regex_t *re = must_compile("[a-z]+");
    assert(n00b_regex_count(re, n00b_string_from_cstr("one two three")) == 3);
    assert(n00b_regex_count(re, n00b_string_from_cstr("")) == 0);
    assert(n00b_regex_count(re, n00b_string_from_cstr("hello")) == 1);

    printf("  [PASS] count_basic\n");
}

// ============================================================================
// matches tests
// ============================================================================

static void
test_matches_positions(void)
{
    n00b_regex_t *re = must_compile("[0-9]+");
    n00b_string_t *input = n00b_string_from_cstr("abc123def456");
    auto results = n00b_regex_matches(re, input);

    assert(results->len == 2);
    // First match: "123" at index 3, length 3
    assert(results->data[0].index == 3);
    assert(results->data[0].length == 3);
    // Second match: "456" at index 9, length 3
    assert(results->data[1].index == 9);
    assert(results->data[1].length == 3);

    printf("  [PASS] matches_positions\n");
}

// ============================================================================
// Extension operator tests
// ============================================================================

static void
test_complement_match(void)
{
    // ~(ab) matches anything that is NOT "ab"
    n00b_regex_t *re = must_compile("~(ab)");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("ac")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("ba")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("a")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("")));
    // "ab" itself — the complement should still match since it's unanchored
    // (.*~(ab).* wraps it), need full-string match test with anchors

    printf("  [PASS] complement_match\n");
}

static void
test_wildcard_match(void)
{
    // _ matches any single codepoint
    n00b_regex_t *re = must_compile("a_c");
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abc")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("axc")));
    // _ also matches newline (unlike .)
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("a\nc")));

    printf("  [PASS] wildcard_match\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex match tests...\n");

    test_literal_match();
    test_alternation_match();
    test_quantifier_match();
    test_plus_match();
    test_dot_match();
    test_charclass_match();
    test_word_escape_match();
    test_count_basic();
    test_matches_positions();
    test_complement_match();
    test_wildcard_match();

    printf("All regex match tests passed.\n");
    n00b_shutdown();
    return 0;
}
