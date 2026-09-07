
#include "parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TOK_PLUS (PARSE_TOK_START_ID + 10)
#define TOK_ID   (PARSE_TOK_START_ID + 11)

static const char *
leaf_text(parse_tree_t *node)
{
    if (!node || !core_tree_is_leaf(node)) {
        return nullptr;
    }

    parse_token_info_t *tok = core_tree_leaf_value(node);

    if (!tok || !core_option_is_set(tok->value)) {
        return nullptr;
    }

    core_string_t s = core_option_get(tok->value);

    return s.data;
}

static const char *
first_leaf_text(parse_tree_t *node)
{
    if (!node) {
        return nullptr;
    }

    if (core_tree_is_leaf(node)) {
        return leaf_text(node);
    }

    size_t nc = core_tree_num_children(node);

    for (size_t i = 0; i < nc; i++) {
        const char *t = first_leaf_text(core_tree_child(node, i));

        if (t) {
            return t;
        }
    }

    return nullptr;
}

static void
test_basic_parse(void)
{
    parse_grammar_t *g = parse_grammar_new();

    int64_t E_id = parse_nonterm_id(parse_nonterm(g, CORE_STRING_STATIC("E")));
    int64_t T_id = parse_nonterm_id(parse_nonterm(g, CORE_STRING_STATIC("T")));

    parse_register_terminal(g, CORE_STRING_STATIC("PLUS"));
    parse_register_terminal(g, CORE_STRING_STATIC("ID"));

    parse_match_t rule1[] = {
        {.kind = PARSE_MATCH_NT, .nt_id = E_id},
        PARSE_TERMINAL(TOK_PLUS),
        {.kind = PARSE_MATCH_NT, .nt_id = T_id},
    };
    parse_add_rule_v(g, E_id, 3, rule1);

    parse_match_t rule2[] = {{.kind = PARSE_MATCH_NT, .nt_id = T_id}};
    parse_add_rule_v(g, E_id, 1, rule2);

    parse_match_t rule3[] = {PARSE_TERMINAL(TOK_ID)};
    parse_add_rule_v(g, T_id, 1, rule3);

    parse_grammar_set_start_id(g, E_id);

    parse_token_info_t tokens[3] = {
        { .tid = TOK_ID,   .index = 0, .line = 1, .column = 1,
          .value = core_option_set(core_string_t, CORE_STRING_STATIC("a")) },
        { .tid = TOK_PLUS, .index = 1, .line = 1, .column = 3,
          .value = core_option_set(core_string_t, CORE_STRING_STATIC("+")) },
        { .tid = TOK_ID,   .index = 2, .line = 1, .column = 5,
          .value = core_option_set(core_string_t, CORE_STRING_STATIC("b")) },
    };

    parse_token_info_t *ptrs[3] = { &tokens[0], &tokens[1], &tokens[2] };

    parse_token_stream_t *ts = scan_token_stream_from_array(ptrs, 3);
    assert(ts);

    parse_pwz_parser_t *p = parse_pwz_new(g);
    assert(p);

    bool ok = parse_pwz_parse(p, ts);
    assert(ok && "parse should succeed");

    parse_tree_t *tree = parse_pwz_get_tree(p);
    assert(tree && "should have a parse tree");

    assert(!core_tree_is_leaf(tree));
    parse_nt_node_t *root = &core_tree_node_value(tree);
    assert(root->id == E_id);
    assert(core_tree_num_children(tree) == 3);

    void *result = parse_tree_walk(g, tree, nullptr);
    (void)result;

    printf("PASS: basic parse of 'a + b'\n");

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_codepoint_parse(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_nonterm_t *S = parse_nonterm(g, CORE_STRING_STATIC("S"));

    parse_add_rule(g, S, PARSE_CHAR('a'), PARSE_CHAR('b'));

    parse_grammar_set_start(g, S);

    core_string_t input = CORE_STRING_STATIC("ab");
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(input);
    assert(ts);

    parse_pwz_parser_t *p = parse_pwz_new(g);
    bool ok = parse_pwz_parse(p, ts);
    assert(ok && "codepoint parse should succeed");

    parse_tree_t *tree = parse_pwz_get_tree(p);
    assert(tree);

    printf("PASS: codepoint parse of 'ab'\n");

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_repetition_group_flattening(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_nonterm_t *S = parse_nonterm(g, CORE_STRING_STATIC("S"));

    parse_add_rule(g, S, parse_plus_group(g, PARSE_ANY()));
    parse_grammar_set_start(g, S);

    parse_token_stream_t *ts
        = scan_token_stream_from_codepoints(CORE_STRING_STATIC("abcd"));
    parse_pwz_parser_t *p = parse_pwz_new(g);
    assert(parse_pwz_parse(p, ts));

    parse_tree_t *tree = parse_pwz_get_tree(p);
    assert(tree && core_tree_num_children(tree) == 1);

    parse_tree_t *group = core_tree_child(tree, 0);
    assert(group && !core_tree_is_leaf(group));
    assert(core_tree_num_children(group) == 4);

    for (size_t i = 0; i < 4; i++) {
        const char *text
            = first_leaf_text(core_tree_child(group, i));
        assert(text && text[0] == "abcd"[i] && text[1] == 0);
    }

    printf("PASS: ordered repetition group flattening\n");

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_forest(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_nonterm_t *S = parse_nonterm(g, CORE_STRING_STATIC("S"));

    parse_add_rule(g, S, PARSE_NT(S), PARSE_NT(S));
    parse_add_rule(g, S, PARSE_CHAR('a'));

    parse_grammar_set_start(g, S);

    core_string_t input = CORE_STRING_STATIC("aaa");
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(input);

    parse_forest_t forest = parse_pwz_parse_grammar(g, ts);

    int32_t count = parse_forest_count(&forest);
    assert(count >= 1 && "should have at least one parse");

    printf("PASS: ambiguous parse of 'aaa' (%d trees)\n", count);

    parse_forest_free(&forest);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_set_match(void)
{
    static const int64_t excluded[] = {TOK_PLUS};

    static const parse_match_set_t not_plus = {
        .ids    = excluded,
        .n      = 1,
        .negate = true,
    };

    static const parse_match_set_t only_plus = {
        .ids    = excluded,
        .n      = 1,
        .negate = false,
    };

    assert(parse_match_set_holds(&only_plus, TOK_PLUS));
    assert(!parse_match_set_holds(&only_plus, TOK_ID));
    assert(!parse_match_set_holds(&not_plus, TOK_PLUS));
    assert(parse_match_set_holds(&not_plus, TOK_ID));

    parse_grammar_t *g = parse_grammar_new();
    int64_t        S = parse_nonterm_id(parse_nonterm(g,
                                                  CORE_STRING_STATIC("S")));

    parse_register_terminal(g, CORE_STRING_STATIC("PLUS"));
    parse_register_terminal(g, CORE_STRING_STATIC("ID"));

    parse_match_t rule[] = {PARSE_SET(&not_plus), PARSE_TERMINAL(TOK_PLUS)};

    parse_add_rule_v(g, S, 2, rule);
    parse_grammar_set_start_id(g, S);

    parse_token_info_t ok[2] = {
        {.tid = TOK_ID, .index = 0, .line = 1, .column = 1},
        {.tid = TOK_PLUS, .index = 1, .line = 1, .column = 2},
    };
    parse_token_info_t bad[2] = {
        {.tid = TOK_PLUS, .index = 0, .line = 1, .column = 1},
        {.tid = TOK_PLUS, .index = 1, .line = 1, .column = 2},
    };

    parse_token_info_t *okp[2]  = {&ok[0], &ok[1]};
    parse_token_info_t *badp[2] = {&bad[0], &bad[1]};

    parse_token_stream_t *ts = scan_token_stream_from_array(okp, 2);
    parse_pwz_parser_t   *p  = parse_pwz_new(g);

    assert(parse_pwz_parse(p, ts) && "a token outside the excluded set matches");
    parse_pwz_free(p);
    scan_token_stream_free(ts);

    ts = scan_token_stream_from_array(badp, 2);
    p  = parse_pwz_new(g);

    assert(!parse_pwz_parse(p, ts) && "an excluded token does not match");
    parse_pwz_free(p);
    scan_token_stream_free(ts);

    parse_grammar_free(g);

    printf("PASS: negated and plain token sets match\n");
}

int
main(void)
{
    test_set_match();
    test_basic_parse();
    test_codepoint_parse();
    test_repetition_group_flattening();
    test_forest();

    printf("\nAll PWZ smoke tests passed.\n");
    return 0;
}
