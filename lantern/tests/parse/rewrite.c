#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/rewrite.h"
#include "parse/tree_dump.h"
#include "parse/tree_print.h"
#include "parse/tree_util.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "harness.h"

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

static core_string_t
S(const char *s)
{
    return core_string_from_cstr(s);
}

static bool
lang_scan(parse_scanner_t *s)
{
    scan_skip_whitespace(s);

    if (scan_at_eof(s)) {
        return false;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        core_option_t(core_string_t) id = scan_identifier(s);

        if (core_option_is_set(id)) {
            int64_t kw = scan_terminal_id(s, core_option_get(id));

            scan_emit(s,
                          kw != PARSE_TOK_OTHER ? (int32_t)kw
                                              : PARSE_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);

    return true;
}

static const char *const LANG
    = "<prog> ::= <stmts>\n"
      "<stmts> ::= <stmt> <stmts> | <stmt>\n"
      "<stmt> ::= <with> | <call> | <plain> | <decl>\n"
      "<with> ::= %\"with\" \"(\" %IDENTIFIER \")\" \"{\" <stmts> \"}\"\n"
      "<call> ::= %IDENTIFIER \"(\" %IDENTIFIER \")\" \";\"\n"
      "<plain> ::= %IDENTIFIER \";\"\n"
      "<decl> ::= %\"let\" %IDENTIFIER \"=\" %IDENTIFIER \";\"\n"
      "         | %\"let\" %IDENTIFIER \":\" <type> \"=\" %IDENTIFIER \";\"\n"
      "<type> ::= %IDENTIFIER\n";

static parse_grammar_t *g;

static parse_tree_t *
parse_with(const char *nt, const char *src, parse_pwz_parser_t **pp,
           parse_token_stream_t **tsp, parse_scanner_t **scp,
           core_buffer_t **bufp)
{
    parse_nonterm_t *start = parse_find_nonterm(g, S(nt));

    *bufp = core_buffer_from_bytes(src, (int64_t)strlen(src));
    *scp  = scan_scanner_new(*bufp, lang_scan, g,
                            core_option_none(core_string_t), nullptr, nullptr);
    *tsp  = scan_token_stream_new(*scp);
    *pp   = parse_pwz_new_for_start(g, parse_nonterm_id(start));

    return parse_pwz_parse(*pp, *tsp) ? parse_pwz_get_tree(*pp) : nullptr;
}

static char *
render(parse_tree_t *t)
{
    parse_print_opts_t o = {.width = 500, .indent = 2};

    return parse_tree_print(g, t, &o);
}

static int rewrites;

static parse_tree_t *
find_nt(parse_tree_t *node, core_string_t name)
{
    if (!node || core_tree_is_leaf(node)) {
        return nullptr;
    }

    if (core_string_eq(core_tree_node_value(node).name, name)) {
        return node;
    }

    for (size_t i = 0; i < core_tree_num_children(node); i++) {
        parse_tree_t *found = find_nt(core_tree_child(node, i), name);

        if (found) {
            return found;
        }
    }

    return nullptr;
}

static parse_tree_array_t
build_type_syntax(parse_grammar_t                         *grammar,
                  const parse_type_materialization_rule_t *rule,
                  const parse_tree_t                      *node,
                  th_type                                 *type,
                  void                                    *state,
                  char                                   **err)
{
    (void)rule;
    (void)node;
    (void)err;

    th_ctx             *types  = state;
    const char         *text   = th_print(types, type);
    parse_tree_array_t  syntax = core_array_new(parse_tree_ptr_t, 2);
    parse_tree_t       *type_node = parse_tree_make_nt(grammar, S("type"), 0);

    core_tree_add_child(
        type_node,
        parse_tree_make_synthetic_token(PARSE_TOK_IDENTIFIER,
                                        core_string_borrow(text,
                                                           strlen(text))));
    core_array_set(syntax, 0, parse_tree_make_synthetic_token(':', S(":")));
    core_array_set(syntax, 1, type_node);

    return syntax;
}

static bool
desugar_with(parse_visit_t *v)
{
    parse_tree_t *body = nullptr;
    parse_tree_t *name = nullptr;
    size_t            nc   = core_tree_num_children(v->node);

    for (size_t i = 0; i < nc; i++) {
        parse_tree_t *kid = core_tree_child(v->node, i);

        if (core_tree_is_leaf(kid)) {
            parse_token_info_t *tok = core_tree_leaf_value(kid);

            if (tok && tok->tid == PARSE_TOK_IDENTIFIER && !name) {
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

    parse_token_info_t *nt = core_tree_leaf_value(name);
    char              src[128];

    snprintf(src,
             sizeof(src),
             "context_init(%s); q; context_end(%s);",
             core_option_get(nt->value).data,
             core_option_get(nt->value).data);

    char           *err = nullptr;
    parse_template_t *t   = parse_template_parse(g, S("stmts"), S(src),
                                             lang_scan, &err);

    if (!t) {
        printf("FAIL  the template does not parse: %s\n", err ? err : "?");
        fails++;
        core_free(err);

        return true;
    }

    parse_tree_t *fresh = parse_template_instance(t);

    parse_template_free(t);

    for (parse_tree_t *at = fresh; at;) {
        size_t             n    = core_tree_num_children(at);
        parse_tree_t  *next = nullptr;

        for (size_t i = 0; i < n; i++) {
            parse_tree_t *kid = core_tree_child(at, i);
            char             *txt = kid && !core_tree_is_leaf(kid)
                                      ? render(kid)
                                      : nullptr;

            if (txt && !strcmp(txt, "q ;")) {
                core_tree_set_child(at, i, body);
                core_free(txt);
                next = nullptr;
                at   = nullptr;
                break;
            }

            core_free(txt);

            if (kid && !core_tree_is_leaf(kid)) {
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
    g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(S(LANG), S("prog"), g)) {
        printf("FAIL  the grammar does not load\n");

        return 1;
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_scanner_t      *sc;
    core_buffer_t       *buf;
    parse_tree_t   *tree = parse_with("prog", "with (r) { a; b; }", &p,
                                          &ts, &sc, &buf);

    if (!tree) {
        printf("FAIL  the program does not parse\n");

        return 1;
    }

    same("the source round-trips before any rewrite",
         render(tree),
         "with ( r ) { a ; b ; }");

    parse_tree_t *lone = parse_tree_clone(tree);

    same("a clone renders the same as its original",
         render(lone),
         "with ( r ) { a ; b ; }");

    check("but is a different node", lone != tree);

    parse_token_info_t *source_token = parse_tree_find_first_terminal(tree);
    parse_trivia_t *source_comment = core_alloc(parse_trivia_t);

    source_comment->text = CORE_STRING_STATIC("# retained");
    source_comment->kind = PARSE_TRIVIA_COMMENT;
    source_token->leading_trivia = source_comment;

    parse_tree_t *commented = parse_tree_clone(tree);
    parse_token_info_t *cloned_token
        = parse_tree_find_first_terminal(commented);

    check("a clone owns an independent copy of comment trivia",
          cloned_token && cloned_token->leading_trivia
              && cloned_token->leading_trivia != source_comment
              && core_string_eq(cloned_token->leading_trivia->text,
                                source_comment->text));
    same("a cloned tree retains its comments",
         render(commented),
         "# retained\nwith ( r ) { a ; b ; }");

    source_token->leading_trivia = nullptr;
    parse_trivia_free(source_comment);
    parse_tree_free(commented);

    parse_tree_t *made = parse_tree_make_nt(g, S("plain"), 0);

    check("a node can be built from the grammar by name", made != nullptr);

    if (made) {
        core_tree_add_child(
            made,
            parse_tree_make_synthetic_token(PARSE_TOK_IDENTIFIER, S("z")));
        core_tree_add_child(made,
                            parse_tree_make_synthetic_token(';', S(";")));

        same("and renders as its parts", render(made), "z ;");
    }

    char           *err = nullptr;
    parse_template_t *t   = parse_template_parse(g, S("stmts"), S("x; y;"),
                                             lang_scan, &err);

    check("a template parses from source at a chosen start symbol",
          t != nullptr);
    core_free(err);

    if (t) {
        same("and instantiates as that source", render(parse_template_instance(t)),
             "x ; y ;");

        parse_tree_t *i1 = parse_template_instance(t);
        parse_tree_t *i2 = parse_template_instance(t);

        check("each instance is independent", i1 != i2);
        parse_template_free(t);
        same("instances retain token text after their template is freed",
             render(i1), "x ; y ;");
        parse_tree_free(i1);
        parse_tree_free(i2);
    }

    parse_pwz_parser_t   *typed_p;
    parse_token_stream_t *typed_ts;
    parse_scanner_t      *typed_sc;
    core_buffer_t        *typed_buf;
    parse_tree_t         *implicit = parse_with("prog", "let x = y;", &typed_p,
                                                &typed_ts, &typed_sc,
                                                &typed_buf);
    parse_tree_t         *decl = find_nt(implicit, S("decl"));
    th_ctx               *types = th_ctx_new();

    check("the implicit declaration parsed", decl != nullptr);

    if (decl) {
        parse_token_info_t *source_token = parse_tree_find_first_terminal(decl);

        check("parsed tokens retain source provenance",
              source_token && source_token->origin == PARSE_TOKEN_SOURCE);

        core_tree_node_value(decl).type = th_name(types, "int");

        const parse_type_materialization_rule_t type_rules[] = {
            {
                .node_name           = S("decl"),
                .implicit_rule_index = 0,
                .explicit_rule_index = 1,
                .insert_before_child = 2,
            },
        };
        char         *type_err = nullptr;
        parse_tree_t *explicit = parse_tree_materialize_types(
            g, types, implicit, type_rules, 1, build_type_syntax, types,
            &type_err);

        check("a resolved type materializes", explicit && !type_err);
        same("materialization leaves the source tree unchanged",
             render(implicit), "let x = y ;");
        same("materialization selects the explicit grammar production",
             render(explicit), "let x : int = y ;");

        parse_tree_t *explicit_decl = find_nt(explicit, S("decl"));

        check("the inserted separator is synthetic",
              explicit_decl
                  && parse_node_token(core_tree_child(explicit_decl, 2))->origin
                         == PARSE_TOKEN_SYNTHETIC);

        parse_tree_t *explicit_type = explicit_decl
                                      ? core_tree_child(explicit_decl, 3)
                                      : nullptr;
        parse_token_info_t *type_token = explicit_type
                                         ? parse_tree_find_first_terminal(
                                               explicit_type)
                                         : nullptr;

        check("the inserted type syntax is synthetic",
              type_token && type_token->origin == PARSE_TOKEN_SYNTHETIC);

        parse_tree_free(explicit);
        core_free(type_err);

        core_tree_node_value(decl).type = th_var(types);
        explicit = parse_tree_materialize_types(
            g, types, implicit, type_rules, 1, build_type_syntax, types,
            &type_err);

        check("an unresolved type is rejected",
              !explicit && type_err
                  && strstr(type_err, "requires a resolved type"));
        core_free(type_err);
    }

    th_ctx_free(types);
    parse_pwz_free(typed_p);
    scan_token_stream_free(typed_ts);
    scan_scanner_free(typed_sc);
    core_buffer_free(typed_buf);

    parse_tree_walk_t *w = parse_tree_walk_new(g, nullptr);

    parse_walk_on_nt(w, S("with"), nullptr, desugar_with, nullptr);

    int32_t passes = 0;

    tree = parse_tree_rewrite(w, tree, 0, &passes);

    check("the rewrite ran the sugar exactly once", rewrites == 1);
    check("and stopped as soon as nothing changed", passes == 2);

    same("`with` desugared into the two calls around its body",
         render(tree),
         "context_init ( r ) ; a ; b ; context_end ( r ) ;");

    parse_tree_walk_free(w);
    parse_tree_free(lone);
    parse_tree_free(made);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
    parse_grammar_free(g);

    return report("fe rewrite");
}
