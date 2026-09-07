#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/alloc.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "private/parse/grammar.h"
#include "harness.h"

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static const char *const EXTRA_TOKENS[] = {
    "CHAR_LIT",  "EMBED",      "FLOAT_LIT", "HEX_LIT",
    "INT_LIT",   "NEWLINE",    "STRING_LIT",
};

static bool
load_ok(const char *text, const char *start)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool ok = parse_bnf_load(S(text), S(start), g);

    parse_grammar_free(g);

    return ok;
}

static size_t
annot_total(parse_grammar_t *g)
{
    size_t n = parse_grammar_annot_count(g);

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        n += parse_nt_annot_count(g->nt_list.data[i]);
    }

    for (size_t i = 0; i < core_list_len(g->rules); i++) {
        n += parse_rule_annot_count(&g->rules.data[i]);
    }

    return n;
}

static char *
reprint_counting(core_string_t text, const char *start, const char *what,
                 size_t *annots, size_t *rules)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    for (size_t i = 0; i < sizeof(EXTRA_TOKENS) / sizeof(EXTRA_TOKENS[0]);
         i++) {
        parse_register_terminal(g, S(EXTRA_TOKENS[i]));
    }

    if (!parse_bnf_load(text, S(start), g)) {
        printf("FAIL  %s does not load\n", what);
        fails++;
        parse_grammar_free(g);

        return nullptr;
    }

    char *out = parse_bnf_print(g, parse_bnf_last_unit());

    if (annots) {
        *annots = annot_total(g);
    }

    if (rules) {
        *rules = core_list_len(g->rules);
    }

    parse_grammar_free(g);

    return out;
}

static char *
reprint(core_string_t text, const char *start, const char *what)
{
    return reprint_counting(text, start, what, nullptr, nullptr);
}

static void
round_trips(const char *path, const char *start)
{
    core_string_t text = slurp(path);

    if (!text.data) {
        return;
    }

    size_t a1 = 0, a2 = 0, r1 = 0, r2 = 0;

    char *once = reprint_counting(text, start, path, &a1, &r1);

    if (!once) {
        return;
    }

    char *twice = reprint_counting(core_string_from_cstr(once), start,
                                   "the reprint", &a2, &r2);

    if (!twice) {
        printf("---- %s reprinted ----\n%s\n", path, once);
        core_free(once);

        return;
    }

    if (!a1) {
        printf("FAIL  %s has no annotations, so it proves nothing\n", path);
        fails++;
    }

    if (a1 != a2) {
        printf("FAIL  %s: %zu annotations became %zu across the reprint\n",
               path,
               a1,
               a2);
        fails++;
    }

    if (r1 != r2) {
        printf("FAIL  %s: %zu rules became %zu across the reprint\n",
               path,
               r1,
               r2);
        fails++;
    }

    if (strcmp(once, twice) != 0) {
        printf("FAIL  %s does not reach a fixpoint\n", path);
        fails++;

        const char *a = once;
        const char *b = twice;

        while (*a && *a == *b) {
            a++;
            b++;
        }

        printf("  first difference at byte %zu\n", (size_t)(a - once));
        printf("  once:  %.90s\n", a);
        printf("  twice: %.90s\n", b);
    }

    core_free(once);
    core_free(twice);
}

static void
one_line(const char *src, const char *start, const char *want)
{
    char *out = reprint(S(src), start, src);

    if (!out) {
        return;
    }

    if (!strstr(out, want)) {
        printf("FAIL  reprinting %s", src);
        printf("  wanted to see: %s\n", want);
        printf("  got:\n%s\n", out);
        fails++;
    }

    core_free(out);
}

int
main(void)
{
    one_line("<top> @nospace($1) ::= <n> <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @nospace($1) ::= <n> <n>");

    one_line("<top> @declares($1, $2, \"var\") ::= <n> <n>\n"
             "<n> ::= __LOWER\n",
             "top",
             "<top> @declares($1, $2, \"var\") ::= <n> <n>");

    one_line("<top> @scope(\"module\", $1) ::= <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @scope(\"module\", $1) ::= <n>");

    one_line("<top> @literal(\"int\", $n) ::= <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @literal(\"int\", $n) ::= <n>");

    one_line("<top> @infer($0 unify $1) ::= <n> <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @infer($0 unify $1) ::= <n> <n>");

    one_line("<top> @infer(\"$0 unify $1\") ::= <n> <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @infer(\"$0 unify $1\") ::= <n> <n>");

    one_line("<top> @disambig(pred(a, 2), 5) ::= <n>\n<n> ::= __LOWER\n",
             "top",
             "<top> @disambig(pred(a, 2), 5) ::= <n>");

    one_line("<top> @adt(\"enum\", $1, \"arms\", $0) ::= <n> <n>\n"
             "<n> ::= __LOWER\n",
             "top",
             "<top> @adt(\"enum\", $1, \"arms\", $0) ::= <n> <n>");

    one_line("<top> ::= <n>? <n>* <n>+\n<n> ::= __LOWER\n",
             "top",
             "<top> ::= (<n>)? (<n>)* (<n>)+");

    one_line("<top> ::= %\"kw\" | \"b\"\n", "top",
             "<top> ::= %\"kw\"\n    | \"b\"");

    check("an empty string literal is not a production",
          !load_ok("<top> ::= %\"kw\" | \"\"\n", "top"));
    check("and neither is one on its own",
          !load_ok("<top> ::= <e>\n<e> ::= \"\"\n", "top"));
    check("a one-character literal still loads",
          load_ok("<top> ::= \"b\"\n", "top"));

    round_trips("toy.bnf", "prog");
    round_trips("../../grammars/n00b.bnf", "module");
    round_trips("../../grammars/c_ncc.bnf", "translation_unit");

    return report("fe bnf-print");
}
