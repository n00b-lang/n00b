#include "internal/parse/annot_grammar.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/scan_builtins.h"
#include "scanner/scanner.h"
#include "unicode/identifiers.h"
#include "lib/alloc.h"
#include "lib/buffer.h"

#include "annot_bnf.h"
#include "infer_bnf.h"

#include <stdio.h>
#include <string.h>

static ncc_grammar_t *grammar;
static ncc_grammar_t *building;

bool
ncc_annot_building(void)
{
    return building != nullptr;
}

static bool
load_hook(ncc_grammar_t *g, ncc_string_t text, ncc_string_t start,
          ncc_string_t hook)
{
    if (!text.data || !start.data) {
        return true;
    }

    if (!ncc_bnf_load(text, start, g)) {
        return false;
    }

    ncc_nonterm_t *lhs = ncc_nonterm(g, hook);
    ncc_match_t    m   = {.kind  = NCC_MATCH_NT,
                          .nt_id = ncc_nonterm(g, start)->id};

    ncc_add_rule_v(g, lhs->id, 1, &m);

    return true;
}

static const int64_t type_stops[] = {
    ',', '[', ']', '(', ')', NCC_TOK_STRING_LIT,
};

static const ncc_match_set_t type_atom = {
    .ids    = type_stops,
    .n      = sizeof type_stops / sizeof type_stops[0],
    .negate = true,
};

static const int64_t nest_stops[] = {'[', ']', '(', ')'};

static const ncc_match_set_t nest_atom = {
    .ids    = nest_stops,
    .n      = sizeof nest_stops / sizeof nest_stops[0],
    .negate = true,
};

static void
add_type_span(ncc_grammar_t *g)
{
    ncc_nonterm_t *type = ncc_nonterm(g, NCC_STRING_STATIC("type"));
    ncc_nonterm_t *piece
        = ncc_nonterm(g, NCC_STRING_STATIC("type-piece"));
    ncc_nonterm_t *nest = ncc_nonterm(g, NCC_STRING_STATIC("type-nest"));
    ncc_nonterm_t *npc
        = ncc_nonterm(g, NCC_STRING_STATIC("type-nest-piece"));

    ncc_add_rule(g, type, NCC_NT(piece));
    ncc_add_rule(g, type, NCC_NT(piece), NCC_NT(type));

    ncc_add_rule(g, piece, NCC_SET(&type_atom));
    ncc_add_rule(g, piece, NCC_CHAR('['), NCC_CHAR(']'));
    ncc_add_rule(g, piece, NCC_CHAR('['), NCC_NT(nest), NCC_CHAR(']'));
    ncc_add_rule(g, piece, NCC_CHAR('('), NCC_CHAR(')'));
    ncc_add_rule(g, piece, NCC_CHAR('('), NCC_NT(nest), NCC_CHAR(')'));

    ncc_add_rule(g, nest, NCC_NT(npc));
    ncc_add_rule(g, nest, NCC_NT(npc), NCC_NT(nest));

    ncc_add_rule(g, npc, NCC_SET(&nest_atom));
    ncc_add_rule(g, npc, NCC_CHAR('['), NCC_CHAR(']'));
    ncc_add_rule(g, npc, NCC_CHAR('['), NCC_NT(nest), NCC_CHAR(']'));
    ncc_add_rule(g, npc, NCC_CHAR('('), NCC_CHAR(')'));
    ncc_add_rule(g, npc, NCC_CHAR('('), NCC_NT(nest), NCC_CHAR(')'));
}

ncc_grammar_t *
ncc_annot_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    if (building) {
        return building;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    building = g;

    bool ok = ncc_bnf_load(NCC_STRING_STATIC(ANNOT_BNF),
                           NCC_STRING_STATIC("a-ref"),
                           g);

    ok = ok
      && load_hook(g,
                   NCC_STRING_STATIC(INFER_BNF),
                   NCC_STRING_STATIC("infer-expr"),
                   NCC_STRING_STATIC("infer"));

    if (ok) {
        add_type_span(g);
    }

    building = nullptr;

    if (!ok) {
        fprintf(stderr,
                "bnf: the annotation-argument grammar does not load\n");
        ncc_grammar_free(g);

        return nullptr;
    }

    grammar = g;

    return grammar;
}

void
ncc_annot_grammar_shutdown(void)
{
    if (grammar) {
        ncc_grammar_free(grammar);
        grammar = nullptr;
    }
}

static ncc_option_t(ncc_string_t)
scan_ident(ncc_scanner_t *s)
{
    if (!ncc_unicode_is_id_start(ncc_scan_peek(s, 0))) {
        return ncc_option_none(ncc_string_t);
    }

    ncc_scan_mark(s);
    ncc_scan_advance(s);

    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t c = ncc_scan_peek(s, 0);

        if (c == '-') {
            if (!ncc_unicode_is_id_continue(ncc_scan_peek(s, 1))) {
                break;
            }

            ncc_scan_advance(s);

            continue;
        }

        if (!ncc_unicode_is_id_continue(c)) {
            break;
        }

        ncc_scan_advance(s);
    }

    return ncc_option_set(ncc_string_t, ncc_scan_extract(s));
}

static bool
after_tick(ncc_scanner_t *s)
{
    ncc_token_info_t *prev = ncc_scan_prev_token(s);

    return prev && prev->tid == '`';
}

static bool
annot_scan(ncc_scanner_t *s)
{
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_emit(s, NCC_TOK_STRING_LIT, ncc_scan_string_double(s));

        return true;
    }

    if (c == '\'') {
        ncc_scan_emit(s, NCC_TOK_STRING_LIT, ncc_scan_string_single(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        ncc_scan_emit(s, NCC_TOK_INTEGER, ncc_scan_integer(s));

        return true;
    }

    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80) {
        bool                       name = after_tick(s);
        ncc_option_t(ncc_string_t) id   = scan_ident(s);

        if (ncc_option_is_set(id)) {
            int64_t kw = name ? NCC_TOK_OTHER
                              : ncc_scan_terminal_id(s,
                                                     ncc_option_get(id));

            ncc_scan_emit(s,
                          kw != NCC_TOK_OTHER ? (int32_t)kw
                                              : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    if (c == '.' && ncc_scan_peek(s, 1) == '.' && ncc_scan_peek(s, 2) == '.') {
        int64_t op = ncc_scan_terminal_id(s, NCC_STRING_STATIC("..."));

        if (op != NCC_TOK_OTHER && op > 0xFF) {
            ncc_scan_advance_n(s, 3);
            ncc_scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    ncc_codepoint_t next = ncc_scan_peek(s, 1);

    if (next > 0 && next < 128) {
        char two[3] = {(char)c, (char)next, 0};

        int64_t op = ncc_scan_terminal_id(s, ncc_string_borrow(two, 2));

        if (op != NCC_TOK_OTHER && op > 0xFF) {
            ncc_scan_advance_n(s, 2);
            ncc_scan_emit_marked(s, (int32_t)op);

            return true;
        }
    }

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);

    return true;
}

ncc_token_stream_t *
ncc_annot_lex(ncc_string_t text, ncc_buffer_t **buf_out,
              ncc_scanner_t **sc_out)
{
    ncc_grammar_t *g = ncc_annot_grammar();

    if (!g || !text.data) {
        return nullptr;
    }

    ncc_buffer_t  *buf = ncc_buffer_from_bytes(text.data,
                                               (int64_t)text.u8_bytes);
    ncc_scanner_t *sc  = ncc_scanner_new(buf, annot_scan, g,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);

    *buf_out = buf;
    *sc_out  = sc;

    return ncc_token_stream_new(sc);
}

void
ncc_annot_lex_free(ncc_token_stream_t *ts, ncc_buffer_t *buf,
                   ncc_scanner_t *sc)
{
    if (ts) {
        ncc_token_stream_free(ts);
    }

    if (sc) {
        ncc_scanner_free(sc);
    }

    if (buf) {
        ncc_buffer_free(buf);
    }
}
