#include "scanner/tokenizer.h"
#include "scanner/scan_builtins.h"
#include "unicode/identifiers.h"

#include <string.h>

static int32_t
named(ncc_scanner_t *s, ncc_string_t name, int32_t fallback)
{
    int64_t id = ncc_scan_terminal_id(s, name);

    return id != NCC_TOK_OTHER ? (int32_t)id : fallback;
}

static void
skip_set(ncc_scanner_t *s, const char *set)
{
    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t c = ncc_scan_peek(s, 0);

        if (c > 127 || !strchr(set, (char)c)) {
            break;
        }

        ncc_scan_advance(s);
    }
}

static void
punct(ncc_scanner_t *s)
{
    ncc_scan_mark(s);

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    ncc_scan_advance(s);
    ncc_scan_emit_marked(s, (int32_t)c);
}

static bool
scan_json(ncc_scanner_t *s)
{
    skip_set(s, " \t\n\r");

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_mark(s);
        ncc_scan_emit(s, named(s, NCC_STRING_STATIC("STRING_LIT"), NCC_TOK_STRING_LIT),
                      ncc_scan_string_double(s));

        return true;
    }

    if (c == '-' || ncc_cp_is_ascii_digit(c)) {
        ncc_scan_mark(s);

        if (c == '-') {
            ncc_scan_advance(s);
        }

        bool seen_dot = false;
        bool seen_exp = false;

        while (!ncc_scan_at_eof(s)) {
            ncc_codepoint_t k = ncc_scan_peek(s, 0);

            if (ncc_cp_is_ascii_digit(k)) {
                ncc_scan_advance(s);

                continue;
            }

            if (k == '.' && !seen_dot && !seen_exp) {
                seen_dot = true;
                ncc_scan_advance(s);

                continue;
            }

            if ((k == 'e' || k == 'E') && !seen_exp) {
                seen_exp = true;
                ncc_scan_advance(s);

                if (!ncc_scan_at_eof(s)) {
                    ncc_codepoint_t sign = ncc_scan_peek(s, 0);

                    if (sign == '+' || sign == '-') {
                        ncc_scan_advance(s);
                    }
                }

                continue;
            }

            break;
        }

        ncc_scan_emit_marked(s, seen_dot || seen_exp
                                    ? named(s, NCC_STRING_STATIC("FLOAT_LIT"), NCC_TOK_FLOAT)
                                    : named(s, NCC_STRING_STATIC("INT_LIT"), NCC_TOK_INTEGER));

        return true;
    }

    if (ncc_unicode_is_id_start(c)) {
        ncc_scan_mark(s);

        ncc_option_t(ncc_string_t) id = ncc_scan_identifier(s);

        if (ncc_option_is_set(id)) {
            ncc_string_t text = ncc_option_get(id);
            int64_t      kw   = ncc_scan_terminal_id(s, text);

            ncc_scan_emit(s,
                          kw != NCC_TOK_OTHER ? (int32_t)kw
                                              : NCC_TOK_IDENTIFIER,
                          id);

            return true;
        }
    }

    punct(s);

    return true;
}

static bool
scan_lisp(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek(s, 0) == ';') {
        ncc_scan_skip_line_comment(s);

        goto restart;
    }

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_mark(s);
        ncc_scan_emit(s, named(s, NCC_STRING_STATIC("STRING_LIT"), NCC_TOK_STRING_LIT),
                      ncc_scan_string_double(s));

        return true;
    }

    if (ncc_cp_is_ascii_digit(c)
        || ((c == '-' || c == '+') && ncc_cp_is_ascii_digit(ncc_scan_peek(s, 1)))) {
        ncc_scan_mark(s);

        if (c == '-' || c == '+') {
            ncc_scan_advance(s);
        }

        if (ncc_scan_number(s, named(s, NCC_STRING_STATIC("INT_LIT"), NCC_TOK_INTEGER),
                            named(s, NCC_STRING_STATIC("FLOAT_LIT"), NCC_TOK_FLOAT))) {
            return true;
        }
    }

    if (c == '(' || c == ')' || c == '\'' || c == '`' || c == ',') {
        punct(s);

        return true;
    }

    ncc_scan_mark(s);

    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t k = ncc_scan_peek(s, 0);

        if (k <= ' ' || k == '(' || k == ')' || k == '"' || k == ';'
            || k == '\'' || k == '`' || k == ',') {
            break;
        }

        ncc_scan_advance(s);
    }

    ncc_string_t sym = ncc_scan_extract(s);

    if (!sym.u8_bytes) {
        punct(s);

        return true;
    }

    int64_t kw = ncc_scan_terminal_id(s, sym);

    ncc_scan_emit(s,
                  kw != NCC_TOK_OTHER ? (int32_t)kw
                                      : named(s, NCC_STRING_STATIC("SYMBOL"),
                                              NCC_TOK_IDENTIFIER),
                  ncc_option_set(ncc_string_t, sym));

    return true;
}

static bool
scan_shell(ncc_scanner_t *s)
{
restart:
    skip_set(s, " \t");

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek(s, 0) == '#') {
        ncc_scan_skip_line_comment(s);

        goto restart;
    }

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"' || c == '\'') {
        ncc_scan_mark(s);
        ncc_scan_emit(s,
                      named(s, NCC_STRING_STATIC("STRING_LIT"), NCC_TOK_STRING_LIT),
                      c == '"' ? ncc_scan_string_double(s)
                               : ncc_scan_string_single(s));

        return true;
    }

    static const ncc_string_t OPS[] = {
        NCC_STRING_STATIC("&&"), NCC_STRING_STATIC("||"),
        NCC_STRING_STATIC(">>"), NCC_STRING_STATIC("<<"),
        NCC_STRING_STATIC(";;"), NCC_STRING_STATIC("|&"),
    };

    ncc_codepoint_t next = ncc_scan_peek(s, 1);

    for (size_t i = 0; next > 0 && i < sizeof(OPS) / sizeof(OPS[0]); i++) {
        if (OPS[i].data[0] == (char)c && OPS[i].data[1] == (char)next) {
            ncc_scan_mark(s);
            ncc_scan_advance_n(s, 2);
            ncc_scan_emit_marked(s, named(s, OPS[i], (int32_t)c));

            return true;
        }
    }

    if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '('
        || c == ')' || c == '\n') {
        punct(s);

        return true;
    }

    ncc_scan_mark(s);

    while (!ncc_scan_at_eof(s)) {
        ncc_codepoint_t k = ncc_scan_peek(s, 0);

        if (k <= ' ' || k == '|' || k == '&' || k == ';' || k == '<'
            || k == '>' || k == '(' || k == ')' || k == '"' || k == '\''
            || k == '#') {
            break;
        }

        ncc_scan_advance(s);
    }

    ncc_string_t word = ncc_scan_extract(s);

    if (!word.u8_bytes) {
        punct(s);

        return true;
    }

    int64_t kw = ncc_scan_terminal_id(s, word);

    ncc_scan_emit(s,
                  kw != NCC_TOK_OTHER ? (int32_t)kw
                                      : named(s, NCC_STRING_STATIC("WORD"), NCC_TOK_IDENTIFIER),
                  ncc_option_set(ncc_string_t, word));

    return true;
}

static bool
scan_c(ncc_scanner_t *s)
{
restart:
    ncc_scan_skip_whitespace(s);

    if (ncc_scan_at_eof(s)) {
        return false;
    }

    if (ncc_scan_peek_byte(s, 0) == '/' && ncc_scan_peek_byte(s, 1) == '/') {
        ncc_scan_skip_line_comment(s);

        goto restart;
    }

    if (ncc_scan_peek_byte(s, 0) == '/' && ncc_scan_peek_byte(s, 1) == '*') {
        ncc_scan_skip_block_comment(s, NCC_STRING_STATIC("/*"),
                                    NCC_STRING_STATIC("*/"));

        goto restart;
    }

    ncc_codepoint_t c = ncc_scan_peek(s, 0);

    if (c == '"') {
        ncc_scan_mark(s);
        ncc_scan_emit(s, named(s, NCC_STRING_STATIC("STRING_LIT"), NCC_TOK_STRING_LIT),
                      ncc_scan_string_double(s));

        return true;
    }

    if (c == '\'') {
        ncc_scan_mark(s);
        ncc_scan_emit(s, named(s, NCC_STRING_STATIC("CHAR_LIT"), NCC_TOK_CHAR_LIT),
                      ncc_scan_string_single(s));

        return true;
    }

    if (ncc_cp_is_ascii_digit(c)
        || (c == '.' && ncc_cp_is_ascii_digit((ncc_codepoint_t)ncc_scan_peek_byte(s, 1)))) {
        ncc_scan_mark(s);

        if (ncc_scan_number(s, named(s, NCC_STRING_STATIC("INT_LIT"), NCC_TOK_INTEGER),
                            named(s, NCC_STRING_STATIC("FLOAT_LIT"), NCC_TOK_FLOAT))) {
            return true;
        }
    }

    if (ncc_unicode_is_id_start(c)) {
        ncc_scan_mark(s);

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

    static const ncc_string_t THREE[] = {
        NCC_STRING_STATIC("<<="),
        NCC_STRING_STATIC(">>="),
        NCC_STRING_STATIC("..."),
        {0},
    };
    static const ncc_string_t TWO[] = {
        NCC_STRING_STATIC("->"), NCC_STRING_STATIC("++"),
        NCC_STRING_STATIC("--"), NCC_STRING_STATIC("<<"),
        NCC_STRING_STATIC(">>"), NCC_STRING_STATIC("<="),
        NCC_STRING_STATIC(">="), NCC_STRING_STATIC("=="),
        NCC_STRING_STATIC("!="), NCC_STRING_STATIC("&&"),
        NCC_STRING_STATIC("||"), NCC_STRING_STATIC("+="),
        NCC_STRING_STATIC("-="), NCC_STRING_STATIC("*="),
        NCC_STRING_STATIC("/="), NCC_STRING_STATIC("%="),
        NCC_STRING_STATIC("&="), NCC_STRING_STATIC("|="),
        NCC_STRING_STATIC("^="), NCC_STRING_STATIC("::"),
        {0},
    };

    char         buf3[4] = {(char)c,
                            (char)ncc_scan_peek_byte(s, 1),
                            (char)ncc_scan_peek_byte(s, 2),
                            0};
    ncc_string_t three   = ncc_string_borrow(buf3, 3);

    for (size_t i = 0; THREE[i].data; i++) {
        if (ncc_string_eq(THREE[i], three)) {
            ncc_scan_mark(s);
            ncc_scan_advance_n(s, 3);
            ncc_scan_emit_marked(s, named(s, THREE[i], (int32_t)c));

            return true;
        }
    }

    char         buf2[3] = {(char)c, (char)ncc_scan_peek_byte(s, 1), 0};
    ncc_string_t two     = ncc_string_borrow(buf2, 2);

    for (size_t i = 0; buf2[1] && TWO[i].data; i++) {
        if (ncc_string_eq(TWO[i], two)) {
            ncc_scan_mark(s);
            ncc_scan_advance_n(s, 2);
            ncc_scan_emit_marked(s, named(s, TWO[i], (int32_t)c));

            return true;
        }
    }

    punct(s);

    return true;
}

void
ncc_tokenizers_register_builtin(void)
{
    ncc_tokenizer_register(NCC_STRING_STATIC("json"), scan_json);
    ncc_tokenizer_register(NCC_STRING_STATIC("lisp"), scan_lisp);
    ncc_tokenizer_register(NCC_STRING_STATIC("shell"), scan_shell);
    ncc_tokenizer_register(NCC_STRING_STATIC("c"), scan_c);
}
