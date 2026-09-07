#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/grammar.h"

#include "ir/typelang.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/scanner.h"
#include "scanner/scan_builtins.h"
#include "scanner/tokenizer.h"
#include "lib/buffer.h"
#include "scanner/token_stream.h"

#include "ir_bnf.h"
#include "typelang_bnf.h"

static ncc_grammar_t *grammar;
static int32_t        ir_unit;

static int64_t tid_of_name(ncc_grammar_t *g, const char *name)
{
    return ncc_lookup_terminal(g, ncc_string_borrow(name,
                                                    strlen(name)));
}

static const ncc_string_t DECLARED[] = {
    NCC_STRING_STATIC("INT_LIT"),
    NCC_STRING_STATIC("FLOAT_LIT"),
    NCC_STRING_STATIC("STRING_LIT"),
    NCC_STRING_STATIC("NEWLINE"),
};

static bool ir_scan(ncc_scanner_t *s);

ncc_grammar_t *ir_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    ncc_grammar_t *g = ncc_grammar_new();

    ncc_grammar_set_error_recovery(g, false);
    ncc_tokenizer_register(NCC_STRING_STATIC("ir"), ir_scan);

    for (size_t i = 0; i < sizeof(DECLARED) / sizeof(DECLARED[0]); i++) {
        ncc_register_terminal(g, DECLARED[i]);
    }


    bool ok = ncc_bnf_load(NCC_STRING_STATIC(TYPELANG_BNF),
                           NCC_STRING_STATIC("module"), g);

    ok = ncc_bnf_load(NCC_STRING_STATIC(IR_BNF),
                      NCC_STRING_STATIC("module"), g)
      && ok;

    ir_unit = ncc_bnf_last_unit();

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

    ncc_nonterm_t *nt = g ? ncc_find_nonterm(g,
                                             ncc_string_borrow(name,
                                                               strlen(name)))
                          : nullptr;

    return nt ? nt->id : -1;
}

int32_t ir_grammar_unit(void)
{
    ir_grammar();

    return ir_unit;
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
static int32_t tid_nl;

static bool word_start(ncc_codepoint_t c, void *ctx)
{
    (void)ctx;

    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || c >= 0x80;
}

static bool after_sigil(ncc_scanner_t *s)
{
    ncc_token_info_t *prev = ncc_scan_prev_token(s);

    if (!prev || (prev->tid != '%' && prev->tid != '$' && prev->tid != '@')) {
        return false;
    }

    return prev->end_byte == (size_t)ncc_scan_offset(s);
}

static bool ir_scan(ncc_scanner_t *s)
{
    bool broke = false;

restart:
    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t c = ncc_scan_peek(s, 0);

        if (c == '\n' || c == '\r') {
            broke = true;
            ncc_scan_advance(s);
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            ncc_scan_advance(s);
            continue;
        }
        break;
    }

    if (broke) {
        ncc_scan_mark(s);
        ncc_scan_emit_marked(s, tid_nl);

        return true;
    }

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek_byte(s, 0) == '#') {
        ncc_scan_mark(s);
        ncc_scan_skip_line_comment(s);

        while (!ncc_scan_at_eof(s)
               && (ncc_scan_peek(s, 0) == '\n' || ncc_scan_peek(s, 0) == '\r')) {
            ncc_scan_advance(s);
        }
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
                                     : ncc_scan_terminal_id(s, text);

            ncc_scan_emit(s, kw != NCC_TOK_OTHER ? (int32_t)kw
                                                 : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    ncc_codepoint_t next = ncc_scan_peek(s, 1);
    char            two[3] = {(char)c, (char)next, 0};

    if (next > 0 && next < 128 && !isspace((int)next)) {
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

static void learn_ir_tids(ncc_grammar_t *g)
{
    tid_int   = (int32_t)tid_of_name(g, "INT_LIT");
    tid_float = (int32_t)tid_of_name(g, "FLOAT_LIT");
    tid_str   = (int32_t)tid_of_name(g, "STRING_LIT");
    tid_nl    = (int32_t)tid_of_name(g, "NEWLINE");
}

ncc_token_stream_t *ir_lex(const char *src, size_t len)
{
    ncc_grammar_t *g = ir_grammar();

    if (!g || !src) {
        return nullptr;
    }

    learn_ir_tids(g);

    return ncc_tokenize(g, src, len, nullptr, nullptr);
}
