#include <stdio.h>

#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/annotation.h"
#include "private/parse/grammar.h"
#include "scanner/lex.h"

static bool
imports_lexer(parse_grammar_t *g)
{
    for (size_t i = 0; i < parse_grammar_annot_count(g); i++) {
        parse_annotation_t *a = parse_grammar_annot_at(g, i);

        if (a && a->kind == PARSE_ANNOT_LEXER) {
            return true;
        }
    }

    return false;
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: bnfcheck <file.bnf> <start> [terminal ...]\n");
        return 2;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    for (int i = 3; i < argc; i++) {
        parse_register_terminal(g, core_string_from_cstr(argv[i]));
    }

    core_string_t path  = core_string_from_cstr(argv[1]);
    core_string_t start = core_string_from_cstr(argv[2]);
    bool          ok    = parse_bnf_load_file(path, start, g);

    core_string_t status = ok ? CORE_STRING_STATIC("OK")
                              : CORE_STRING_STATIC("FAILED");

    printf("load %.*s: %.*s\n", (int)path.u8_bytes, path.data,
           (int)status.u8_bytes, status.data);

    if (!ok) {
        core_string_t err = parse_bnf_error();

        fprintf(stderr, "%.*s\n", (int)err.u8_bytes, err.data);
        parse_grammar_free(g);
        return 1;
    }

    if (imports_lexer(g) && !scan_grammar_lex_compiled(g)) {
        core_string_t err = scan_lex_build_error();

        fprintf(stderr, "%.*s\n", (int)err.u8_bytes, err.data);
        parse_grammar_free(g);
        return 1;
    }

    int empty = 0, total = 0;

    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        parse_nonterm_t *nt = g->nt_list.data[i];

        if (nt->group_nt) {
            continue;
        }
        if (!nt->name.data
            || (nt->name.u8_bytes && nt->name.data[0] == '$')) {
            continue;
        }

        total++;

        if (core_list_len(nt->rule_ids) == 0) {
            printf("  undefined nonterminal: <%.*s>\n", (int)nt->name.u8_bytes,
                   nt->name.data);
            empty++;
        }
    }
    printf("  %d nonterminals, %d undefined, %zu rules, %zu annotations\n",
           total, empty, core_list_len(g->rules), parse_grammar_annot_count(g));
    printf("  named terminals:");

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
        core_string_t term = g->named_terms.data[i].value;

        printf(" %.*s", (int)term.u8_bytes, term.data);
    }

    printf("\n");
    parse_grammar_free(g);
    return empty ? 1 : 0;
}
