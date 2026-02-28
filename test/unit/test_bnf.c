#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "text/strings/string_ops.h"
#include "slay/bnf.h"
#include "slay/pwz.h"
#include "slay/parse_tree.h"
#include "internal/slay/grammar_internal.h"

// r"..." is a raw string (no escape processing), so \n stays literal.
// BNF needs actual newlines, so we use n00b_string_from_cstr for BNF text.
static n00b_string_t *
bnf(const char *text)
{
    return n00b_string_from_cstr(text);
}

// ============================================================================
// 1. Simple grammar: S ::= A B
// ============================================================================

static void
test_simple_grammar(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= %IDENTIFIER %INTEGER\n"),
        r"S", g);

    assert(ok);
    assert(g->finalized);
    assert(g->default_start >= 0);

    n00b_nonterm_t *s = n00b_get_nonterm(g, g->default_start);
    assert(s != NULL);
    assert(n00b_unicode_str_eq(s->name, r"S"));

    n00b_grammar_free(g);
    printf("  [PASS] simple_grammar\n");
}

// ============================================================================
// 2. Alternatives: S ::= A | B
// ============================================================================

static void
test_alternatives(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= %IDENTIFIER | %INTEGER\n"),
        r"S", g);

    assert(ok);

    n00b_nonterm_t *s = n00b_get_nonterm(g, g->default_start);
    assert(s != NULL);
    // S should have 2 rules (one for each alternative).
    assert(n00b_list_len(s->rule_ids) >= 2);

    n00b_grammar_free(g);
    printf("  [PASS] alternatives\n");
}

// ============================================================================
// 3. Multiple rules with cross-references
// ============================================================================

static void
test_multiple_rules(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<expr> ::= <term> | <expr> %IDENTIFIER <term>\n"
            "<term> ::= %INTEGER\n"),
        r"expr", g);

    assert(ok);

    n00b_nonterm_t *expr = n00b_get_nonterm(g, g->default_start);
    assert(expr != NULL);
    assert(n00b_unicode_str_eq(expr->name, r"expr"));

    n00b_grammar_free(g);
    printf("  [PASS] multiple_rules\n");
}

// ============================================================================
// 4. Literal tokens
// ============================================================================

static void
test_literal_tokens(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= \"hello\" %IDENTIFIER\n"),
        r"S", g);

    assert(ok);
    assert(g->finalized);

    n00b_grammar_free(g);
    printf("  [PASS] literal_tokens\n");
}

// ============================================================================
// 5. Comments and continuation lines
// ============================================================================

static void
test_comments_and_continuations(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("# This is a comment\n"
            "<S> ::= %IDENTIFIER\n"
            "    | %INTEGER  # another comment\n"),
        r"S", g);

    assert(ok);

    n00b_nonterm_t *s = n00b_get_nonterm(g, g->default_start);
    assert(s != NULL);
    assert(n00b_list_len(s->rule_ids) >= 2);

    n00b_grammar_free(g);
    printf("  [PASS] comments_and_continuations\n");
}

// ============================================================================
// 6. Empty input returns false
// ============================================================================

static void
test_empty_input(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(bnf(""), r"S", g);
    assert(!ok);

    n00b_grammar_free(g);
    printf("  [PASS] empty_input\n");
}

// ============================================================================
// 7. Null grammar returns false
// ============================================================================

static void
test_null_grammar(void)
{
    bool ok = n00b_bnf_load(
        bnf("<S> ::= %IDENTIFIER\n"), r"S", NULL);
    assert(!ok);
    printf("  [PASS] null_grammar\n");
}

// ============================================================================
// 8. Explicit parse_fn kwarg
// ============================================================================

static void
test_explicit_parse_fn(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= %IDENTIFIER\n"),
        r"S", g,
        .parse_fn = n00b_pwz_parse_grammar);

    assert(ok);
    assert(g->finalized);

    n00b_grammar_free(g);
    printf("  [PASS] explicit_parse_fn\n");
}

// ============================================================================
// 9. EBNF quantifier: S ::= A+
// ============================================================================

static void
test_ebnf_plus(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= %IDENTIFIER+\n"),
        r"S", g);

    assert(ok);
    assert(g->finalized);

    n00b_grammar_free(g);
    printf("  [PASS] ebnf_plus\n");
}

// ============================================================================
// 10. Character class: __DIGIT
// ============================================================================

static void
test_char_class(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    bool ok = n00b_bnf_load(
        bnf("<S> ::= __DIGIT+\n"),
        r"S", g);

    assert(ok);
    assert(g->finalized);

    n00b_grammar_free(g);
    printf("  [PASS] char_class\n");
}

// ============================================================================
// main
// ============================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("BNF loader tests:\n");
    test_simple_grammar();
    test_alternatives();
    test_multiple_rules();
    test_literal_tokens();
    test_comments_and_continuations();
    test_empty_input();
    test_null_grammar();
    test_explicit_parse_fn();
    test_ebnf_plus();
    test_char_class();

    printf("All BNF tests passed.\n");
    return 0;
}
