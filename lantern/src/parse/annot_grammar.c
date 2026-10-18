#include "private/parse/annot_grammar.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "private/parse/grammar.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "scanner/tokenizer.h"
#include "unicode/identifiers.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include "annot_bnf.h"
#include "infer_bnf.h"
#include "lex_bnf.h"

#include <pthread.h>
#include <string.h>

static parse_grammar_t *grammar;
static _Thread_local parse_grammar_t *building;
static pthread_mutex_t grammar_lock = PTHREAD_MUTEX_INITIALIZER;
static bool annot_scan(parse_scanner_t *s);

bool
parse_annot_building(void)
{
    return building != nullptr;
}

static bool
load_hook(parse_grammar_t *g, core_string_t text, core_string_t start,
          core_string_t hook)
{
    if (!text.data || !start.data) {
        return true;
    }

    if (!parse_bnf_load(text, start, g)) {
        return false;
    }

    parse_nonterm_t *lhs = parse_nonterm(g, hook);
    parse_match_t    m   = {.kind  = PARSE_MATCH_NT,
                          .nt_id = parse_nonterm(g, start)->id};

    parse_add_rule_v(g, lhs->id, 1, &m);

    return true;
}

static const int64_t type_stops[] = {
    ',', '[', ']', '(', ')', PARSE_TOK_STRING_LIT,
};

static const parse_match_set_t type_atom = {
    .ids    = type_stops,
    .n      = sizeof type_stops / sizeof type_stops[0],
    .negate = true,
};

static const int64_t nest_stops[] = {'[', ']', '(', ')'};

static const parse_match_set_t nest_atom = {
    .ids    = nest_stops,
    .n      = sizeof nest_stops / sizeof nest_stops[0],
    .negate = true,
};

static void
add_type_span(parse_grammar_t *g)
{
    parse_nonterm_t *type = parse_nonterm(g, CORE_STRING_STATIC("type"));
    parse_nonterm_t *piece
        = parse_nonterm(g, CORE_STRING_STATIC("type-piece"));
    parse_nonterm_t *nest = parse_nonterm(g, CORE_STRING_STATIC("type-nest"));
    parse_nonterm_t *npc
        = parse_nonterm(g, CORE_STRING_STATIC("type-nest-piece"));

    parse_add_rule(g, type, PARSE_NT(piece));
    parse_add_rule(g, type, PARSE_NT(piece), PARSE_NT(type));

    parse_add_rule(g, piece, PARSE_SET(&type_atom));
    parse_add_rule(g, piece, PARSE_CHAR('['), PARSE_CHAR(']'));
    parse_add_rule(g, piece, PARSE_CHAR('['), PARSE_NT(nest), PARSE_CHAR(']'));
    parse_add_rule(g, piece, PARSE_CHAR('('), PARSE_CHAR(')'));
    parse_add_rule(g, piece, PARSE_CHAR('('), PARSE_NT(nest), PARSE_CHAR(')'));

    parse_add_rule(g, nest, PARSE_NT(npc));
    parse_add_rule(g, nest, PARSE_NT(npc), PARSE_NT(nest));

    parse_add_rule(g, npc, PARSE_SET(&nest_atom));
    parse_add_rule(g, npc, PARSE_CHAR('['), PARSE_CHAR(']'));
    parse_add_rule(g, npc, PARSE_CHAR('['), PARSE_NT(nest), PARSE_CHAR(']'));
    parse_add_rule(g, npc, PARSE_CHAR('('), PARSE_CHAR(')'));
    parse_add_rule(g, npc, PARSE_CHAR('('), PARSE_NT(nest), PARSE_CHAR(')'));
}

parse_grammar_t *
parse_annot_grammar(void)
{
    if (building) {
        return building;
    }

    pthread_mutex_lock(&grammar_lock);

    if (grammar) {
        parse_grammar_t *result = grammar;

        pthread_mutex_unlock(&grammar_lock);

        return result;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    scan_tokenizer_register(CORE_STRING_STATIC("annotation-bootstrap"),
                            annot_scan);
    scan_grammar_set_tokenizer(g,
                               CORE_STRING_STATIC("annotation-bootstrap"));

    building = g;

    bool ok = parse_bnf_load(CORE_STRING_STATIC(ANNOT_BNF),
                           CORE_STRING_STATIC("a-ref"),
                           g);

    ok = ok && parse_bnf_load(CORE_STRING_STATIC(LEX_BNF),
                              CORE_STRING_STATIC("lex-file"), g);

    ok = ok
      && load_hook(g,
                   CORE_STRING_STATIC(INFER_BNF),
                   CORE_STRING_STATIC("infer-expr"),
                   CORE_STRING_STATIC("infer"));

    if (ok) {
        add_type_span(g);
    }

    building = nullptr;

    if (!ok) {
        parse_grammar_free(g);
        pthread_mutex_unlock(&grammar_lock);

        return nullptr;
    }

    grammar = g;
    pthread_mutex_unlock(&grammar_lock);

    return grammar;
}

void
parse_annot_grammar_shutdown(void)
{
    pthread_mutex_lock(&grammar_lock);

    if (grammar) {
        parse_grammar_free(grammar);
        grammar = nullptr;
    }

    pthread_mutex_unlock(&grammar_lock);
}

static core_option_t(core_string_t)
scan_ident(parse_scanner_t *s)
{
    if (!unicode_is_id_start(scan_peek(s, 0))) {
        return core_option_none(core_string_t);
    }

    scan_mark(s);
    scan_advance(s);

    while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (c == '-') {
            if (!unicode_is_id_continue(scan_peek(s, 1))) {
                break;
            }

            scan_advance(s);

            continue;
        }

        if (!unicode_is_id_continue(c)) {
            break;
        }

        scan_advance(s);
    }

    return core_option_set(core_string_t, scan_extract(s));
}

static bool
after_tick(parse_scanner_t *s)
{
    parse_token_info_t *prev = scan_prev_token(s);

    return prev && parse_token_tid(prev) == '`';
}

static bool
annot_scan(parse_scanner_t *s)
{
    for (;;) {
        scan_skip_whitespace(s);
        if (scan_peek(s, 0) != '#') {
            break;
        }
        while (!scan_at_eof(s) && scan_peek(s, 0) != '\n') {
            scan_advance(s);
        }
    }

    if (scan_at_eof(s)) {
        return false;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c == '"') {
        scan_emit_owned(s, PARSE_TOK_STRING_LIT, scan_string_double(s));

        return true;
    }

    if (c == '\'') {
        scan_emit_owned(s, PARSE_TOK_STRING_LIT, scan_string_single(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        scan_emit_owned(s, PARSE_TOK_INTEGER, scan_integer(s));

        return true;
    }

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80) {
        bool                       name = after_tick(s);
        core_option_t(core_string_t) id   = scan_ident(s);

        if (core_option_is_set(id)) {
            int64_t kw = name ? PARSE_TOK_OTHER
                              : scan_terminal_id(s,
                                                     core_option_get(id));

            scan_emit_owned(s,
                            kw != PARSE_TOK_OTHER ? (int32_t)kw
                                                  : PARSE_TOK_IDENTIFIER,
                            id);

            return true;
        }
    }

    if (c == '.' && scan_peek(s, 1) == '.' && scan_peek(s, 2) == '.') {
        int64_t op = scan_terminal_id(s, CORE_STRING_STATIC("..."));

        if (op != PARSE_TOK_OTHER && op > 0xFF) {
            scan_advance_n(s, 3);
            scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    core_codepoint_t next = scan_peek(s, 1);

    if (next > 0 && next < 128) {
        char two[3] = {(char)c, (char)next, 0};

        int64_t op = scan_terminal_id(s, core_string_borrow(two, 2));

        if (op != PARSE_TOK_OTHER && op > 0xFF) {
            scan_advance_n(s, 2);
            scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);

    return true;
}

parse_token_stream_t *
parse_annot_lex(core_string_t text, core_buffer_t **buf_out,
              parse_scanner_t **sc_out)
{
    parse_grammar_t *g = parse_annot_grammar();

    if (!g || !text.data) {
        return nullptr;
    }

    core_buffer_t  *buf = core_buffer_from_bytes(text.data,
                                               (int64_t)text.u8_bytes);
    parse_scanner_t *sc  = scan_scanner_new(buf, annot_scan, g,
                                         core_option_none(core_string_t),
                                         nullptr, nullptr);

    *buf_out = buf;
    *sc_out  = sc;

    return scan_token_stream_new(sc);
}

void
parse_annot_lex_free(parse_token_stream_t *ts, core_buffer_t *buf,
                   parse_scanner_t *sc)
{
    if (ts) {
        scan_token_stream_free(ts);
    }

    if (sc) {
        scan_scanner_free(sc);
    }

    if (buf) {
        core_buffer_free(buf);
    }
}
