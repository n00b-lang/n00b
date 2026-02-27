// test_pwz.c — Smoke tests for the PWZ parser extraction.
//
// Tests:
//   1. Token-level parse: E → E "+" T | T ; T → ID
//   2. Codepoint-level parse: S → 'a' 'b'
//   3. Ambiguous grammar (forest): S → S S | 'a'

#include "slay/pwz.h"
#include "parsers/token_stream.h"
#include "core/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// Terminal IDs.
#define TOK_PLUS (N00B_TOK_START_ID + 10)
#define TOK_ID   (N00B_TOK_START_ID + 11)

static void
test_basic_parse(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    n00b_nonterm_t *E = n00b_nonterm(g, n00b_string_from_cstr("E"));
    n00b_nonterm_t *T = n00b_nonterm(g, n00b_string_from_cstr("T"));

    // Capture ID before finalize (finalize may realloc the nt_list).
    int64_t E_id = n00b_nonterm_id(E);

    n00b_register_terminal(g, n00b_string_from_cstr("PLUS"));
    n00b_register_terminal(g, n00b_string_from_cstr("ID"));

    n00b_add_rule(g, E, N00B_NT(E), N00B_TERMINAL(TOK_PLUS), N00B_NT(T));
    n00b_add_rule(g, E, N00B_NT(T));
    n00b_add_rule(g, T, N00B_TERMINAL(TOK_ID));

    n00b_grammar_set_start(g, E);

    n00b_token_info_t tokens[3] = {
        { .tid = TOK_ID,   .index = 0, .line = 1, .column = 1,
          .value = n00b_option_set(n00b_string_t, n00b_string_from_cstr("a")) },
        { .tid = TOK_PLUS, .index = 1, .line = 1, .column = 3,
          .value = n00b_option_set(n00b_string_t, n00b_string_from_cstr("+")) },
        { .tid = TOK_ID,   .index = 2, .line = 1, .column = 5,
          .value = n00b_option_set(n00b_string_t, n00b_string_from_cstr("b")) },
    };

    n00b_token_info_t *ptrs[3] = { &tokens[0], &tokens[1], &tokens[2] };

    n00b_token_stream_t *ts = n00b_token_stream_from_array(ptrs, 3);
    assert(ts);

    n00b_pwz_parser_t *p = n00b_pwz_new(g);
    assert(p);

    bool ok = n00b_pwz_parse(p, ts);
    assert(ok && "parse should succeed");

    n00b_parse_tree_t *tree = n00b_pwz_get_tree(p);
    assert(tree && "should have a parse tree");

    assert(!n00b_tree_is_leaf(tree));
    n00b_nt_node_t *root = &n00b_tree_node_value(tree);
    assert(root->id == E_id);
    assert(n00b_tree_num_children(tree) == 3);

    void *result = n00b_parse_tree_walk(g, tree, NULL);
    (void)result;

    printf("PASS: basic parse of 'a + b'\n");

    n00b_pwz_free(p);
    n00b_token_stream_free(ts);
    n00b_grammar_free(g);
}

static void
test_codepoint_parse(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    n00b_nonterm_t *S = n00b_nonterm(g, n00b_string_from_cstr("S"));

    n00b_add_rule(g, S, N00B_CHAR('a'), N00B_CHAR('b'));

    n00b_grammar_set_start(g, S);

    n00b_string_t input = n00b_string_from_cstr("ab");
    n00b_token_stream_t *ts = n00b_token_stream_from_codepoints(input);
    assert(ts);

    n00b_pwz_parser_t *p = n00b_pwz_new(g);
    bool ok = n00b_pwz_parse(p, ts);
    assert(ok && "codepoint parse should succeed");

    n00b_parse_tree_t *tree = n00b_pwz_get_tree(p);
    assert(tree);

    printf("PASS: codepoint parse of 'ab'\n");

    n00b_pwz_free(p);
    n00b_token_stream_free(ts);
    n00b_grammar_free(g);
}

static void
test_forest(void)
{
    n00b_grammar_t *g = n00b_grammar_new();

    n00b_nonterm_t *S = n00b_nonterm(g, n00b_string_from_cstr("S"));

    n00b_add_rule(g, S, N00B_NT(S), N00B_NT(S));
    n00b_add_rule(g, S, N00B_CHAR('a'));

    n00b_grammar_set_start(g, S);

    n00b_string_t input = n00b_string_from_cstr("aaa");
    n00b_token_stream_t *ts = n00b_token_stream_from_codepoints(input);

    n00b_parse_forest_t forest = n00b_pwz_parse_grammar(g, ts);

    int32_t count = n00b_parse_forest_count(&forest);
    assert(count >= 1 && "should have at least one parse");

    printf("PASS: ambiguous parse of 'aaa' (%d trees)\n", count);

    n00b_parse_forest_free(&forest);
    n00b_token_stream_free(ts);
    n00b_grammar_free(g);
}

int
main(void)
{
    test_basic_parse();
    test_codepoint_parse();
    test_forest();

    printf("\nAll PWZ smoke tests passed.\n");
    return 0;
}
