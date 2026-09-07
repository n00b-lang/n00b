#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/annotation.h"
#include "private/parse/grammar.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: bnfcheck <file.bnf> <start>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *p = core_alloc_size(1, (size_t)n + 1);
    size_t got = fread(p, 1, (size_t)n, f); p[got] = 0; fclose(f);

    parse_grammar_t *g = parse_grammar_new();
    parse_grammar_set_error_recovery(g, false);

    for (int i = 3; i < argc; i++) {
        parse_register_terminal(g, core_string_from_cstr(argv[i]));
    }

    bool ok = parse_bnf_load(core_string_from_cstr(p), core_string_from_cstr(argv[2]), g);
    printf("load %s: %s\n", argv[1], ok ? "OK" : "FAILED");
    if (!ok) return 1;

    int empty = 0, total = 0;
    for (size_t i = 0; i < core_list_len(g->nt_list); i++) {
        parse_nonterm_t *nt = g->nt_list.data[i];
        if (nt->group_nt) continue;
        if (!nt->name.data || nt->name.data[0] == '$') continue;
        total++;
        if (core_list_len(nt->rule_ids) == 0) {
            printf("  undefined nonterminal: <%s>\n", nt->name.data);
            empty++;
        }
    }
    printf("  %d nonterminals, %d undefined, %zu rules, %zu annotations\n",
           total, empty, core_list_len(g->rules), parse_grammar_annot_count(g));
    printf("  named terminals:");

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
        printf(" %s", g->named_terms.data[i].value.data);
    }

    printf("\n");
    parse_grammar_free(g);
    return 0;
}
