#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/alloc.h"
#include "parse/bnf.h"
#include "parse/grammar.h"
#include "parse/annotation.h"
#include "internal/parse/grammar_internal.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: bnfcheck <file.bnf> <start>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *p = malloc((size_t)n + 1);
    size_t got = fread(p, 1, (size_t)n, f); p[got] = 0; fclose(f);

    ncc_grammar_t *g = ncc_grammar_new();
    ncc_grammar_set_error_recovery(g, false);

    for (int i = 3; i < argc; i++) {
        ncc_register_terminal(g, ncc_string_from_cstr(argv[i]));
    }

    bool ok = ncc_bnf_load(ncc_string_from_cstr(p), ncc_string_from_cstr(argv[2]), g);
    printf("load %s: %s\n", argv[1], ok ? "OK" : "FAILED");
    if (!ok) return 1;

    int empty = 0, total = 0;
    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        ncc_nonterm_t *nt = g->nt_list.data[i];
        if (nt->group_nt) continue;
        if (!nt->name.data || nt->name.data[0] == '$') continue;
        total++;
        if (ncc_list_len(nt->rule_ids) == 0) {
            printf("  undefined nonterminal: <%s>\n", nt->name.data);
            empty++;
        }
    }
    printf("  %d nonterminals, %d undefined, %zu rules, %zu annotations\n",
           total, empty, ncc_list_len(g->rules), ncc_grammar_annot_count(g));
    printf("  named terminals:");

    for (size_t i = 0; i < ncc_list_len(g->named_terms); i++) {
        printf(" %s", g->named_terms.data[i].value.data);
    }

    printf("\n");
    ncc_grammar_free(g);
    return 0;
}
