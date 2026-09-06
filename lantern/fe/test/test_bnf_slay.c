#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/grammar.h"

static int fails = 0;

static void check(const char *what, bool ok)
{
    if (!ok) {
        fails++;
        printf("FAIL  %s\n", what);
    }
}

static ncc_string_t slurp(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        printf("FAIL  cannot open %s\n", path);
        fails++;

        return ncc_string_empty();
    }

    fseek(f, 0, SEEK_END);

    long n = ftell(f);

    rewind(f);

    char *p = malloc((size_t)n + 1);

    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        p[0] = 0;
    }
    else {
        p[n] = 0;
    }
    fclose(f);

    return ncc_string_from_cstr(p);
}

static const char *const N00B_TOKENS[] = {
    "CHAR_LIT", "EMBED",   "FLOAT_LIT", "HEX_LIT",
    "INT_LIT",  "NEWLINE", "STRING_LIT",
};

static void declare_tokens(ncc_grammar_t *g)
{
    for (size_t i = 0; i < sizeof(N00B_TOKENS) / sizeof(N00B_TOKENS[0]); i++) {
        ncc_register_terminal(g, ncc_string_from_cstr(N00B_TOKENS[i]));
    }
}

static bool loads(const char *text, const char *start)
{
    ncc_grammar_t *g = ncc_grammar_new();

    declare_tokens(g);

    bool ok = ncc_bnf_load(ncc_string_from_cstr(text),
                           ncc_string_from_cstr(start), g);

    ncc_grammar_free(g);

    return ok;
}

static void test_the_annotation_forms_slay_writes(void)
{
    static const struct {
        const char *what;
        const char *src;
    } cases[] = {
        {"no arguments", "<a> @notrivia ::= %\"x\"\n"},
        {"a string argument", "<a> @scope(\"module\") ::= %\"x\"\n"},
        {"a child reference", "<a> @varref($0) ::= %\"x\"\n"},
        {"two child references", "<a> @assigns($0,$2) ::= %\"x\"\n"},
        {"children and a string", "<a> @declares($0,$2,\"param\") ::= %\"x\"\n"},
        {"several on one rule",
         "<a> @scope(\"if\") @branch($1,$2,$3) @infer(\"$1 unify $2\") "
         "::= %\"x\"\n"},
        {"a file-level annotation", "@tokenizer(\"n00b\")\n<a> ::= %\"x\"\n"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (!loads(cases[i].src, "a")) {
            printf("FAIL  a rule annotated with %s does not load\n",
                   cases[i].what);
            fails++;
        }
    }
    check("every annotation form the slay grammar uses is accepted", true);
}

static void test_a_rule_may_wrap_after_the_arrow(void)
{
    check("a rule whose right side starts on the next line loads",
          loads("<a> ::=\n      %\"x\"\n", "a"));
    check("and one that wraps after several annotations",
          loads("<a> @scope(\"if\") @branch($1,$2,$3) ::=\n     %\"x\"\n",
                "a"));
    check("and the alternative-continuation form still works",
          loads("<a> ::= %\"x\"\n    | %\"y\"\n", "a"));
}

static void test_the_slay_grammar_loads(void)
{
    ncc_string_t text = slurp("n00b.bnf");

    if (!text.data) {
        return;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    declare_tokens(g);
    check("slay's annotated grammar loads whole",
          ncc_bnf_load(text, ncc_string_from_cstr("module"), g));
    ncc_grammar_free(g);

    ncc_grammar_t *bare = ncc_grammar_new();

    check("and is refused when its token names are not declared",
          !ncc_bnf_load(text, ncc_string_from_cstr("module"), bare));
    ncc_grammar_free(bare);
}

int main(void)
{
    test_the_annotation_forms_slay_writes();
    test_a_rule_may_wrap_after_the_arrow();
    test_the_slay_grammar_loads();

    if (!fails) {
        printf("fe bnf/slay: all checks passed\n");
    }

    return fails ? 1 : 0;
}
