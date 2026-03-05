#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "text/regex/parse.h"
#include "text/regex/node.h"
#include "text/regex/charset.h"
#include "adt/result.h"

// ============================================================================
// Helper: parse a pattern, assert success, return root node ID
// ============================================================================

static uint32_t
must_parse(n00b_regex_builder_t *b, const char *pat)
{
    n00b_string_t *s = n00b_string_from_cstr(pat);
    auto r = n00b_regex_parse(b, s, false, false, false);
    assert(n00b_result_is_ok(r));
    return n00b_result_get(r);
}

// ============================================================================
// Parse tests
// ============================================================================

static void
test_parse_literal(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "abc");

    // Should be Concat(Concat(Singleton('a'), Singleton('b')), Singleton('c'))
    // or some equivalent structure
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_CONCAT);

    printf("  [PASS] parse_literal\n");
}

static void
test_parse_alternation(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "a|b");

    // Singleton merge optimization: Singleton('a') | Singleton('b') → Singleton('a'|'b')
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'a'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'b'));
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'c'));

    // Non-singleton alternation still produces Or
    root = must_parse(&b, "ab|cd");
    n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_OR);

    printf("  [PASS] parse_alternation\n");
}

static void
test_parse_quantifiers(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    // a* => Loop(Singleton('a'), 0, MAX)
    uint32_t root = must_parse(&b, "a*");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_LOOP);
    assert(n->loop.lo == 0);

    // a+ => Loop(Singleton('a'), 1, MAX)
    root = must_parse(&b, "a+");
    n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_LOOP);
    assert(n->loop.lo == 1);

    // a? => Loop(Singleton('a'), 0, 1)
    root = must_parse(&b, "a?");
    n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_LOOP);
    assert(n->loop.lo == 0);
    assert(n->loop.hi == 1);

    // a{3,5}
    root = must_parse(&b, "a{3,5}");
    n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_LOOP);
    assert(n->loop.lo == 3);
    assert(n->loop.hi == 5);

    printf("  [PASS] parse_quantifiers\n");
}

static void
test_parse_charclass(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "[abc]");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    // 'a', 'b', 'c' should all be in the set
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'a'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'b'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'c'));
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'd'));

    printf("  [PASS] parse_charclass\n");
}

static void
test_parse_negated_charclass(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "[^a]");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'a'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'b'));

    printf("  [PASS] parse_negated_charclass\n");
}

static void
test_parse_escape(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "\\d");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    assert(n00b_regex_charset_contains(&s, n->singleton.set, '0'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, '9'));
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'a'));

    printf("  [PASS] parse_escape\n");
}

static void
test_parse_dot(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, ".");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'a'));
    // By default dot doesn't match newline
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, '\n'));

    printf("  [PASS] parse_dot\n");
}

static void
test_parse_anchors(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "\\Afoo\\z");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    // Should be Concat(BEGIN, Concat(f, Concat(o, Concat(o, END))))
    assert(n->kind == N00B_RE_CONCAT);

    printf("  [PASS] parse_anchors\n");
}

static void
test_parse_intersection(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    // All-singleton collapse: Singleton([a-z]) & Singleton([aeiou]) → Singleton(And([a-z],[aeiou]))
    uint32_t root = must_parse(&b, "[a-z]&[aeiou]");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_SINGLETON);
    // Result should contain only vowels
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'a'));
    assert(n00b_regex_charset_contains(&s, n->singleton.set, 'e'));
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'b'));
    assert(!n00b_regex_charset_contains(&s, n->singleton.set, 'z'));

    // Non-singleton intersection ab&cd: no string matches both "ab" and "cd",
    // so optimizations correctly reduce to NOTHING (empty set singleton).
    root = must_parse(&b, "ab&cd");
    assert(root == N00B_RE_ID_NOTHING);

    // Intersection with compatible patterns: [a-z][a-z]&ab should match "ab"
    root = must_parse(&b, "[a-z][a-z]&ab");
    n = n00b_regex_node_get(&b, root);
    // Head grouping: And(Singleton([a-z]),Singleton(a)) → Singleton(a)
    // Tail grouping: And(Singleton([a-z]),Singleton(b)) → Singleton(b)
    // Result: Concat(Singleton(a), Singleton(b))
    assert(n->kind == N00B_RE_CONCAT);

    printf("  [PASS] parse_intersection\n");
}

static void
test_parse_complement(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    uint32_t root = must_parse(&b, "~(abc)");
    n00b_regex_node_t *n = n00b_regex_node_get(&b, root);
    assert(n->kind == N00B_RE_NOT);

    printf("  [PASS] parse_complement\n");
}

static void
test_parse_error(void)
{
    n00b_regex_solver_t s = n00b_regex_solver_new();
    n00b_regex_builder_t b = n00b_regex_builder_new(&s);

    n00b_string_t *pat = n00b_string_from_cstr("(abc");
    auto r = n00b_regex_parse(&b, pat, false, false, false);
    assert(n00b_result_is_err(r));

    printf("  [PASS] parse_error\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running regex parse tests...\n");

    test_parse_literal();
    test_parse_alternation();
    test_parse_quantifiers();
    test_parse_charclass();
    test_parse_negated_charclass();
    test_parse_escape();
    test_parse_dot();
    test_parse_anchors();
    test_parse_intersection();
    test_parse_complement();
    test_parse_error();

    printf("All regex parse tests passed.\n");
    n00b_shutdown();
    return 0;
}
