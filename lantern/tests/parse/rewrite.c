#include <stdio.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/rewrite.h"
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

    parse_tree_free(lone);
    parse_tree_free(made);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
    parse_grammar_free(g);

    return report("fe rewrite");
}
