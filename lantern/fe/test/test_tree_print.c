#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_print.h"
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
    ncc_token_info_t *t = ncc_alloc(typeof(*t));

    t->tid   = (int32_t)tid;
    t->index = i;
    t->line  = 1;
    t->value = ncc_option_set(ncc_string_t, ncc_string_from_cstr(text));

    return t;
}

static char *lay_out(ncc_grammar_t *g, const char *start,
                     ncc_token_info_t **toks, int n, int32_t width)
{
    ncc_grammar_set_start_id(g, nt_id(g, start));

    ncc_token_stream_t *ts = ncc_token_stream_from_array(toks, n);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);
    char               *out = nullptr;

    if (ncc_pwz_parse(p, ts)) {
        ncc_print_opts_t o = {.width = width, .indent = 2};

        out = ncc_tree_print(g, ncc_pwz_get_tree(p), &o);
    }

    ncc_pwz_free(p);
    ncc_token_stream_free(ts);

    return out;
}

static void same(const char *what, char *got, const char *want)
{
    if (!got || strcmp(got, want)) {
        fails++;
        printf("FAIL  %s\n---- want ----\n%s\n---- got ----\n%s\n", what,
               want, got ? got : "(nothing)");
    }
    ncc_free(got);
}

static ncc_grammar_t *load(const char *text, const char *start)
{
    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    if (!ncc_bnf_load(ncc_string_from_cstr(text),
                      ncc_string_from_cstr(start), g)) {
        check("the grammar loads", false);

        return nullptr;
    }

    return g;
}

static const char *const CALLS =
    "<call> @group @nospace($1) @indent($2) @softline($2) @softline($3) ::="
    " %IDENTIFIER %\"(\" <args>? %\")\"\n"
    "<args> @newline @nospace($1) ::= <arg> (%\",\" <arg>)*\n"
    "<arg>  ::= %IDENTIFIER\n";

static void test_a_group_stays_flat_while_it_fits(void)
{
    ncc_grammar_t *g = load(CALLS, "call");

    if (!g) {
        return;
    }

    ncc_token_info_t *toks[] = {
        tk(NCC_TOK_IDENTIFIER, "f", 0),
        tk('(', "(", 1),
        tk(NCC_TOK_IDENTIFIER, "alpha", 2),
        tk(',', ",", 3),
        tk(NCC_TOK_IDENTIFIER, "beta", 4),
        tk(',', ",", 5),
        tk(NCC_TOK_IDENTIFIER, "gamma", 6),
        tk(')', ")", 7),
    };

    same("a call that fits stays on one line",
         lay_out(g, "call", toks, 8, 80), "f(alpha, beta, gamma)");

    same("the same call breaks when it does not fit",
         lay_out(g, "call", toks, 8, 12),
         "f(\n  alpha,\n  beta,\n  gamma\n)");

    ncc_grammar_free(g);
}

static const char *const ALIGNED =
    "<pair> @nospace($1) @align($2) ::= %IDENTIFIER %\":\" <call>\n"
    "<call> @group @nospace($1) @newline($2) @newline($3) ::="
    " %IDENTIFIER %\"(\" <args>? %\")\"\n"
    "<args> @newline @nospace($1) ::= <arg> (%\",\" <arg>)*\n"
    "<arg>  ::= %IDENTIFIER\n";

static void test_align_hangs_under_the_column_it_names(void)
{
    ncc_grammar_t *g = load(ALIGNED, "pair");

    if (!g) {
        return;
    }

    ncc_token_info_t *toks[] = {
        tk(NCC_TOK_IDENTIFIER, "key", 0),
        tk(':', ":", 1),
        tk(NCC_TOK_IDENTIFIER, "f", 2),
        tk('(', "(", 3),
        tk(NCC_TOK_IDENTIFIER, "alpha", 4),
        tk(',', ",", 5),
        tk(NCC_TOK_IDENTIFIER, "beta", 6),
        tk(')', ")", 7),
    };

    same("a broken value hangs under the column align names",
         lay_out(g, "pair", toks, 8, 10),
         "key: f(\n     alpha,\n     beta\n     )");

    ncc_grammar_free(g);
}

static const char *const BLOCKS =
    "<prog> @blankline($1) @indent($1) @hardline($2) ::="
    " %\"{\" <stmt>* %\"}\"\n"
    "<stmt> @nospace($2) ::= %IDENTIFIER <arg>* %\";\"\n"
    "<arg>  ::= %IDENTIFIER\n";

static void test_blank_lines_and_indentation_come_from_the_grammar(void)
{
    ncc_grammar_t *g = load(BLOCKS, "prog");

    if (!g) {
        return;
    }

    ncc_token_info_t *toks[] = {
        tk('{', "{", 0),
        tk(NCC_TOK_IDENTIFIER, "one", 1),
        tk(NCC_TOK_IDENTIFIER, "x", 2),
        tk(';', ";", 3),
        tk(NCC_TOK_IDENTIFIER, "two", 4),
        tk(';', ";", 5),
        tk('}', "}", 6),
    };

    same("statements indent and separate as the annotations ask",
         lay_out(g, "prog", toks, 7, 80),
         "{\n\n  one x;\n\n  two;\n}");

    ncc_grammar_free(g);
}

int main(void)
{
    test_a_group_stays_flat_while_it_fits();
    test_align_hangs_under_the_column_it_names();
    test_blank_lines_and_indentation_come_from_the_grammar();

    if (!fails) {
        printf("tree print: all checks passed\n");
    }

    return fails ? 1 : 0;
}
