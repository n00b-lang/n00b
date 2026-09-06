#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/grammar.h"

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/scanner.h"
#include "scanner/scan_builtins.h"
#include "lib/buffer.h"
#include "scanner/token_stream.h"

#include "ir_bnf.h"
#include "typelang_bnf.h"

static ncc_grammar_t *grammar;

static int64_t tid_of_name(ncc_grammar_t *g, const char *name)
{
    return ncc_lookup_terminal(g, ncc_string_from_cstr(name));
}

static const char *const DECLARED[] = {"INT_LIT", "FLOAT_LIT", "STRING_LIT"};

ncc_grammar_t *ir_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);

    for (size_t i = 0; i < sizeof(DECLARED) / sizeof(DECLARED[0]); i++) {
        ncc_register_terminal(g, ncc_string_from_cstr(DECLARED[i]));
    }

    bool ok = ncc_bnf_load(ncc_string_from_cstr(TYPELANG_BNF),
                           ncc_string_from_cstr("module"), g);

    ok = ncc_bnf_load(ncc_string_from_cstr(IR_BNF),
                      ncc_string_from_cstr("module"), g)
      && ok;

    if (!ok) {
        fprintf(stderr, "ir: the IR grammar does not load\n");
        ncc_grammar_free(g);

        return nullptr;
    }

    grammar = g;

    return grammar;
}

int64_t ir_grammar_nt(const char *name)
{
    ncc_grammar_t *g = ir_grammar();

    if (!g) {
        return -1;
    }

    for (size_t i = 0; i < ncc_list_len(g->nt_list); i++) {
        ncc_nonterm_t *nt = g->nt_list.data[i];

        if (nt->name.data && !strcmp(nt->name.data, name)) {
            return nt->id;
        }
    }

    return -1;
}

int64_t ir_grammar_tid(const char *name)
{
    ncc_grammar_t *g = ir_grammar();

    return g ? tid_of_name(g, name) : -1;
}

void ir_grammar_shutdown(void)
{
    if (grammar) {
        ncc_grammar_free(grammar);
        grammar = nullptr;
    }
}

static int32_t tid_int;
static int32_t tid_float;
static int32_t tid_str;

static bool word_start(ncc_codepoint_t c, void *ctx)
{
    (void)ctx;

    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80;
}

static bool after_sigil(ncc_scanner_t *s)
{
    ncc_token_info_t *prev = ncc_scan_prev_token(s);

    return prev && (prev->tid == '%' || prev->tid == '$' || prev->tid == '@');
}

static bool ir_scan(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek_byte(s, 0) == '#') {
        ncc_scan_mark(s);
        ncc_scan_skip_line_comment(s);
        ncc_scan_add_leading_trivia(s, ncc_scan_extract(s));
        goto restart;
    }

    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_emit(s, tid_str, ncc_scan_string_double(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        if (ncc_scan_number(s, tid_int, tid_float)) {
            return true;
        }
    }

    if (word_start(c, nullptr)) {
        bool                       name = after_sigil(s);
        ncc_option_t(ncc_string_t) id   = ncc_scan_identifier(s);

        if (ncc_option_is_set(id)) {
            ncc_string_t text = ncc_option_get(id);
            int64_t      kw   = name ? NCC_TOK_OTHER
                                     : ncc_scan_terminal_id(s, text.data);

            ncc_scan_emit(s, kw != NCC_TOK_OTHER ? (int32_t)kw
                                                 : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    ncc_codepoint_t next = ncc_scan_peek(s, 1);
    char            two[3] = {(char)c, (char)next, 0};

    if (next > 0 && next < 128 && !isspace((int)next)) {
        int64_t op = ncc_scan_terminal_id(s, two);

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

ncc_token_stream_t *ir_lex(const char *src, size_t len)
{
    ncc_grammar_t *g = ir_grammar();

    if (!g || !src) {
        return nullptr;
    }

    tid_int   = (int32_t)tid_of_name(g, "INT_LIT");
    tid_float = (int32_t)tid_of_name(g, "FLOAT_LIT");
    tid_str   = (int32_t)tid_of_name(g, "STRING_LIT");

    ncc_buffer_t  *buf = ncc_buffer_from_bytes(src, (int64_t)len);
    ncc_scanner_t *sc  = ncc_scanner_new(buf, ir_scan, g,
                                         ncc_option_none(ncc_string_t),
                                         nullptr, nullptr);

    return ncc_token_stream_new(sc);
}
