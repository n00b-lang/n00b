#include <stdio.h>
#include <stdlib.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/pwz.h"
#include "parse/tree_print.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static int64_t nt_id(parse_grammar_t *g, core_string_t name)
{
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        if (g->nt_list.data[i]->name.data
            && core_string_eq(g->nt_list.data[i]->name, name)) {
            return g->nt_list.data[i]->id;
        }
    }

    return -1;
}

static parse_token_info_t *tk_s(int64_t tid, core_string_t text, int i)
{
    parse_token_info_t *t = parse_token_new();

    parse_token_set_tid(t, (int32_t)tid);
    parse_token_set_index(t, i);
    parse_token_set_position(t, 1, 0, 0, 0);
    parse_token_set_value(t, core_option_set(core_string_t,
                                             core_string_dup(text)),
                          false);

    return t;
}

#define tk(tid, text, i) tk_s((tid), S(text), (i))

static core_string_t lay_out(parse_grammar_t *g, core_string_t start,
                             parse_token_info_t **toks, int n, int32_t width)
{
    parse_token_stream_t *ts = scan_token_stream_from_array(toks, n);
    parse_pwz_parser_t   *p
        = parse_pwz_new_for_start(g, nt_id(g, start));
    core_string_t         out = {};

    if (parse_pwz_parse(p, ts)) {
        parse_print_opts_t o = {.width = width, .indent = 2};

        out = parse_tree_print(g, parse_pwz_get_tree(p), &o);
    }

    parse_pwz_free(p);
    scan_token_stream_free(ts);

    return out;
}

static void same(core_string_t what, core_string_t got, core_string_t want)
{
    if (!got.data || !core_string_eq(got, want)) {
        core_string_t shown = got.data ? got : S("(nothing)");

        fails++;
        printf("FAIL  %.*s\n---- want ----\n%.*s\n---- got ----\n%.*s\n",
               (int)what.u8_bytes, what.data, (int)want.u8_bytes, want.data,
               (int)shown.u8_bytes, shown.data);
    }
    core_free(got.data);
}

static parse_grammar_t *load(core_string_t text, core_string_t start)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(text, start, g)) {
        check("the grammar loads", false);

        return nullptr;
    }

    return g;
}

#define CALLS                                                                \
    S("<call> @group @nospace($1) @indent($2) @softline($2) @softline($3) ::=" \
      " %IDENTIFIER %\"(\" <args>? %\")\"\n"                                 \
      "<args> @newline @nospace($1) ::= <arg> (%\",\" <arg>)*\n"             \
      "<arg>  ::= %IDENTIFIER\n")

static void test_a_group_stays_flat_while_it_fits(void)
{
    parse_grammar_t *g = load(CALLS, S("call"));

    if (!g) {
        return;
    }

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "f", 0),
        tk('(', "(", 1),
        tk(PARSE_TOK_IDENTIFIER, "alpha", 2),
        tk(',', ",", 3),
        tk(PARSE_TOK_IDENTIFIER, "beta", 4),
        tk(',', ",", 5),
        tk(PARSE_TOK_IDENTIFIER, "gamma", 6),
        tk(')', ")", 7),
    };

    same(S("a call that fits stays on one line"),
         lay_out(g, S("call"), toks, 8, 80), S("f(alpha, beta, gamma)"));

    same(S("the same call breaks when it does not fit"),
         lay_out(g, S("call"), toks, 8, 12),
         S("f(\n  alpha,\n  beta,\n  gamma\n)"));

    parse_grammar_free(g);
}

#define ALIGNED                                                       \
    S("<pair> @nospace($1) @align($2) ::= %IDENTIFIER %\":\" <call>\n" \
      "<call> @group @nospace($1) @newline($2) @newline($3) ::="      \
      " %IDENTIFIER %\"(\" <args>? %\")\"\n"                          \
      "<args> @newline @nospace($1) ::= <arg> (%\",\" <arg>)*\n"      \
      "<arg>  ::= %IDENTIFIER\n")

static void test_align_hangs_under_the_column_it_names(void)
{
    parse_grammar_t *g = load(ALIGNED, S("pair"));

    if (!g) {
        return;
    }

    parse_token_info_t *toks[] = {
        tk(PARSE_TOK_IDENTIFIER, "key", 0),
        tk(':', ":", 1),
        tk(PARSE_TOK_IDENTIFIER, "f", 2),
        tk('(', "(", 3),
        tk(PARSE_TOK_IDENTIFIER, "alpha", 4),
        tk(',', ",", 5),
        tk(PARSE_TOK_IDENTIFIER, "beta", 6),
        tk(')', ")", 7),
    };

    same(S("a broken value hangs under the column align names"),
         lay_out(g, S("pair"), toks, 8, 10),
         S("key: f(\n     alpha,\n     beta\n     )"));

    parse_grammar_free(g);
}

#define BLOCKS                                               \
    S("<prog> @blankline($1) @indent($1) @hardline($2) ::="  \
      " %\"{\" <stmt>* %\"}\"\n"                             \
      "<stmt> @nospace($2) ::= %IDENTIFIER <arg>* %\";\"\n"  \
      "<arg>  ::= %IDENTIFIER\n")

static void test_blank_lines_and_indentation_come_from_the_grammar(void)
{
    parse_grammar_t *g = load(BLOCKS, S("prog"));

    if (!g) {
        return;
    }

    parse_token_info_t *toks[] = {
        tk('{', "{", 0),
        tk(PARSE_TOK_IDENTIFIER, "one", 1),
        tk(PARSE_TOK_IDENTIFIER, "x", 2),
        tk(';', ";", 3),
        tk(PARSE_TOK_IDENTIFIER, "two", 4),
        tk(';', ";", 5),
        tk('}', "}", 6),
    };

    same(S("statements indent and separate as the annotations ask"),
         lay_out(g, S("prog"), toks, 7, 80),
         S("{\n\n  one x;\n\n  two;\n}"));

    parse_grammar_free(g);
}

int main(void)
{
    test_a_group_stays_flat_while_it_fits();
    test_align_hangs_under_the_column_it_names();
    test_blank_lines_and_indentation_come_from_the_grammar();

    return report("tree print");
}
