#include <stdio.h>
#include <stdlib.h>

#include "lib/alloc.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "private/parse/grammar.h"
#include "harness.h"

#define S(x) CORE_STRING_STATIC(x)

static const core_string_t EXTRA_TOKENS[] = {
    S("CHAR_LIT"),  S("EMBED"),      S("FLOAT_LIT"), S("HEX_LIT"), S("i"),
    S("u"),         S("bits"),       S("INT_LIT"),   S("LITERAL_PREFIX"),
    S("NEWLINE"),   S("STRING_LIT"), S("INDENT"),    S("DEDENT"),
};

static bool
load_ok(core_string_t text, core_string_t start)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    bool ok = parse_bnf_load(text, start, g);

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

static core_string_t
reprint_counting(core_string_t text, core_string_t start, core_string_t what,
                 size_t *annots, size_t *rules)
{
    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    for (size_t i = 0; i < sizeof(EXTRA_TOKENS) / sizeof(EXTRA_TOKENS[0]);
         i++) {
        parse_register_terminal(g, EXTRA_TOKENS[i]);
    }

    if (!parse_bnf_load(text, start, g)) {
        core_string_t err = parse_bnf_error();

        printf("FAIL  %.*s does not load: %.*s\n", (int)what.u8_bytes,
               what.data, (int)err.u8_bytes, err.data);
        fails++;
        parse_grammar_free(g);

        return (core_string_t){};
    }

    core_string_t out = parse_bnf_print(g, parse_bnf_last_unit());

    if (annots) {
        *annots = annot_total(g);
    }

    if (rules) {
        *rules = core_list_len(g->rules);
    }

    parse_grammar_free(g);

    return out;
}

static core_string_t
reprint(core_string_t text, core_string_t start, core_string_t what)
{
    return reprint_counting(text, start, what, nullptr, nullptr);
}

static void
round_trips_s(const char *file, core_string_t path, core_string_t start)
{
    core_string_t text = slurp(file);

    if (!text.data) {
        return;
    }

    size_t a1 = 0, a2 = 0, r1 = 0, r2 = 0;

    core_string_t once = reprint_counting(text, start, path, &a1, &r1);

    if (!once.data) {
        return;
    }

    core_string_t twice = reprint_counting(once, start, S("the reprint"),
                                           &a2, &r2);

    if (!twice.data) {
        printf("---- %.*s reprinted ----\n%.*s\n", (int)path.u8_bytes,
               path.data, (int)once.u8_bytes, once.data);
        core_free(once.data);

        return;
    }

    if (!a1) {
        printf("FAIL  %.*s has no annotations, so it proves nothing\n",
               (int)path.u8_bytes, path.data);
        fails++;
    }

    if (a1 != a2) {
        printf("FAIL  %.*s: %zu annotations became %zu across the reprint\n",
               (int)path.u8_bytes,
               path.data,
               a1,
               a2);
        fails++;
    }

    if (r1 != r2) {
        printf("FAIL  %.*s: %zu rules became %zu across the reprint\n",
               (int)path.u8_bytes,
               path.data,
               r1,
               r2);
        fails++;
    }

    if (!core_string_eq(once, twice)) {
        printf("FAIL  %.*s does not reach a fixpoint\n", (int)path.u8_bytes,
               path.data);
        fails++;

        size_t at = 0;

        while (at < once.u8_bytes && at < twice.u8_bytes
               && once.data[at] == twice.data[at]) {
            at++;
        }

        size_t a_rest = once.u8_bytes - at;
        size_t b_rest = twice.u8_bytes - at;

        printf("  first difference at byte %zu\n", at);
        printf("  once:  %.*s\n", (int)(a_rest < 90 ? a_rest : 90),
               once.data + at);
        printf("  twice: %.*s\n", (int)(b_rest < 90 ? b_rest : 90),
               twice.data + at);
    }

    core_free(once.data);
    core_free(twice.data);
}

#define round_trips(path, start) round_trips_s((path), S(path), S(start))

static void
one_line(core_string_t src, core_string_t start, core_string_t want)
{
    core_string_t out = reprint(src, start, src);

    if (!out.data) {
        return;
    }

    if (core_string_find(out, want) < 0) {
        printf("FAIL  reprinting %.*s", (int)src.u8_bytes, src.data);
        printf("  wanted to see: %.*s\n", (int)want.u8_bytes, want.data);
        printf("  got:\n%.*s\n", (int)out.u8_bytes, out.data);
        fails++;
    }

    core_free(out.data);
}

int
main(void)
{
    one_line(S("<top> @nospace($1) ::= <n> <n>\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @nospace($1) ::= <n> <n>"));

    one_line(S("<top> @declares($1, $2, \"var\") ::= <n> <n>\n"
               "<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @declares($1, $2, \"var\") ::= <n> <n>"));

    one_line(S("<top> @scope(\"module\", $1) ::= <n>\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @scope(\"module\", $1) ::= <n>"));

    one_line(S("<top> @literal(\"int\", $n) ::= <n>\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @literal(\"int\", $n) ::= <n>"));

    one_line(S("<top> @infer($0 unify $1) ::= <n> <n>\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @infer($0 unify $1) ::= <n> <n>"));

    one_line(S("<top> @infer(\"$0 unify $1\") ::= <n> <n>\n"
               "<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @infer(\"$0 unify $1\") ::= <n> <n>"));

    one_line(S("<top> @disambig(pred(a, 2), 5) ::= <n>\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @disambig(pred(a, 2), 5) ::= <n>"));

    one_line(S("<top> @adt(\"enum\", $1, \"arms\", $0) ::= <n> <n>\n"
               "<n> ::= __LOWER\n"),
             S("top"),
             S("<top> @adt(\"enum\", $1, \"arms\", $0) ::= <n> <n>"));

    one_line(S("<top> ::= <n>? <n>* <n>+\n<n> ::= __LOWER\n"),
             S("top"),
             S("<top> ::= (<n>)? (<n>)* (<n>)+"));

    one_line(S("<top> ::= %\"kw\" | \"b\"\n"), S("top"),
             S("<top> ::= %\"kw\"\n    | \"b\""));

    check("an empty string literal is not a production",
          !load_ok(S("<top> ::= %\"kw\" | \"\"\n"), S("top")));
    check("and neither is one on its own",
          !load_ok(S("<top> ::= <e>\n<e> ::= \"\"\n"), S("top")));
    check("a one-character literal still loads",
          load_ok(S("<top> ::= \"b\"\n"), S("top")));

    round_trips("toy.bnf", "prog");
    round_trips("../../grammars/c_ncc.bnf", "translation_unit");

    return report("fe bnf-print");
}
