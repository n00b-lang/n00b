// test_slay_pprint.c — regression tests for slay's pretty-printer.
//
// Builds tiny grammars + parse trees in-process, runs
// `n00b_pretty_print`, and asserts the formatted output matches
// expectations for the annotation kinds we care about
// (@indent, @group, @hardline, @softline) and for the width-based
// group fallback (a flat-fitting group stays on one line; the same
// document at a tiny line_width breaks).

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "adt/option.h"
#include "adt/result.h"
#include "adt/tree.h"
#include "slay/grammar.h"
#include "slay/annotation.h"
#include "slay/parse_tree.h"
#include "slay/pprint.h"
#include "internal/slay/grammar_internal.h"

// ----------------------------------------------------------------------------
// Helpers — build leaf token nodes / interior NT nodes manually.
// ----------------------------------------------------------------------------

static n00b_token_info_t *
mk_tok(const char *text, int64_t tid)
{
    n00b_token_info_t *t = n00b_alloc(n00b_token_info_t);
    t->tid   = tid;
    t->value = n00b_option_set(n00b_string_t *,
                                n00b_string_from_cstr(text));
    return t;
}

static n00b_parse_tree_t *
mk_leaf(const char *text, int64_t tid)
{
    return n00b_tree_leaf(n00b_nt_node_t, n00b_token_info_t *,
                           mk_tok(text, tid));
}

static n00b_parse_tree_t *
mk_nt_node(n00b_grammar_t *g, n00b_nonterm_t *nt)
{
    n00b_nt_node_t v = {};
    v.name = nt->name;
    v.id   = nt->id;
    (void)g;
    return n00b_tree_node(n00b_nt_node_t, n00b_token_info_t *, v);
}

// ----------------------------------------------------------------------------
// Test 1: @indent + @hardline produce indented newlines.
//
// Grammar:
//   block ::= "{" stmt stmt "}"
//   stmt  ::= IDENT
//
// `block` carries @indent and @hardline annotations on child indices
// 1, 2, 3 (the two stmts and the closing "}"). The pretty-printer
// should emit:
//
//   {
//       a
//       b
//   }
//
// (4-space indent, newline between every child, dedent before "}".)
// ----------------------------------------------------------------------------
static void
test_indent_hardline(void)
{
    n00b_grammar_t *g     = n00b_grammar_new();
    n00b_nonterm_t *block = n00b_nonterm(g, r"block");
    n00b_nonterm_t *stmt  = n00b_nonterm(g, r"stmt");

    int64_t lb = n00b_register_terminal(g, r"{");
    int64_t rb = n00b_register_terminal(g, r"}");
    int64_t id = n00b_register_terminal(g, r"IDENT");

    n00b_add_rule(g, block,
                  N00B_TERMINAL(lb),
                  N00B_NT(stmt),
                  N00B_NT(stmt),
                  N00B_TERMINAL(rb));
    n00b_add_rule(g, stmt, N00B_TERMINAL(id));

    // Re-resolve after add_rule (list may have reallocated).
    block = n00b_get_nonterm(g, 0);
    stmt  = n00b_get_nonterm(g, 1);

    n00b_nt_indent(block);
    n00b_nt_hardline(block, N00B_CHILD_IX(1));
    n00b_nt_hardline(block, N00B_CHILD_IX(2));
    n00b_nt_hardline(block, N00B_CHILD_IX(3));

    // Build the parse tree manually.
    n00b_parse_tree_t *root = mk_nt_node(g, block);
    (void)n00b_tree_add_child(root, mk_leaf("{", lb));

    n00b_parse_tree_t *s1 = mk_nt_node(g, stmt);
    (void)n00b_tree_add_child(s1, mk_leaf("a", id));
    (void)n00b_tree_add_child(root, s1);

    n00b_parse_tree_t *s2 = mk_nt_node(g, stmt);
    (void)n00b_tree_add_child(s2, mk_leaf("b", id));
    (void)n00b_tree_add_child(root, s2);

    (void)n00b_tree_add_child(root, mk_leaf("}", rb));

    auto rr = n00b_pretty_print(g, root);
    assert(n00b_result_is_ok(rr));
    n00b_string_t *out = n00b_result_get(rr);

    const char *expected = "{\n    a\n    b\n}";
    printf("    actual:   <<%.*s>>\n", (int)out->u8_bytes, out->data);
    printf("    expected: <<%s>>\n", expected);
    assert(out->u8_bytes == strlen(expected));
    assert(memcmp(out->data, expected, out->u8_bytes) == 0);

    n00b_grammar_free(g);
    printf("  [PASS] indent_hardline\n");
}

// ----------------------------------------------------------------------------
// Test 2: width-based group fallback.
//
// Grammar:
//   list  ::= "(" item item item ")"
//   item  ::= IDENT
//
// `list` carries @group + @softline on child indices 1, 2, 3.
// With line_width=80 the group fits flat; softlines become spaces.
// With line_width=4 the group breaks; softlines become newlines.
// ----------------------------------------------------------------------------
static void
test_group_softline_width(void)
{
    n00b_grammar_t *g    = n00b_grammar_new();
    n00b_nonterm_t *list = n00b_nonterm(g, r"list");
    n00b_nonterm_t *item = n00b_nonterm(g, r"item");

    int64_t lp = n00b_register_terminal(g, r"(");
    int64_t rp = n00b_register_terminal(g, r")");
    int64_t id = n00b_register_terminal(g, r"IDENT");

    n00b_add_rule(g, list,
                  N00B_TERMINAL(lp),
                  N00B_NT(item),
                  N00B_NT(item),
                  N00B_NT(item),
                  N00B_TERMINAL(rp));
    n00b_add_rule(g, item, N00B_TERMINAL(id));

    list = n00b_get_nonterm(g, 0);
    item = n00b_get_nonterm(g, 1);

    n00b_nt_group(list);
    n00b_nt_softline(list, N00B_CHILD_IX(1));
    n00b_nt_softline(list, N00B_CHILD_IX(2));
    n00b_nt_softline(list, N00B_CHILD_IX(3));

    n00b_parse_tree_t *root = mk_nt_node(g, list);
    (void)n00b_tree_add_child(root, mk_leaf("(", lp));

    const char *names[] = {"x", "y", "z"};
    for (int k = 0; k < 3; k++) {
        n00b_parse_tree_t *it = mk_nt_node(g, item);
        (void)n00b_tree_add_child(it, mk_leaf(names[k], id));
        (void)n00b_tree_add_child(root, it);
    }
    (void)n00b_tree_add_child(root, mk_leaf(")", rp));

    // 1) Wide: should fit flat — no newlines.
    auto r1 = n00b_pretty_print(g, root, .line_width = 80);
    assert(n00b_result_is_ok(r1));
    n00b_string_t *flat = n00b_result_get(r1);
    printf("    flat:    <<%.*s>>\n", (int)flat->u8_bytes, flat->data);
    bool has_nl_flat = false;
    for (size_t i = 0; i < flat->u8_bytes; i++) {
        if (flat->data[i] == '\n') {
            has_nl_flat = true;
            break;
        }
    }
    assert(!has_nl_flat);

    // 2) Tight: should break — must contain newlines.
    auto r2 = n00b_pretty_print(g, root, .line_width = 4);
    assert(n00b_result_is_ok(r2));
    n00b_string_t *broken = n00b_result_get(r2);
    printf("    broken:  <<%.*s>>\n", (int)broken->u8_bytes, broken->data);
    bool has_nl_broken = false;
    for (size_t i = 0; i < broken->u8_bytes; i++) {
        if (broken->data[i] == '\n') {
            has_nl_broken = true;
            break;
        }
    }
    assert(has_nl_broken);

    n00b_grammar_free(g);
    printf("  [PASS] group_softline_width\n");
}

// ----------------------------------------------------------------------------
// Test 3: null-input result handling.
// ----------------------------------------------------------------------------
static void
test_null_input(void)
{
    auto r = n00b_pretty_print(nullptr, nullptr);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_ERR_PPRINT_NULL_INPUT);

    n00b_string_t *desc = n00b_pretty_print_err_str(N00B_ERR_PPRINT_NULL_INPUT);
    assert(desc && desc->u8_bytes > 0);

    printf("  [PASS] null_input\n");
}

// ----------------------------------------------------------------------------
// Test 4: a leaf token alone produces just the token text (no group,
// no annotations) — sanity check of the simplest case.
// ----------------------------------------------------------------------------
static void
test_single_token(void)
{
    n00b_grammar_t *g   = n00b_grammar_new();
    n00b_nonterm_t *nt  = n00b_nonterm(g, r"thing");
    int64_t         id  = n00b_register_terminal(g, r"IDENT");
    (void)n00b_add_rule(g, nt, N00B_TERMINAL(id));
    nt = n00b_get_nonterm(g, 0);

    n00b_parse_tree_t *root = mk_nt_node(g, nt);
    (void)n00b_tree_add_child(root, mk_leaf("hello", id));

    auto rr = n00b_pretty_print(g, root);
    assert(n00b_result_is_ok(rr));
    n00b_string_t *out = n00b_result_get(rr);
    printf("    single:  <<%.*s>>\n", (int)out->u8_bytes, out->data);

    assert(out->u8_bytes == 5);
    assert(memcmp(out->data, "hello", 5) == 0);

    n00b_grammar_free(g);
    printf("  [PASS] single_token\n");
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("Running slay pretty-print tests...\n");

    test_single_token();
    test_indent_hardline();
    test_group_softline_width();
    test_null_input();

    printf("All slay pretty-print tests passed.\n");
    n00b_shutdown();
    return 0;
}
