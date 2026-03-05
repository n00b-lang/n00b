#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "text/regex/regex.h"
#include "adt/result.h"

static n00b_regex_t *
must_compile(const char *pat)
{
    n00b_string_t *s = n00b_string_from_cstr(pat);
    auto r = n00b_regex_new(s);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// ============================================================================
// Replace tests
// ============================================================================

static void
test_replace_basic(void)
{
    n00b_regex_t *re = must_compile("[0-9]+");
    n00b_string_t *input = n00b_string_from_cstr("a1b22c333");
    n00b_string_t *repl  = n00b_string_from_cstr("#");
    n00b_string_t *result = n00b_regex_replace(re, input, repl);

    assert(strcmp(result->data, "a#b#c#") == 0);

    printf("  [PASS] replace_basic\n");
}

static void
test_replace_dollar_zero(void)
{
    n00b_regex_t *re = must_compile("[a-z]+");
    n00b_string_t *input = n00b_string_from_cstr("hello world");
    n00b_string_t *repl  = n00b_string_from_cstr("[$0]");
    n00b_string_t *result = n00b_regex_replace(re, input, repl);

    assert(strcmp(result->data, "[hello] [world]") == 0);

    printf("  [PASS] replace_dollar_zero\n");
}

// ============================================================================
// Split tests
// ============================================================================

static void
test_split_basic(void)
{
    n00b_regex_t *re = must_compile(",");
    n00b_string_t *input = n00b_string_from_cstr("a,b,c");
    auto parts = n00b_regex_split(re, input);

    assert(parts->len == 3);
    assert(strcmp(parts->data[0]->data, "a") == 0);
    assert(strcmp(parts->data[1]->data, "b") == 0);
    assert(strcmp(parts->data[2]->data, "c") == 0);

    printf("  [PASS] split_basic\n");
}

static void
test_split_whitespace(void)
{
    n00b_regex_t *re = must_compile("\\s+");
    n00b_string_t *input = n00b_string_from_cstr("one  two\tthree");
    auto parts = n00b_regex_split(re, input);

    assert(parts->len == 3);
    assert(strcmp(parts->data[0]->data, "one") == 0);
    assert(strcmp(parts->data[1]->data, "two") == 0);
    assert(strcmp(parts->data[2]->data, "three") == 0);

    printf("  [PASS] split_whitespace\n");
}

// ============================================================================
// Compile (full DFA) tests
// ============================================================================

static void
test_compile_full_dfa(void)
{
    n00b_regex_t *re = must_compile("[a-z]+");

    assert(!n00b_regex_is_compiled(re));
    n00b_regex_compile(re);
    assert(n00b_regex_is_compiled(re));

    // Should still work after compilation
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("hello")));
    assert(!n00b_regex_is_match(re, n00b_string_from_cstr("123")));

    printf("  [PASS] compile_full_dfa\n");
}

// ============================================================================
// Case-insensitive tests
// ============================================================================

static void
test_case_insensitive(void)
{
    n00b_string_t *s = n00b_string_from_cstr("hello");
    auto r = n00b_regex_new(s, .case_insensitive = true);
    assert(n00b_result_is_ok(r));
    n00b_regex_t *re = n00b_result_get(r);

    assert(n00b_regex_is_match(re, n00b_string_from_cstr("HELLO")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("Hello")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("hElLo")));

    printf("  [PASS] case_insensitive\n");
}

// ============================================================================
// Empty pattern / empty input
// ============================================================================

static void
test_empty_cases(void)
{
    n00b_regex_t *re = must_compile("");
    // Empty pattern should match anything (epsilon matches everywhere)
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("")));
    assert(n00b_regex_is_match(re, n00b_string_from_cstr("abc")));

    n00b_regex_t *re2 = must_compile("a+");
    // Empty input should not match a+
    assert(!n00b_regex_is_match(re2, n00b_string_from_cstr("")));

    printf("  [PASS] empty_cases\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex API tests...\n");

    test_replace_basic();
    test_replace_dollar_zero();
    test_split_basic();
    test_split_whitespace();
    test_compile_full_dfa();
    test_case_insensitive();
    test_empty_cases();

    printf("All regex API tests passed.\n");
    n00b_shutdown();
    return 0;
}
