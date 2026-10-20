#include <stdio.h>

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

#define S(x) CORE_STRING_STATIC(x)

static void
same(core_string_t what, core_string_t got, core_string_t want)
{
    if (!got.data || !core_string_eq(got, want)) {
        core_string_t shown = got.data ? got : S("(null)");

        fails++;
        printf("FAIL  %.*s\n  want: [%.*s]\n  got:  [%.*s]\n",
               (int)what.u8_bytes,
               what.data,
               (int)want.u8_bytes,
               want.data,
               (int)shown.u8_bytes,
               shown.data);
    }
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

#define LANG                                                                  \
    S("<prog> ::= <stmts>\n"                                                  \
      "<stmts> ::= <stmt> <stmts> | <stmt>\n"                                 \
      "<stmt> ::= <with> | <call> | <plain> | <decl>\n"                       \
      "<with> ::= %\"with\" \"(\" %IDENTIFIER \")\" \"{\" <stmts> \"}\"\n"    \
      "<call> ::= %IDENTIFIER \"(\" %IDENTIFIER \")\" \";\"\n"                \
      "<plain> ::= %IDENTIFIER \";\"\n"                                       \
      "<decl> ::= %\"let\" %IDENTIFIER \"=\" %IDENTIFIER \";\"\n"             \
      "         | %\"let\" %IDENTIFIER \":\" <type> \"=\" %IDENTIFIER \";\"\n" \
      "<type> ::= %IDENTIFIER\n")

static parse_grammar_t *g;

static parse_tree_t *
parse_with(core_string_t nt, core_string_t src, parse_pwz_parser_t **pp,
           parse_token_stream_t **tsp, parse_scanner_t **scp,
           core_buffer_t **bufp)
{
    parse_nonterm_t *start = parse_find_nonterm(g, nt);

    *bufp = core_buffer_from_bytes(src.data, (int64_t)src.u8_bytes);
    *scp  = scan_scanner_new(*bufp, lang_scan, g,
                            core_option_none(core_string_t), nullptr, nullptr);
    *tsp  = scan_token_stream_new(*scp);
    *pp   = parse_pwz_new_for_start(g, parse_nonterm_id(start));

    return parse_pwz_parse(*pp, *tsp) ? parse_pwz_get_tree(*pp) : nullptr;
}

static core_string_t
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
                  core_string_t                           *err)
{
    (void)rule;
    (void)node;
    (void)err;

    th_ctx             *types  = state;
    core_string_t       text   = th_print(types, type);
    parse_tree_array_t  syntax = core_array_new(parse_tree_ptr_t, 2);
    parse_tree_t       *type_node = parse_tree_make_nt(grammar, S("type"), 0);

    core_tree_add_child(
        type_node,
        parse_tree_make_synthetic_token(PARSE_TOK_IDENTIFIER, text));
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

            if (tok && parse_token_tid(tok) == PARSE_TOK_IDENTIFIER
                && !name) {
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
    core_string_t     word = core_option_get(parse_token_value(nt));
    char              src[128];

    int               n   = snprintf(src,
                                     sizeof(src),
                                     "context_init(%.*s); q; context_end(%.*s);",
                                     (int)word.u8_bytes, word.data,
                                     (int)word.u8_bytes, word.data);
    size_t            len = n < 0 ? 0
                          : (size_t)n < sizeof(src) ? (size_t)n
                                                    : sizeof(src) - 1;
    core_string_t     err = {};
    parse_template_t *t   = parse_template_parse(g, S("stmts"),
                                                 core_string_borrow(src, len),
                                                 lang_scan, &err);

    if (!t) {
        core_string_t shown = err.data ? err : S("?");

        printf("FAIL  the template does not parse: %.*s\n",
               (int)shown.u8_bytes, shown.data);
        fails++;
        core_free(err.data);

        return true;
    }

    parse_tree_t *fresh = parse_template_instance(t);

    parse_template_free(t);

    for (parse_tree_t *at = fresh; at;) {
        size_t             n    = core_tree_num_children(at);
        parse_tree_t  *next = nullptr;

        for (size_t i = 0; i < n; i++) {
            parse_tree_t *kid = core_tree_child(at, i);
            core_string_t     txt = kid && !core_tree_is_leaf(kid)
                                      ? render(kid)
                                      : (core_string_t){};

            if (txt.data && core_string_eq(txt, S("q ;"))) {
                core_tree_set_child(at, i, body);
                core_free(txt.data);
                next = nullptr;
                at   = nullptr;
                break;
            }

            core_free(txt.data);

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

    if (!parse_bnf_load(LANG, S("prog"), g)) {
        printf("FAIL  the grammar does not load\n");

        return 1;
    }

    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_scanner_t      *sc;
    core_buffer_t       *buf;
    parse_tree_t   *tree = parse_with(S("prog"), S("with (r) { a; b; }"), &p,
                                          &ts, &sc, &buf);

    if (!tree) {
        printf("FAIL  the program does not parse\n");

        return 1;
    }

    same(S("the source round-trips before any rewrite"),
         render(tree),
         S("with ( r ) { a ; b ; }"));

    parse_tree_t *lone = parse_tree_clone(tree);

    same(S("a clone renders the same as its original"),
         render(lone),
         S("with ( r ) { a ; b ; }"));

    check("but is a different node", lone != tree);

    parse_token_info_t *source_token = parse_tree_find_first_terminal(tree);
    parse_trivia_t *source_comment = core_alloc(parse_trivia_t);

    source_comment->text = CORE_STRING_STATIC("# retained");
    source_comment->kind = PARSE_TRIVIA_COMMENT;
    parse_token_set_leading_trivia(source_token, source_comment);

    parse_tree_t *commented = parse_tree_clone(tree);
    parse_token_info_t *cloned_token
        = parse_tree_find_first_terminal(commented);

    check("a clone owns an independent copy of comment trivia",
          cloned_token && parse_token_leading_trivia(cloned_token)
              && parse_token_leading_trivia(cloned_token) != source_comment
              && core_string_eq(parse_token_leading_trivia(cloned_token)->text,
                                source_comment->text));
    same(S("a cloned tree retains its comments"),
         render(commented),
         S("# retained\nwith ( r ) { a ; b ; }"));

    parse_token_set_leading_trivia(source_token, nullptr);
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

        same(S("and renders as its parts"), render(made), S("z ;"));
    }

    core_string_t     err = {};
    parse_template_t *t   = parse_template_parse(g, S("stmts"), S("x; y;"),
                                             lang_scan, &err);

    check("a template parses from source at a chosen start symbol",
          t != nullptr);
    core_free(err.data);

    if (t) {
        same(S("and instantiates as that source"),
             render(parse_template_instance(t)), S("x ; y ;"));

        parse_tree_t *i1 = parse_template_instance(t);
        parse_tree_t *i2 = parse_template_instance(t);

        check("each instance is independent", i1 != i2);
        parse_template_free(t);
        same(S("instances retain token text after their template is freed"),
             render(i1), S("x ; y ;"));
        parse_tree_free(i1);
        parse_tree_free(i2);
    }

    parse_pwz_parser_t   *typed_p;
    parse_token_stream_t *typed_ts;
    parse_scanner_t      *typed_sc;
    core_buffer_t        *typed_buf;
    parse_tree_t         *implicit = parse_with(S("prog"), S("let x = y;"),
                                                &typed_p,
                                                &typed_ts, &typed_sc,
                                                &typed_buf);
    parse_tree_t         *decl = find_nt(implicit, S("decl"));
    th_ctx               *types = th_ctx_new();

    check("the implicit declaration parsed", decl != nullptr);

    if (decl) {
        parse_token_info_t *source_token = parse_tree_find_first_terminal(decl);

        check("parsed tokens retain source provenance",
              source_token
                  && parse_token_origin(source_token) == PARSE_TOKEN_SOURCE);

        parse_tree_set_type(decl, th_name(types, S("int")));

        const parse_type_materialization_rule_t type_rules[] = {
            {
                .node_name           = S("decl"),
                .implicit_rule_index = 0,
                .explicit_rule_index = 1,
                .insert_before_child = 2,
            },
        };
        core_string_t type_err = {};
        parse_tree_t *explicit = parse_tree_materialize_types(
            g, types, implicit, type_rules, 1, build_type_syntax, types,
            &type_err);

        check("a resolved type materializes", explicit && !type_err.data);
        same(S("materialization leaves the source tree unchanged"),
             render(implicit), S("let x = y ;"));
        same(S("materialization selects the explicit grammar production"),
             render(explicit), S("let x : int = y ;"));

        parse_tree_t *explicit_decl = find_nt(explicit, S("decl"));

        check("the inserted separator is synthetic",
              explicit_decl
                  && parse_token_origin(parse_node_token(
                         core_tree_child(explicit_decl, 2)))
                         == PARSE_TOKEN_SYNTHETIC);

        parse_tree_t *explicit_type = explicit_decl
                                      ? core_tree_child(explicit_decl, 3)
                                      : nullptr;
        parse_token_info_t *type_token = explicit_type
                                         ? parse_tree_find_first_terminal(
                                               explicit_type)
                                         : nullptr;

        check("the inserted type syntax is synthetic",
              type_token
                  && parse_token_origin(type_token) == PARSE_TOKEN_SYNTHETIC);

        parse_tree_free(explicit);
        core_free(type_err.data);
        type_err = (core_string_t){};

        parse_tree_set_type(decl, th_var(types));
        explicit = parse_tree_materialize_types(
            g, types, implicit, type_rules, 1, build_type_syntax, types,
            &type_err);

        check("an unresolved type is rejected",
              !explicit && type_err.data
                  && core_string_find(type_err,
                                      S("requires a resolved type")) >= 0);
        core_free(type_err.data);
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

    same(S("`with` desugared into the two calls around its body"),
         render(tree),
         S("context_init ( r ) ; a ; b ; context_end ( r ) ;"));

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
