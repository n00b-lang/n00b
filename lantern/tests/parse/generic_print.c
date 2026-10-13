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

static bool
toy_scan(parse_scanner_t *s)
{
restart:
    scan_skip_whitespace(s);

    if (scan_at_eof(s)) {
        return false;
    }

    if (scan_peek_byte(s, 0) == '#') {
        scan_skip_line_comment(s);
        goto restart;
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

typedef struct {
    parse_grammar_t      *g;
    parse_pwz_parser_t   *p;
    parse_token_stream_t *ts;
    parse_scanner_t      *sc;
    core_buffer_t       *buf;
    parse_tree_t   *tree;
    parse_nt_id_t start;
} run_t;

static bool
run(run_t *r, const char *bnf, const char *start, const char *src)
{
    *r      = (run_t){0};
    r->g    = parse_grammar_new();
    r->start = parse_nonterm_id(parse_nonterm(
        r->g, core_string_from_cstr(start)));

    parse_grammar_set_error_recovery(r->g, false);

    if (!parse_bnf_load(core_string_from_cstr(bnf),
                      core_string_from_cstr(start),
                      r->g)) {
        printf("FAIL  the grammar does not load\n");
        fails++;

        return false;
    }

    r->buf = core_buffer_from_bytes(src, (int64_t)strlen(src));
    r->sc  = scan_scanner_new(r->buf, toy_scan, r->g,
                             core_option_none(core_string_t), nullptr, nullptr);
    r->ts  = scan_token_stream_new(r->sc);
    r->p   = parse_pwz_new_for_start(r->g, r->start);

    if (!parse_pwz_parse(r->p, r->ts)) {
        printf("FAIL  %s does not parse\n", src);
        fails++;

        return false;
    }

    r->tree = parse_pwz_get_tree(r->p);

    return true;
}

static void
run_free(run_t *r)
{
    parse_pwz_free(r->p);
    scan_token_stream_free(r->ts);
    scan_scanner_free(r->sc);
    core_buffer_free(r->buf);
    parse_grammar_free(r->g);
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

    char *d = parse_tree_dump_str(r.g, r.tree, false);

    same("a parse tree dumps structurally with no annotations at all",
         d,
         "<prog> rule=0\n"
         "  <stmt> rule=0\n"
         "    TOKEN[1073741831] \"let\"\n"
         "    TOKEN[1073741825] \"a\"\n"
         "    TOKEN[61] \"=\"\n"
         "    TOKEN[1073741825] \"b\"\n"
         "    TOKEN[59] \";\"\n");

    core_free(d);
    run_free(&r);
}

static void
test_formatting_falls_back_to_one_space(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a   =\n\tb;    let c = d;")) {
        return;
    }

    parse_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = parse_tree_print(r.g, r.tree, &o);

    same("with no printing annotations every token is one space apart, "
         "whatever the input's whitespace was",
         s,
         "let a = b ; let c = d ;");

    core_free(s);
    run_free(&r);
}

static void
test_annotations_win_where_they_exist(void)
{
    run_t r;

    if (!run(&r, SHAPED, "prog", "let a = b; let c = d;")) {
        return;
    }

    parse_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = parse_tree_print(r.g, r.tree, &o);

    same("a production's own annotations override the fallback, and the "
         "gap between two of them belongs to the production above",
         s,
         "let\na\n=\nb; let\nc\n=\nd;");

    core_free(s);
    run_free(&r);
}

static void
test_a_comment_the_caller_marks_as_leading_is_kept(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a = b;")) {
        return;
    }

    parse_tree_t *first = r.tree;

    while (first && !core_tree_is_leaf(first)) {
        first = core_tree_child(first, 0);
    }

    parse_token_info_t *tok = first ? core_tree_leaf_value(first) : nullptr;

    check("the first token was found", tok != nullptr);

    if (!tok) {
        run_free(&r);

        return;
    }

    parse_trivia_t *tv = core_alloc(parse_trivia_t);

    tv->text = CORE_STRING_STATIC("# why");
    tv->kind = PARSE_TRIVIA_COMMENT;

    tok->leading_trivia = tv;

    parse_print_opts_t o = {.width = 200, .indent = 4};
    char            *s = parse_tree_print(r.g, r.tree, &o);

    same("comment trivia is content and keeps its own line, while the "
         "input's whitespace does not survive at all",
         s,
         "# why\nlet a = b ;");

    core_free(s);
    run_free(&r);
}

static void
test_trivia_indent_places_standalone_comment_inside_delimiters(void)
{
    const char *grammar =
        "<prog> @trivia_indent($1) ::= %\"{\" %\"}\"\n";
    const char *source = "{\n# inside\n}";
    run_t r;

    if (!run(&r, grammar, "prog", source)) return;

    parse_print_opts_t options = {
        .source = core_string_borrow(source, strlen(source)),
        .width = 80,
        .indent = 4,
    };
    char *printed = parse_tree_print(r.g, r.tree, &options);

    same("trivia indentation moves the comment without moving the closer",
         printed, "{\n    # inside\n}");

    core_free(printed);
    run_free(&r);
}

static parse_tree_t *
last_leaf(parse_tree_t *tree)
{
    while (tree && !core_tree_is_leaf(tree)) {
        size_t n = core_tree_num_children(tree);

        tree = n ? core_tree_child(tree, n - 1) : nullptr;
    }

    return tree;
}

static void
test_a_comment_attached_to_the_previous_token_is_kept(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a = b;")) {
        return;
    }

    parse_tree_t *leaf = last_leaf(r.tree);
    parse_token_info_t *tok = leaf ? core_tree_leaf_value(leaf) : nullptr;
    parse_trivia_t *tv = core_alloc(parse_trivia_t);

    tv->text = CORE_STRING_STATIC("# after");
    tv->kind = PARSE_TRIVIA_COMMENT;

    if (tok) {
        tok->trailing_trivia = tv;
    }

    parse_print_opts_t o = {.width = 200, .indent = 4};
    char *s = parse_tree_print(r.g, r.tree, &o);

    same("trailing comment trivia is printed", s,
         "let a = b ; # after\n");

    core_free(s);
    run_free(&r);
}

static void
test_a_final_comment_is_printed(void)
{
    run_t r;

    if (!run(&r, PLAIN, "prog", "let a = b;\n# final")) {
        return;
    }

    parse_print_opts_t o = {.width = 200, .indent = 4};
    char *s = parse_tree_print(r.g, r.tree, &o);

    same("a final attach-right comment survives parsing and printing", s,
         "let a = b ;\n# final\n");

    core_free(s);
    run_free(&r);
}

static bool
comment_scan(parse_scanner_t *s)
{
restart:
    scan_skip_whitespace(s);

    if (scan_at_eof(s)) {
        return false;
    }

    if (scan_peek_byte(s, 0) == '#') {
        scan_skip_line_comment(s);
        goto restart;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c == '_' || (c >= 'a' && c <= 'z')) {
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

static char *
comments_of(const char *src, scan_trivia_attach_t how, size_t *n_lead,
            size_t *n_trail)
{
    core_buffer_t  *buf = core_buffer_from_bytes(src, (int64_t)strlen(src));
    parse_scanner_t *sc  = scan_scanner_new(buf, comment_scan, nullptr,
                                         core_option_none(core_string_t),
                                         nullptr, nullptr);

    scan_set_trivia_attach(sc, how);

    parse_token_stream_t *ts = scan_token_stream_new(sc);
    parse_token_info_t   *t;

    *n_lead  = 0;
    *n_trail = 0;

    while (scan_stream_next(ts)) {
        ;
    }

    for (int32_t i = 0; i < scan_stream_token_count(ts); i++) {
        t = scan_stream_get(ts, i);

        for (parse_trivia_t *tv = t->leading_trivia; tv; tv = tv->next) {
            if (tv->kind == PARSE_TRIVIA_COMMENT) {
                (*n_lead)++;
            }
        }

        for (parse_trivia_t *tv = t->trailing_trivia; tv; tv = tv->next) {
            if (tv->kind == PARSE_TRIVIA_COMMENT) {
                (*n_trail)++;
            }
        }
    }

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);

    return nullptr;
}

static void
test_trivia_attaches_right_by_default(void)
{
    size_t lead  = 0;
    size_t trail = 0;

    comments_of("a # why\nb", SCAN_TRIVIA_ATTACH_RIGHT, &lead, &trail);

    check("by default a comment attaches to the token after it",
          lead == 1 && trail == 0);

    comments_of("a # why\nb", SCAN_TRIVIA_ATTACH_LEFT, &lead, &trail);

    check("and a scanner can ask for the token before it instead",
          lead == 0 && trail == 1);

    check("the default is attach-right",
          scan_trivia_attach(nullptr) == SCAN_TRIVIA_ATTACH_RIGHT);
}

static void
test_final_trivia_attaches_to_the_last_token(void)
{
    const char *src = "a\n# final";
    core_buffer_t *buf = core_buffer_from_bytes(src, (int64_t)strlen(src));
    parse_scanner_t *sc = scan_scanner_new(
        buf, comment_scan, nullptr, core_option_none(core_string_t), nullptr,
        nullptr);
    parse_token_stream_t *ts = scan_token_stream_new(sc);

    while (scan_stream_next(ts)) {
        ;
    }

    parse_token_info_t *tok = scan_stream_get(ts, 0);
    size_t comments = 0;

    for (parse_trivia_t *tv = tok ? tok->trailing_trivia : nullptr; tv;
         tv = tv->next) {
        if (tv->kind == PARSE_TRIVIA_COMMENT) {
            comments++;
        }
    }

    check("attach-right preserves a final comment", comments == 1);

    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
}

static void
test_notrivia_clears_it(void)
{
    static const char *const G
        = "<top> @notrivia($1) ::= <a> <b>\n"
          "<a> ::= %\"x\"\n"
          "<b> ::= %\"y\"\n";

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);

    if (!parse_bnf_load(core_string_from_cstr(G),
                      CORE_STRING_STATIC("top"), g)) {
        check("the @notrivia grammar loads", false);

        return;
    }

    core_buffer_t  *buf = core_buffer_from_bytes("x # gap\ny", 9);
    parse_scanner_t *sc  = scan_scanner_new(buf, comment_scan, g,
                                         core_option_none(core_string_t),
                                         nullptr, nullptr);
    parse_token_stream_t *ts = scan_token_stream_new(sc);
    parse_pwz_parser_t   *p
        = parse_pwz_new_for_start(g, parse_nonterm_id(
              parse_find_nonterm(g, CORE_STRING_STATIC("top"))));

    if (!parse_pwz_parse(p, ts)) {
        check("x # gap\\ny parses", false);

        return;
    }

    parse_tree_t   *tree = parse_pwz_get_tree(p);
    parse_annot_result_t *r = parse_annot_result_new(nullptr);
    parse_tree_walk_t *walk = parse_tree_walk_new(g, r);
    parse_annot_register(walk, r, nullptr, nullptr);
    parse_tree_walk_run(walk, tree);
    parse_tree_walk_free(walk);

    parse_tree_t *b   = core_tree_child(tree, 1);
    parse_token_info_t *tok = b ? parse_tree_find_first_terminal(b) : nullptr;

    check("the @notrivia child was found", tok != nullptr);
    check("and @notrivia left it with no trivia attached",
          tok && !tok->leading_trivia && !tok->trailing_trivia
              && tok->no_trivia);

    parse_annot_result_free(r);
    parse_pwz_free(p);
    scan_token_stream_free(ts);
    scan_scanner_free(sc);
    core_buffer_free(buf);
    parse_grammar_free(g);
}

int
main(void)
{
    test_trivia_attaches_right_by_default();
    test_final_trivia_attaches_to_the_last_token();
    test_notrivia_clears_it();

    test_a_tree_dumps_without_any_annotations();
    test_formatting_falls_back_to_one_space();
    test_annotations_win_where_they_exist();
    test_trivia_indent_places_standalone_comment_inside_delimiters();
    test_a_comment_the_caller_marks_as_leading_is_kept();
    test_a_comment_attached_to_the_previous_token_is_kept();
    test_a_final_comment_is_printed();

    return report("fe generic-print");
}
