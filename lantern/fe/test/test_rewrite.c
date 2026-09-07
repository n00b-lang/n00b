#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/rewrite.h"
#include "parse/tree_dump.h"
#include "parse/tree_print.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "internal/parse/grammar_internal.h"
#include "ir_test.h"

static void
same(const char *what, char *got, const char *want)
{
    if (!got || strcmp(got, want) != 0) {
        fails++;
        printf("FAIL  %s\n  want: [%s]\n  got:  [%s]\n",
               what,
               want,
               got ? got : "(null)");
    }
}

static ncc_string_t
S(const char *s)
{
    return ncc_string_from_cstr(s);
}

static bool
lang_scan(ncc_scanner_t *s)
{
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        ncc_option_t(ncc_string_t) id = ncc_scan_identifier(s);

        if (ncc_option_is_set(id)) {
            int64_t kw = ncc_scan_terminal_id(s, ncc_option_get(id));

            ncc_scan_emit(s,
                          kw != NCC_TOK_OTHER ? (int32_t)kw
                                              : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);

    return true;
}

static const char *const LANG
    = "<prog> ::= <stmts>\n"
      "<stmts> ::= <stmt> <stmts> | <stmt>\n"
      "<stmt> ::= <with> | <call> | <plain>\n"
      "<with> ::= %\"with\" \"(\" %IDENTIFIER \")\" \"{\" <stmts> \"}\"\n"
      "<call> ::= %IDENTIFIER \"(\" %IDENTIFIER \")\" \";\"\n"
      "<plain> ::= %IDENTIFIER \";\"\n";

static ncc_grammar_t *g;

static ncc_parse_tree_t *
parse_with(const char *nt, const char *src, ncc_pwz_parser_t **pp,
           ncc_token_stream_t **tsp, ncc_scanner_t **scp,
           ncc_buffer_t **bufp)
{
    ncc_nonterm_t *start = ncc_find_nonterm(g, S(nt));

    ncc_grammar_set_start(g, start);

    *bufp = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
    *scp  = ncc_scanner_new(*bufp, lang_scan, g,
                            ncc_option_none(ncc_string_t), nullptr, nullptr);
    *tsp  = ncc_token_stream_new(*scp);
    *pp   = ncc_pwz_new(g);

    return ncc_pwz_parse(*pp, *tsp) ? ncc_pwz_get_tree(*pp) : nullptr;
}

static char *
render(ncc_parse_tree_t *t)
{
    ncc_print_opts_t o = {.width = 500, .indent = 2};

    return ncc_tree_print(g, t, &o);
}

static int rewrites;

static bool
desugar_with(ncc_visit_t *v)
{
    ncc_parse_tree_t *body = nullptr;
    ncc_parse_tree_t *name = nullptr;
    size_t            nc   = ncc_tree_num_children(v->node);

    for (size_t i = 0; i < nc; i++) {
        ncc_parse_tree_t *kid = ncc_tree_child(v->node, i);

        if (ncc_tree_is_leaf(kid)) {
            ncc_token_info_t *tok = ncc_tree_leaf_value(kid);

            if (tok && tok->tid == NCC_TOK_IDENTIFIER && !name) {
                name = kid;
            }
        }
        else {
            body = kid;
        }
    }

    if (!body || !name) {
        return true;
    }

    ncc_token_info_t *nt = ncc_tree_leaf_value(name);
    char              src[128];

    snprintf(src,
             sizeof(src),
             "context_init(%s); q; context_end(%s);",
             ncc_option_get(nt->value).data,
             ncc_option_get(nt->value).data);

    char           *err = nullptr;
    ncc_template_t *t   = ncc_template_parse(g, S("stmts"), S(src),
                                             lang_scan, &err);

    if (!t) {
        printf("FAIL  the template does not parse: %s\n", err ? err : "?");
        fails++;
        ncc_free(err);

        return true;
    }

    ncc_parse_tree_t *fresh = ncc_template_instance(t);

    ncc_template_free(t);

    for (ncc_parse_tree_t *at = fresh; at;) {
        size_t             n    = ncc_tree_num_children(at);
        ncc_parse_tree_t  *next = nullptr;

        for (size_t i = 0; i < n; i++) {
            ncc_parse_tree_t *kid = ncc_tree_child(at, i);
            char             *txt = kid && !ncc_tree_is_leaf(kid)
                                      ? render(kid)
                                      : nullptr;

            if (txt && !strcmp(txt, "q ;")) {
                ncc_tree_set_child(at, i, body);
                ncc_free(txt);
                next = nullptr;
                at   = nullptr;
                break;
            }

            ncc_free(txt);

            if (kid && !ncc_tree_is_leaf(kid)) {
                next = kid;
            }
        }

        if (!at) {
            break;
        }

        at = next;
    }

    v->replace = fresh;
    rewrites++;

    return true;
}

int
main(void)
{
    g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    if (!ncc_bnf_load(S(LANG), S("prog"), g)) {
        printf("FAIL  the grammar does not load\n");

        return 1;
    }

    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_scanner_t      *sc;
    ncc_buffer_t       *buf;
    ncc_parse_tree_t   *tree = parse_with("prog", "with (r) { a; b; }", &p,
                                          &ts, &sc, &buf);

    if (!tree) {
        printf("FAIL  the program does not parse\n");

        return 1;
    }

    same("the source round-trips before any rewrite",
         render(tree),
         "with ( r ) { a ; b ; }");

    ncc_parse_tree_t *lone = ncc_tree_clone(tree);

    same("a clone renders the same as its original",
         render(lone),
         "with ( r ) { a ; b ; }");

    check("but is a different node", lone != tree);

    ncc_parse_tree_t *made = ncc_tree_make_nt(g, S("plain"), 0);

    check("a node can be built from the grammar by name", made != nullptr);

    if (made) {
        ncc_tree_add_child(made,
                           ncc_tree_make_token(NCC_TOK_IDENTIFIER, S("z"),
                                               1, 1));
        ncc_tree_add_child(made, ncc_tree_make_token(';', S(";"), 1, 2));

        same("and renders as its parts", render(made), "z ;");
    }

    char           *err = nullptr;
    ncc_template_t *t   = ncc_template_parse(g, S("stmts"), S("x; y;"),
                                             lang_scan, &err);

    check("a template parses from source at a chosen start symbol",
          t != nullptr);
    ncc_free(err);

    if (t) {
        same("and instantiates as that source", render(ncc_template_instance(t)),
             "x ; y ;");

        ncc_parse_tree_t *i1 = ncc_template_instance(t);
        ncc_parse_tree_t *i2 = ncc_template_instance(t);

        check("each instance is independent", i1 != i2);
        ncc_template_free(t);
    }

    ncc_grammar_set_start(g, ncc_find_nonterm(g, S("prog")));

    ncc_tree_walk_t *w = ncc_tree_walk_new(g, nullptr);

    ncc_walk_on_nt(w, S("with"), nullptr, desugar_with, nullptr);

    int32_t passes = 0;

    tree = ncc_tree_rewrite(w, tree, 0, &passes);

    check("the rewrite ran the sugar exactly once", rewrites == 1);
    check("and stopped as soon as nothing changed", passes == 2);

    same("`with` desugared into the two calls around its body",
         render(tree),
         "context_init ( r ) ; a ; b ; context_end ( r ) ;");

    ncc_tree_walk_free(w);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_scanner_free(sc);
    ncc_buffer_free(buf);
    ncc_grammar_free(g);

    return report("fe rewrite");
}
