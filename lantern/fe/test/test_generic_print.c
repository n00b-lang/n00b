#include <stdio.h>
#include <string.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "parse/annot_walk.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "parse/tree_util.h"
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

static bool
toy_scan(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek_byte(s, 0) == '#') {
        ncc_scan_skip_line_comment(s);
        goto restart;
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

typedef struct {
    ncc_grammar_t      *g;
    ncc_pwz_parser_t   *p;
    ncc_token_stream_t *ts;
    ncc_scanner_t      *sc;
    ncc_buffer_t       *buf;
    ncc_parse_tree_t   *tree;
} run_t;

static bool
run(run_t *r, const char *bnf, const char *start, const char *src)
{
    *r      = (run_t){0};
    r->g    = ncc_grammar_new();

    ncc_grammar_set_error_recovery(r->g, false);

    if (!ncc_bnf_load(ncc_string_from_cstr(bnf),
                      ncc_string_from_cstr(start),
                      r->g)) {
        printf("FAIL  the grammar does not load\n");
        fails++;

        return false;
    }

    r->buf = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
    r->sc  = ncc_scanner_new(r->buf, toy_scan, r->g,
                             ncc_option_none(ncc_string_t), nullptr, nullptr);
    r->ts  = ncc_token_stream_new(r->sc);
    r->p   = ncc_pwz_new(r->g);

    if (!ncc_pwz_parse(r->p, r->ts)) {
        printf("FAIL  %s does not parse\n", src);
        fails++;

        return false;
    }

    r->tree = ncc_pwz_get_tree(r->p);

    return true;
}

static void
run_free(run_t *r)
{
    ncc_pwz_free(r->p);
    ncc_token_stream_free(r->ts);
    ncc_scanner_free(r->sc);
    ncc_buffer_free(r->buf);
    ncc_grammar_free(r->g);
}

static const char *const PLAIN
    = "<prog> ::= <stmt> | <stmt> <prog>\n"
      "<stmt> ::= %\"let\" %IDENTIFIER \"=\" %IDENTIFIER \";\"\n";

static const char *const SHAPED
    = "<prog> ::= <stmt> | <stmt> <prog>\n"
      "<stmt> @nospace($4) @hardline ::= %\"let\" %IDENTIFIER \"=\" "
      "%IDENTIFIER \";\"\n";

static void
test_a_tree_dumps_without_any_annotations(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a = b;")) {
        return;
    }

    char *d = ncc_tree_dump_str(r.g, r.tree, false);

    same("a parse tree dumps structurally with no annotations at all",
         d,
         "<prog> rule=0\n"
         "  <stmt> rule=0\n"
         "    TOKEN[1073741831] \"let\"\n"
         "    TOKEN[1073741825] \"a\"\n"
         "    TOKEN[61] \"=\"\n"
         "    TOKEN[1073741825] \"b\"\n"
         "    TOKEN[59] \";\"\n");

    ncc_free(d);
    run_free(&r);
}

static void
test_formatting_falls_back_to_one_space(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a   =\n\tb;    let c = d;")) {
        return;
    }

    ncc_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = ncc_tree_print(r.g, r.tree, &o);

    same("with no printing annotations every token is one space apart, "
         "whatever the input's whitespace was",
         s,
         "let a = b ; let c = d ;");

    ncc_free(s);
    run_free(&r);
}

static void
test_annotations_win_where_they_exist(void)
{
    run_t r;

    if (!run(&r, SHAPED, "prog", "let a = b; let c = d;")) {
        return;
    }

    ncc_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = ncc_tree_print(r.g, r.tree, &o);

    same("a production's own annotations override the fallback, and the "
         "gap between two of them belongs to the production above",
         s,
         "let\na\n=\nb; let\nc\n=\nd;");

    ncc_free(s);
    run_free(&r);
}

static void
test_a_comment_the_caller_marks_as_leading_is_kept(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a = b;")) {
        return;
    }

    ncc_parse_tree_t *first = r.tree;

    while (first && !ncc_tree_is_leaf(first)) {
        first = ncc_tree_child(first, 0);
    }

    ncc_token_info_t *tok = first ? ncc_tree_leaf_value(first) : nullptr;

    check("the first token was found", tok != nullptr);

    if (!tok) {
        run_free(&r);

        return;
    }

    ncc_trivia_t *tv = ncc_alloc(ncc_trivia_t);

    tv->text = NCC_STRING_STATIC("# why");
    tv->kind = NCC_TRIVIA_COMMENT;

    tok->leading_trivia = tv;

    ncc_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = ncc_tree_print(r.g, r.tree, &o);

    same("comment trivia is content and keeps its own line, while the "
         "input's whitespace does not survive at all",
         s,
         "# why\nlet a = b ;");

    ncc_free(s);
    run_free(&r);
}

static bool
comment_scan(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek_byte(s, 0) == '#') {
        ncc_scan_skip_line_comment(s);
        goto restart;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '_' || (c >= 'a' && c <= 'z')) {
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

static char *
comments_of(const char *src, ncc_trivia_attach_t how, size_t *n_lead,
            size_t *n_trail)
{
    ncc_buffer_t  *buf = ncc_buffer_from_bytes(src, (int64_t)strlen(src));
    ncc_scanner_t *sc  = ncc_scanner_new(buf, comment_scan, nullptr,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);

    ncc_scan_set_trivia_attach(sc, how);

    ncc_token_stream_t *ts = ncc_token_stream_new(sc);
    ncc_token_info_t   *t;

    *n_lead  = 0;
    *n_trail = 0;

    while (ncc_stream_next(ts)) {
        ;
    }

    for (int32_t i = 0; i < ncc_stream_token_count(ts); i++) {
        t = ncc_stream_get(ts, i);

        for (ncc_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
            if (tv->kind == NCC_TRIVIA_COMMENT) {
                (*n_lead)++;
            }
        }

        for (ncc_trivia_t *tv = t->trailing_trivia; tv; tv = tv->next) {
            if (tv->kind == NCC_TRIVIA_COMMENT) {
                (*n_trail)++;
            }
        }
    }

    ncc_token_stream_free(ts);
    ncc_scanner_free(sc);
    ncc_buffer_free(buf);

    return nullptr;
}

static void
test_trivia_attaches_right_by_default(void)
{
    size_t lead  = 0;
    size_t trail = 0;

    comments_of("a # why\nb", NCC_TRIVIA_ATTACH_RIGHT, &lead, &trail);

    check("by default a comment attaches to the token after it",
          lead == 1 && trail == 0);

    comments_of("a # why\nb", NCC_TRIVIA_ATTACH_LEFT, &lead, &trail);

    check("and a scanner can ask for the token before it instead",
          lead == 0 && trail == 1);

    check("the default is attach-right",
          ncc_scan_trivia_attach(nullptr) == NCC_TRIVIA_ATTACH_RIGHT);
}

static void
test_notrivia_clears_it(void)
{
    static const char *const G
        = "<top> @notrivia($1) ::= <a> <b>\n"
          "<a> ::= %\"x\"\n"
          "<b> ::= %\"y\"\n";

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    if (!ncc_bnf_load(ncc_string_from_cstr(G),
                      NCC_STRING_STATIC("top"), g)) {
        check("the @notrivia grammar loads", false);

        return;
    }

    ncc_buffer_t  *buf = ncc_buffer_from_bytes("x # gap\ny", 9);
    ncc_scanner_t *sc  = ncc_scanner_new(buf, comment_scan, g,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);
    ncc_token_stream_t *ts = ncc_token_stream_new(sc);
    ncc_pwz_parser_t   *p  = ncc_pwz_new(g);

    if (!ncc_pwz_parse(p, ts)) {
        check("x # gap\\ny parses", false);

        return;
    }

    ncc_parse_tree_t   *tree = ncc_pwz_get_tree(p);
    ncc_annot_result_t *r    = ncc_annot_walk_tree(g, tree, nullptr);

    ncc_parse_tree_t *b   = ncc_tree_child(tree, 1);
    ncc_token_info_t *tok = b ? ncc_tree_find_first_terminal(b) : nullptr;

    check("the @notrivia child was found", tok != nullptr);
    check("and @notrivia left it with no trivia attached",
          tok && !tok->leading_trivia && !tok->trailing_trivia
              && tok->no_trivia);

    ncc_annot_result_free(r);
    ncc_pwz_free(p);
    ncc_token_stream_free(ts);
    ncc_scanner_free(sc);
    ncc_buffer_free(buf);
    ncc_grammar_free(g);
}

int
main(void)
{
    test_trivia_attaches_right_by_default();
    test_notrivia_clears_it();

    test_a_tree_dumps_without_any_annotations();
    test_formatting_falls_back_to_one_space();
    test_annotations_win_where_they_exist();
    test_a_comment_the_caller_marks_as_leading_is_kept();

    return report("fe generic-print");
}
