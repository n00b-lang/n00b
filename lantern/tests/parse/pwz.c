
#include "parse/pwz.h"
#include "parse/bnf.h"
#include "private/parse/pwz.h"
#include "scanner/token_stream.h"
#include "lib/alloc.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

    parse_pwz_parser_t *p = parse_pwz_new_for_start(g, E_id);
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


    core_string_t input = CORE_STRING_STATIC("ab");
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(input);
    assert(ts);

    parse_pwz_parser_t *p = parse_pwz_new_for_start(g, parse_nonterm_id(S));
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

    parse_token_stream_t *ts
        = scan_token_stream_from_codepoints(CORE_STRING_STATIC("abcd"));
    parse_pwz_parser_t *p = parse_pwz_new_for_start(g, parse_nonterm_id(S));
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


    core_string_t input = CORE_STRING_STATIC("aaa");
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(input);

    parse_forest_t forest
        = parse_pwz_parse_grammar(g, parse_nonterm_id(S), ts);

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
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(g, S);

    assert(parse_pwz_parse(p, ts) && "a token outside the excluded set matches");
    parse_pwz_free(p);
    scan_token_stream_free(ts);

    ts = scan_token_stream_from_array(badp, 2);
    p  = parse_pwz_new_for_start(g, S);

    assert(!parse_pwz_parse(p, ts) && "an excluded token does not match");
    parse_pwz_free(p);
    scan_token_stream_free(ts);

    parse_grammar_free(g);

    printf("PASS: negated and plain token sets match\n");
}

typedef struct {
    parse_grammar_t *grammar;
    parse_nt_id_t    start;
    core_string_t    input;
    bool             ok;
} parallel_parse_t;

static void *
run_parallel_parse(void *arg)
{
    parallel_parse_t *run = arg;
    parse_token_stream_t *ts = scan_token_stream_from_codepoints(run->input);
    parse_pwz_parser_t *p = parse_pwz_new_for_start(run->grammar, run->start);

    run->ok = p && parse_pwz_parse(p, ts);

    if (run->ok) {
        parse_tree_t *tree = parse_pwz_get_tree(p);

        run->ok = tree && !core_tree_is_leaf(tree)
                  && core_tree_node_value(tree).id == run->start;
    }

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return nullptr;
}

static void
test_parallel_starts(void)
{
    parse_grammar_t *g = parse_grammar_new();
    parse_nonterm_t *a = parse_nonterm(g, CORE_STRING_STATIC("a-start"));
    parse_nonterm_t *b = parse_nonterm(g, CORE_STRING_STATIC("b-start"));

    parse_add_rule(g, a, PARSE_CHAR('a'));
    parse_add_rule(g, b, PARSE_CHAR('b'));
    parse_grammar_finalize(g);

    parallel_parse_t runs[] = {
        {.grammar = g,
         .start = parse_nonterm_id(a),
         .input = CORE_STRING_STATIC("a")},
        {.grammar = g,
         .start = parse_nonterm_id(b),
         .input = CORE_STRING_STATIC("b")},
    };
    pthread_t threads[2];

    assert(!pthread_create(&threads[0], nullptr, run_parallel_parse, &runs[0]));
    assert(!pthread_create(&threads[1], nullptr, run_parallel_parse, &runs[1]));
    assert(!pthread_join(threads[0], nullptr));
    assert(!pthread_join(threads[1], nullptr));
    assert(runs[0].ok && runs[1].ok);

    parse_token_stream_t *ts
        = scan_token_stream_from_codepoints(CORE_STRING_STATIC("a"));
    parse_pwz_parser_t *p
        = parse_pwz_new_for_start(g, parse_nonterm_id(a));

    assert(p && parse_pwz_parse(p, ts));
    assert(core_tree_node_value(parse_pwz_get_tree(p)).id
           == parse_nonterm_id(a));

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    parse_grammar_free(g);
    printf("PASS: parallel parses use independent start symbols\n");
}

typedef struct {
    int32_t calls;
} disambig_probe_t;

static bool
count_disambig_calls(void             *ud,
                     const char       *predicate,
                     parse_token_info_t *tok)
{
    disambig_probe_t *probe = ud;

    assert(probe && predicate && tok);
    assert(strcmp(predicate, "shared") == 0);
    probe->calls++;
    return false;
}

static void
test_memoized_disambiguation(void)
{
    enum { ALTERNATIVES = 128, ITERATIONS = 256 };

    char grammar_text[8192];
    size_t used = (size_t)snprintf(grammar_text,
                                   sizeof(grammar_text),
                                   "<S> ::= <P> <X>\n"
                                   "<P> @disambig(shared, 1) ::= %%\"b\"\n"
                                   "<X> ::= ");
    assert(used < sizeof(grammar_text));

    for (int i = 0; i < ALTERNATIVES; i++) {
        char alt[64];
        int n = snprintf(alt, sizeof(alt), "%s<P> <B%d>",
                         i == 0 ? "" : " | ", i);
        assert(n > 0 && used + (size_t)n + 1 < sizeof(grammar_text));
        memcpy(grammar_text + used, alt, (size_t)n);
        used += (size_t)n;
    }
    grammar_text[used++] = '\n';
    for (int i = 0; i < ALTERNATIVES; i++) {
        int n = snprintf(grammar_text + used,
                         sizeof(grammar_text) - used,
                         "<B%d> ::= %%\"c\"\n",
                         i);
        assert(n > 0 && used + (size_t)n < sizeof(grammar_text));
        used += (size_t)n;
    }
    grammar_text[used] = 0;

    parse_grammar_t *g = parse_grammar_new();
    assert(parse_bnf_load(core_string_borrow(grammar_text, used),
                          CORE_STRING_STATIC("S"),
                          g));

    parse_token_stream_t *ts
        = scan_token_stream_from_codepoints(CORE_STRING_STATIC("bbc"));
    assert(ts);

    parse_pwz_parser_t *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, CORE_STRING_STATIC("S"))));
    assert(p && parse_pwz_parse(p, ts));

    disambig_probe_t probe = {0};
    parse_pwz_set_disambiguator(p, count_disambig_calls, &probe);

    clock_t begin = clock();
    for (int i = 0; i < ITERATIONS; i++) {
        assert(parse_pwz_reextract(p));
    }
    double cached_elapsed = (double)(clock() - begin)
                            / (double)CLOCKS_PER_SEC;
    int32_t cached_calls = probe.calls;

    probe.calls = 0;
    parse_pwz_set_score_cache_enabled(p, false);
    begin = clock();
    for (int i = 0; i < ITERATIONS; i++) {
        assert(parse_pwz_reextract(p));
    }
    double uncached_elapsed = (double)(clock() - begin)
                              / (double)CLOCKS_PER_SEC;

    /* Every alternative shares P at the same input position.  Without the
    * score cache this would invoke the predicate once per alternative for
    * each extraction; memoization reduces it to once per extraction. */
    assert(cached_calls == ITERATIONS);
    assert(probe.calls == ALTERNATIVES * ITERATIONS);
    printf("PASS: memoized disambiguation (%d -> %d calls; %.3fs cached, "
           "%d calls/%.3fs uncached)\n",
           ALTERNATIVES * ITERATIONS,
           cached_calls,
           cached_elapsed,
           probe.calls,
           uncached_elapsed);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static void
test_adjacency_annotation(void)
{
    parse_grammar_t *g = parse_grammar_new();
    assert(parse_bnf_load(
        CORE_STRING_STATIC("<S> @adjacent(\"prefix\") ::= %\"(\" %\"x\"\n"),
        CORE_STRING_STATIC("S"), g));
    parse_pwz_parser_t *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, CORE_STRING_STATIC("S"))));
    assert(p);

    parse_token_info_t adjacent_tokens[2] = {
        {.tid = '(', .index = 0, .start_byte = 0, .end_byte = 1},
        {.tid = 'x', .index = 1, .start_byte = 1, .end_byte = 2},
    };
    parse_token_info_t *adjacent_ptrs[2] = {
        &adjacent_tokens[0], &adjacent_tokens[1],
    };
    parse_token_stream_t *ts = scan_token_stream_from_array(adjacent_ptrs, 2);
    assert(ts && parse_pwz_parse(p, ts));
    scan_token_stream_free(ts);

    parse_token_info_t spaced_tokens[2] = {
        {.tid = '(', .index = 0, .start_byte = 0, .end_byte = 1},
        {.tid = 'x', .index = 1, .start_byte = 2, .end_byte = 3},
    };
    parse_token_info_t *spaced_ptrs[2] = {
        &spaced_tokens[0], &spaced_tokens[1],
    };
    ts = scan_token_stream_from_array(spaced_ptrs, 2);
    assert(ts && !parse_pwz_parse(p, ts));
    scan_token_stream_free(ts);

    parse_pwz_free(p);
    parse_grammar_free(g);
    printf("PASS: grammar adjacency assertion\n");
}

static void
test_nullable_nonterminal_before_following_token(void)
{
    parse_grammar_t *g = parse_grammar_new();
    assert(parse_bnf_load(
        CORE_STRING_STATIC("<S> ::= %\"a\" %\"b\" <tail> %\"c\"\n"
                           "<tail> ::= <clause>* <otherwise>?\n"
                           "<clause> ::= %\"d\" %\"b\"\n"
                           "<otherwise> ::= %\"e\" %\"c\"\n"),
        CORE_STRING_STATIC("S"), g));
    parse_pwz_parser_t *p = parse_pwz_new_for_start(
        g, parse_nonterm_id(parse_find_nonterm(g, CORE_STRING_STATIC("S"))));
    assert(p);

    parse_token_info_t tokens[3] = {
        {.tid = 'a', .index = 0},
        {.tid = 'b', .index = 1},
        {.tid = 'c', .index = 2},
    };
    parse_token_info_t *ptrs[3] = {&tokens[0], &tokens[1], &tokens[2]};
    parse_token_stream_t *ts = scan_token_stream_from_array(ptrs, 3);
    assert(ts && parse_pwz_parse(p, ts));

    scan_token_stream_free(ts);
    parse_pwz_free(p);
    parse_grammar_free(g);
    printf("PASS: nullable nonterminal before following token\n");
}

int
main(void)
{
    test_set_match();
    test_basic_parse();
    test_codepoint_parse();
    test_repetition_group_flattening();
    test_forest();
    test_parallel_starts();
    test_memoized_disambiguation();
    test_adjacency_annotation();
    test_nullable_nonterminal_before_following_token();

    printf("\nAll PWZ smoke tests passed.\n");
    return 0;
}
