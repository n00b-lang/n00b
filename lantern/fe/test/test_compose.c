#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_visit.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"

static int fails;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static const char *const TYPES =
    "<type>  ::= <name> | <ptr>\n"
    "<ptr>   ::= %\"ptr\" %\"[\" <type> %\"]\"\n"
    "<name>  ::= %IDENTIFIER\n";

static const char *const LANG =
    "<decl>  ::= %\"local\" %IDENTIFIER %\":\" <type>\n"
    "<decls> ::= <decl> | <decl> <decls>\n";

static int64_t nt_id(ncc_grammar_t *g, const char *name)
{
    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        if (g->nt_list.data[i]->name.data
            && !strcmp(g->nt_list.data[i]->name.data, name)) {
            return g->nt_list.data[i]->id;
        }
    }

    return -1;
}

static ncc_token_info_t *tk(int64_t tid, const char *text, int i)
{
    ncc_token_info_t *t = calloc(1, sizeof(*t));

    t->tid   = tid;
    t->index = i;
    t->line  = 1;
    t->value = ncc_option_set(ncc_string_t, ncc_string_from_cstr(text));

    return t;
}

static bool parses(ncc_grammar_t *g, const char *start,
                   ncc_token_info_t **toks, int n)
{
    ncc_grammar_set_start_id(g, nt_id(g, start));

    ncc_token_stream_t *ts = ncc_token_stream_from_array(toks, n);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);
    bool                ok = ncc_pwz_parse(p, ts);

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return ok;
}

static ncc_grammar_t *compose(const char *first, const char *second,
                              const char *start)
{
    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    bool a = ncc_bnf_load(ncc_string_from_cstr(first),
                          ncc_string_from_cstr(start), g);
    bool b = ncc_bnf_load(ncc_string_from_cstr(second),
                          ncc_string_from_cstr(start), g);

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
        ncc_grammar_t *g = compose(orders[i].first, orders[i].second, "decls");
        int            undefined = 0;

        for (size_t k = 0; k < ncc_list_len(g->nt_list); k++) {
            ncc_nonterm_t *nt = g->nt_list.data[k];

            if (nt->group_nt || !nt->name.data || nt->name.data[0] == '$') {
                continue;
            }

            if (!ncc_list_len(nt->rule_ids)) {
                undefined++;
            }
        }

        if (undefined) {
            printf("FAIL  %s leaves %d nonterminal(s) undefined\n",
                   orders[i].what, undefined);
            fails++;
        }

        ncc_grammar_free(g);
    }
}

static void test_one_grammar_parses_from_several_start_symbols(void)
{
    ncc_grammar_t *g = compose(TYPES, LANG, "decls");

    int64_t kw_local = ncc_lookup_terminal(g, ncc_string_from_cstr("local"));
    int64_t kw_ptr   = ncc_lookup_terminal(g, ncc_string_from_cstr("ptr"));

    ncc_token_info_t *bare[] = {tk(NCC_TOK_IDENTIFIER, "int", 0)};

    check("a type parses on its own", parses(g, "type", bare, 1));

    ncc_token_info_t *decl[] = {
        tk(kw_local, "local", 0),
        tk(NCC_TOK_IDENTIFIER, "x", 1),
        tk(':', ":", 2),
        tk(NCC_TOK_IDENTIFIER, "int", 3),
    };

    check("and the same grammar parses what contains it",
          parses(g, "decl", decl, 4));

    ncc_token_info_t *nested[] = {
        tk(kw_local, "local", 0),
        tk(NCC_TOK_IDENTIFIER, "p", 1),
        tk(':', ":", 2),
        tk(kw_ptr, "ptr", 3),
        tk('[', "[", 4),
        tk(NCC_TOK_IDENTIFIER, "int", 5),
        tk(']', "]", 6),
    };

    check("including a structured type from the other file",
          parses(g, "decl", nested, 7));

    check("and going back to the smaller start still works",
          parses(g, "type", bare, 1));

    ncc_grammar_free(g);
}

static void test_a_missing_part_is_still_reported(void)
{
    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);
    ncc_bnf_load(ncc_string_from_cstr(LANG), ncc_string_from_cstr("decls"), g);

    check("a nonterminal no loaded part defines stays undefined",
          ncc_list_len(ncc_get_nonterm(g, nt_id(g, "type"))->rule_ids) == 0);
    ncc_grammar_free(g);
}

static void *r_int(ncc_reduce_t *r)
{
    (void)r;

    return (void *)(uintptr_t)1;
}

static void *r_sum(ncc_reduce_t *r)
{
    if (r->n_kids != 2) {
        printf("FAIL  <list> saw %zu children, not the 2 its production has\n",
               r->n_kids);
        fails++;

        return (void *)(uintptr_t)0;
    }

    uintptr_t total = (uintptr_t)r->kids[0];

    ncc_reduce_list_t *rest = ncc_reduce_as_list(r->kids[1]);

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

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);
    ncc_bnf_load(ncc_string_from_cstr(G), ncc_string_from_cstr("list"), g);

    ncc_token_info_t *toks[] = {
        tk(NCC_TOK_IDENTIFIER, "a", 0),
        tk(',', ",", 1),
        tk(NCC_TOK_IDENTIFIER, "b", 2),
        tk(',', ",", 3),
        tk(NCC_TOK_IDENTIFIER, "c", 4),
    };

    ncc_token_stream_t *ts = ncc_token_stream_from_array(toks, 5);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    check("the list parses", ncc_pwz_parse(p, ts));

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, nullptr);

    ncc_walk_reduce_nt(w, ncc_string_from_cstr("item"), r_int, nullptr);
    ncc_walk_reduce_nt(w, ncc_string_from_cstr("list"), r_sum, nullptr);

    uintptr_t got = (uintptr_t)ncc_tree_walk_reduce(w, ncc_pwz_get_tree(p));

    if (got != 3) {
        printf("FAIL  a reduce over a quantified group saw %llu items, not 3\n",
               (unsigned long long)got);
        fails++;
    }

    ncc_tree_walk_free(w);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_grammar_free(g);
}

int main(void)
{
    test_the_parts_load_in_either_order();
    test_one_grammar_parses_from_several_start_symbols();
    test_a_missing_part_is_still_reported();
    test_a_reduce_folds_children_into_a_value();

    if (!fails) {
        printf("fe compose: all checks passed\n");
    }

    return fails ? 1 : 0;
}
