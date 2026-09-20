#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "harness.h"

static const char *const N00B_TOKENS[] = {
    "CHAR_LIT", "EMBED",   "FLOAT_LIT", "HEX_LIT",
    "INT_LIT",  "LITERAL_PREFIX", "NEWLINE", "STRING_LIT",
    "INDENT",   "DEDENT",
};

static void declare_tokens(parse_grammar_t *g)
{
    for (size_t i = 0; i < sizeof(N00B_TOKENS) / sizeof(N00B_TOKENS[0]); i++) {
        parse_register_terminal(g, core_string_from_cstr(N00B_TOKENS[i]));
    }
}

static bool loads(const char *text, const char *start)
{
    parse_grammar_t *g = parse_grammar_new();

    declare_tokens(g);

    bool ok = parse_bnf_load(core_string_from_cstr(text),
                           core_string_from_cstr(start), g);

    parse_grammar_free(g);

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
    core_string_t text = slurp("../../grammars/n00b.bnf");

    if (!text.data) {
        return;
    }

    parse_grammar_t *g = parse_grammar_new();

    declare_tokens(g);
    check("slay's annotated grammar loads whole",
          parse_bnf_load(text, CORE_STRING_STATIC("module"), g));
    parse_grammar_free(g);

    parse_grammar_t *bare = parse_grammar_new();

    check("and is refused when its token names are not declared",
          !parse_bnf_load(text, CORE_STRING_STATIC("module"), bare));
    parse_grammar_free(bare);
}

int main(void)
{
    test_the_annotation_forms_slay_writes();
    test_a_rule_may_wrap_after_the_arrow();
    test_the_slay_grammar_loads();

    return report("fe bnf/slay");
}
