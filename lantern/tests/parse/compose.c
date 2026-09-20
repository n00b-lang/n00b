#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_visit.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

static const char *const TYPES =
    "<type>  ::= <name> | <ptr>\n"
    "<ptr>   ::= %\"ptr\" %\"[\" <type> %\"]\"\n"
    "<name>  ::= %IDENTIFIER\n";

static const char *const LANG =
    "<decl>  ::= %\"local\" %IDENTIFIER %\":\" <type>\n"
    "<decls> ::= <decl> | <decl> <decls>\n";

static int64_t nt_id(parse_grammar_t *g, const char *name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (g->nt_list.data[i]->name.data
            && !strcmp(g->nt_list.data[i]->name.data, name)) {
            return g->nt_list.data[i]->id;
        }
    }

    return -1;
}

static parse_token_info_t *tk(int64_t tid, const char *text, int i)
{
    parse_token_info_t *t = core_alloc(typeof(*t));

    t->tid   = tid;
    t->index = i;
    t->line  = 1;
    t->value = core_option_set(core_string_t, core_string_from_cstr(text));

    return t;
}

static bool parses(parse_grammar_t *g, const char *start,
                   parse_token_info_t **toks, int n)
{
    parse_token_stream_t *ts = scan_token_stream_from_array(toks, n);
    parse_pwz_parser_t   *p
        = parse_pwz_new_for_start(g, nt_id(g, start));
    bool                ok = parse_pwz_parse(p, ts);

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return ok;
}

static parse_grammar_t *compose(const char *first, const char *second,
                              const char *start)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool a = parse_bnf_load(core_string_from_cstr(first),
                          core_string_from_cstr(start), g);
    bool b = parse_bnf_load(core_string_from_cstr(second),
                          core_string_from_cstr(start), g);

    check("both parts load into one grammar", a && b);

    return g;
}

static void test_the_parts_load_in_either_order(void)
{
    static const struct {
        const char *what;
        const char *first;
        const char *second;
    } orders[] = {
        {"types first",    TYPES, LANG },
        {"the user first", LANG,  TYPES},
    };

    for (size_t i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        parse_grammar_t *g = compose(orders[i].first, orders[i].second, "decls");

        if (!parse_grammar_report_undefined(g, stdout)) {
            printf("FAIL  %s leaves nonterminals undefined\n",
                   orders[i].what);
            fails++;
        }

        parse_grammar_free(g);
    }
}

static void test_one_grammar_parses_from_several_start_symbols(void)
{
    parse_grammar_t *g = compose(TYPES, LANG, "decls");

    int64_t kw_local = parse_lookup_terminal(g, CORE_STRING_STATIC("local"));
    int64_t kw_ptr   = parse_lookup_terminal(g, CORE_STRING_STATIC("ptr"));

    parse_token_info_t *bare[] = {tk(PARSE_TOK_IDENTIFIER, "int", 0)};

    check("a type parses on its own", parses(g, "type", bare, 1));

    parse_token_info_t *decl[] = {
        tk(kw_local, "local", 0),
        tk(PARSE_TOK_IDENTIFIER, "x", 1),
        tk(':', ":", 2),
        tk(PARSE_TOK_IDENTIFIER, "int", 3),
    };

    check("and the same grammar parses what contains it",
          parses(g, "decl", decl, 4));

    parse_token_info_t *nested[] = {
        tk(kw_local, "local", 0),
        tk(PARSE_TOK_IDENTIFIER, "p", 1),
        tk(':', ":", 2),
        tk(kw_ptr, "ptr", 3),
        tk('[', "[", 4),
        tk(PARSE_TOK_IDENTIFIER, "int", 5),
        tk(']', "]", 6),
    };

    check("including a structured type from the other file",
          parses(g, "decl", nested, 7));

    check("and going back to the smaller start still works",
          parses(g, "type", bare, 1));

    parse_grammar_free(g);
}

static void test_a_missing_part_is_still_reported(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(core_string_from_cstr(LANG), CORE_STRING_STATIC("decls"), g);

    check("a nonterminal no loaded part defines stays undefined",
          core_list_len(parse_get_nonterm(g, nt_id(g, "type"))->rule_ids) == 0);
    parse_grammar_free(g);
}

static void test_a_failed_load_changes_nothing(void)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    check("the transaction baseline grammar loads",
          parse_bnf_load(CORE_STRING_STATIC("<ok> ::= 'x'\n"),
                         CORE_STRING_STATIC("ok"), g));

    size_t nonterms = core_list_len(g->nt_list);
    size_t rules = core_list_len(g->rules);
    size_t terminals = core_list_len(g->named_terms);
    size_t annotations = core_list_len(g->annotations);

    check("a grammar with an undeclared token is rejected",
          !parse_bnf_load(CORE_STRING_STATIC(
                              "@tokenizer(\"json\")\n"
                              "<bad> ::= %MISSING\n"),
                          CORE_STRING_STATIC("bad"), g));
    check("a rejected load adds no nonterminals",
          core_list_len(g->nt_list) == nonterms
              && parse_find_nonterm(g, CORE_STRING_STATIC("bad")) == nullptr);
    check("a rejected load adds no rules, terminals, or annotations",
          core_list_len(g->rules) == rules
              && core_list_len(g->named_terms) == terminals
              && core_list_len(g->annotations) == annotations);
    check("a rejected load does not change the tokenizer",
          !strcmp(scan_grammar_tokenizer(g).data, "text"));

    check("an empty literal is rejected before installing its production",
          !parse_bnf_load(CORE_STRING_STATIC("<empty> ::= \"\"\n"),
                          CORE_STRING_STATIC("empty"), g)
              && parse_find_nonterm(g, CORE_STRING_STATIC("empty")) == nullptr
              && core_list_len(g->rules) == rules);

    parse_grammar_free(g);
}

static void *r_int(parse_reduce_t *r)
{
    (void)r;

    return (void *)(uintptr_t)1;
}

static void *r_sum(parse_reduce_t *r)
{
    if (r->n_kids != 2) {
        printf("FAIL  <list> saw %zu children, not the 2 its production has\n",
               r->n_kids);
        fails++;

        return (void *)(uintptr_t)0;
    }

    uintptr_t total = (uintptr_t)r->kids[0];

    parse_reduce_list_t *rest = parse_reduce_as_list(r->kids[1]);

    if (!rest) {
        printf("FAIL  the repetition did not come through as a list\n");
        fails++;

        return (void *)total;
    }

    for (size_t i = 0; i < rest->n; i++) {
        total += (uintptr_t)rest->v[i];
    }

    return (void *)total;
}

static void test_a_reduce_folds_children_into_a_value(void)
{
    static const char *const G =
        "<list> ::= <item> (%\",\" <item>)*\n"
        "<item> ::= %IDENTIFIER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(core_string_from_cstr(G), CORE_STRING_STATIC("list"), g);

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "a", 0),
        tk(',', ",", 1),
        tk(PARSE_TOK_IDENTIFIER, "b", 2),
        tk(',', ",", 3),
        tk(PARSE_TOK_IDENTIFIER, "c", 4),
    };

    parse_token_stream_t *ts = scan_token_stream_from_array(toks, 5);
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(
        g, nt_id(g, "list"));

    check("the list parses", parse_pwz_parse(p, ts));

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    parse_walk_reduce_nt(w, CORE_STRING_STATIC("item"), r_int, nullptr);
    parse_walk_reduce_nt(w, CORE_STRING_STATIC("list"), r_sum, nullptr);

    uintptr_t got = (uintptr_t)parse_tree_walk_reduce(w, parse_pwz_get_tree(p));

    if (got != 3) {
        printf("FAIL  a reduce over a quantified group saw %llu items, not 3\n",
               (unsigned long long)got);
        fails++;
    }

    parse_tree_walk_free(w);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

static int reduced;

static void *r_count(parse_reduce_t *r)
{
    (void)r;
    reduced++;

    return nullptr;
}

static bool stop_at_item(parse_visit_t *v)
{
    parse_tree_walk_stop(v->walk);

    return true;
}

static void test_a_visitor_can_stop_a_reduce(void)
{
    static const char *const G =
        "<list> ::= <item> (%\",\" <item>)*\n"
        "<item> ::= %IDENTIFIER\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    parse_bnf_load(core_string_from_cstr(G), CORE_STRING_STATIC("list"), g);

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "a", 0),
        tk(',', ",", 1),
        tk(PARSE_TOK_IDENTIFIER, "b", 2),
        tk(',', ",", 3),
        tk(PARSE_TOK_IDENTIFIER, "c", 4),
    };

    parse_token_stream_t *ts = scan_token_stream_from_array(toks, 5);
    parse_pwz_parser_t   *p  = parse_pwz_new_for_start(
        g, nt_id(g, "list"));

    check("the list parses for the stop test", parse_pwz_parse(p, ts));

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    reduced = 0;
    parse_walk_reduce_default(w, r_count, nullptr);
    parse_tree_walk_reduce(w, parse_pwz_get_tree(p));
    check("the whole tree reduces when nothing stops it", reduced > 1);
    parse_tree_walk_free(w);

    int whole = reduced;

    w = parse_tree_walk_new(g, nullptr);

    reduced = 0;
    parse_walk_on_nt(w, CORE_STRING_STATIC("item"), stop_at_item, nullptr,
                   nullptr);
    parse_walk_reduce_default(w, r_count, nullptr);

    void *out = parse_tree_walk_reduce(w, parse_pwz_get_tree(p));

    check("stopping in a visitor halts the reduce", reduced < whole);
    check("and the reduce reports that it stopped",
          parse_tree_walk_stopped(w));
    check("and yields nothing rather than a half-built result",
          out == nullptr);
    parse_tree_walk_free(w);

    parse_pwz_free(p);
    scan_token_stream_free(ts);
    parse_grammar_free(g);
}

int main(void)
{
    test_the_parts_load_in_either_order();
    test_one_grammar_parses_from_several_start_symbols();
    test_a_missing_part_is_still_reported();
    test_a_failed_load_changes_nothing();
    test_a_reduce_folds_children_into_a_value();
    test_a_visitor_can_stop_a_reduce();

    return report("fe compose");
}
