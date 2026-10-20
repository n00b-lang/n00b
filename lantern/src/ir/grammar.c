#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "private/ir/internal.h"
#include "ir/grammar.h"

#include "ir/typelang.h"
#include "parse/annot_args.h"
#include "parse/bnf.h"
#include "parse/pwz.h"
#include "private/parse/grammar.h"
#include "scanner/scanner.h"
#include "unicode/identifiers.h"
#include "scanner/scan_builtins.h"
#include "scanner/tokenizer.h"
#include "lib/buffer.h"
#include "scanner/token_stream.h"

#include "ir_bnf.h"
#include "typelang_bnf.h"

static parse_grammar_t *grammar;
static int32_t        ir_unit;

static int64_t tid_of_name(parse_grammar_t *g, core_string_t name)
{
    return parse_lookup_terminal(g, name);
}

static const core_string_t DECLARED[] = {
    CORE_STRING_STATIC("INT_LIT"),
    CORE_STRING_STATIC("FLOAT_LIT"),
    CORE_STRING_STATIC("STRING_LIT"),
    CORE_STRING_STATIC("NEWLINE"),
};

static bool ir_scan(parse_scanner_t *s);

parse_grammar_t *ir_grammar(void)
{
    if (grammar) {
        return grammar;
    }

    parse_grammar_t *g = parse_grammar_new();

    parse_grammar_set_error_recovery(g, false);
    scan_tokenizer_register(CORE_STRING_STATIC("ir"), ir_scan);

    for (size_t i = 0; i < sizeof(DECLARED) / sizeof(DECLARED[0]); i++) {
        parse_register_terminal(g, DECLARED[i]);
    }


    bool ok = parse_bnf_load(core_string_borrow(TYPELANG_BNF,
                                                sizeof(TYPELANG_BNF) - 1),
                           CORE_STRING_STATIC("module"), g);

    ok = parse_bnf_load(core_string_borrow(IR_BNF, sizeof(IR_BNF) - 1),
                      CORE_STRING_STATIC("module"), g)
      && ok;

    ir_unit = parse_bnf_last_unit();

    if (!ok) {
        fprintf(stderr, "ir: the IR grammar does not load\n");
        parse_grammar_free(g);

        return nullptr;
    }

    grammar = g;

    return grammar;
}

int64_t ir_grammar_nt(core_string_t name)
{
    parse_grammar_t *g = ir_grammar();

    parse_nonterm_t *nt = g ? parse_find_nonterm(g, name) : nullptr;

    return nt ? nt->id : -1;
}

int32_t ir_grammar_unit(void)
{
    ir_grammar();

    return ir_unit;
}

int64_t ir_grammar_tid(core_string_t name)
{
    parse_grammar_t *g = ir_grammar();

    return g ? tid_of_name(g, name) : -1;
}

void ir_grammar_shutdown(void)
{
    if (grammar) {
        parse_grammar_free(grammar);
        grammar = nullptr;
    }
}

static int32_t tid_int;
static int32_t tid_float;
static int32_t tid_str;
static int32_t tid_nl;

static bool after_sigil(parse_scanner_t *s)
{
    parse_token_info_t *prev = scan_prev_token(s);

    if (!prev || (parse_token_tid(prev) != '%' && parse_token_tid(prev) != '$'
                  && parse_token_tid(prev) != '@')) {
        return false;
    }

    return parse_token_end_byte(prev) == (size_t)scan_offset(s);
}

static bool ir_scan(parse_scanner_t *s)
{
    bool broke = false;

restart:
    while (!scan_at_eof(s)) {
        core_codepoint_t c = scan_peek(s, 0);

        if (c == '\n' || c == '\r') {
            broke = true;
            scan_advance(s);
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\v' || c == '\f') {
            scan_advance(s);
            continue;
        }
        break;
    }

    if (broke) {
        scan_mark(s);
        scan_emit_marked(s, tid_nl);

        return true;
    }

    if (scan_at_eof(s)) {
        return false;
    }

    if (scan_peek_byte(s, 0) == '#') {
        scan_mark(s);
        scan_skip_line_comment(s);

        while (!scan_at_eof(s)
               && (scan_peek(s, 0) == '\n' || scan_peek(s, 0) == '\r')) {
            scan_advance(s);
        }
        goto restart;
    }

    scan_mark(s);

    core_codepoint_t c = scan_peek(s, 0);

    if (c == '"') {
        scan_emit_owned(s, tid_str, scan_string_double(s));

        return true;
    }

    if (c >= '0' && c <= '9') {
        if (scan_number(s, tid_int, tid_float)) {
            return true;
        }
    }

    if (unicode_is_id_start(c)) {
        bool                       name = after_sigil(s);
        core_option_t(core_string_t) id   = scan_identifier(s);

        if (core_option_is_set(id)) {
            core_string_t text = core_option_get(id);
            int64_t      kw   = name ? PARSE_TOK_OTHER
                                     : scan_terminal_id(s, text);

            scan_emit_owned(s, kw != PARSE_TOK_OTHER ? (int32_t)kw
                                                       : PARSE_TOK_IDENTIFIER,
                            id);

            return true;
        }
    }

    core_codepoint_t next = scan_peek(s, 1);
    char            two[3] = {(char)c, (char)next, 0};

    if (next > 0 && next < 128 && !isspace((int)next)) {
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

static void learn_ir_tids(parse_grammar_t *g)
{
    tid_int   = (int32_t)tid_of_name(g, CORE_STRING_STATIC("INT_LIT"));
    tid_float = (int32_t)tid_of_name(g, CORE_STRING_STATIC("FLOAT_LIT"));
    tid_str   = (int32_t)tid_of_name(g, CORE_STRING_STATIC("STRING_LIT"));
    tid_nl    = (int32_t)tid_of_name(g, CORE_STRING_STATIC("NEWLINE"));
}

parse_token_stream_t *ir_lex(core_string_t src)
{
    parse_grammar_t *g = ir_grammar();

    if (!g || !src.data) {
        return nullptr;
    }

    learn_ir_tids(g);

    return scan_tokenize(g, src, nullptr, nullptr);
}
