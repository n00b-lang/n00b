#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/grammar.h"

#include "parse/bnf.h"
#include "parse/pwz.h"
#include "internal/parse/grammar_internal.h"
#include "scanner/token_stream.h"

#include "ir_bnf.h"
#include "typelang_bnf.h"

static ncc_grammar_t *grammar;
static bool           char_terminals[128];

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

    memset(char_terminals, 0, sizeof(char_terminals));

    for (size_t i = 0; i < ncc_list_len(g->rules); i++) {
        ncc_parse_rule_t *r = &g->rules.data[i];

        for (size_t j = 0; j < ncc_list_len(r->contents); j++) {
            ncc_match_t *m = &r->contents.data[j];

            if (m->kind == NCC_MATCH_TERMINAL && m->terminal_id > 0
                && m->terminal_id < 128) {
                char_terminals[m->terminal_id] = true;
            }
        }
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
        ncc_nonterm_t *nt = &g->nt_list.data[i];

        if (nt->name.data && !strcmp(nt->name.data, name)) {
            return nt->id;
        }
    }

    return -1;
}

void ir_grammar_shutdown(void)
{
    if (grammar) {
        ncc_grammar_free(grammar);
        grammar = nullptr;
    }
}

static bool id_start(unsigned char c)
{
    return c == '_' || isalpha(c) || c >= 0x80;
}

static bool id_more(unsigned char c)
{
    return c == '_' || isalnum(c) || c >= 0x80;
}

static ncc_trivia_t *pending;

static void emit(ir_tokens_t *t, int64_t tid, const char *at, size_t len,
                 uint32_t line, uint32_t col, size_t off, size_t end)
{
    if (t->n >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->v   = realloc(t->v, (size_t)t->cap * sizeof(*t->v));
        t->at  = realloc(t->at, (size_t)t->cap * sizeof(*t->at));
        t->end = realloc(t->end, (size_t)t->cap * sizeof(*t->end));
    }

    ncc_token_info_t *tok = calloc(1, sizeof(*tok));

    tok->tid            = (int32_t)tid;
    tok->index          = t->n;
    tok->line           = line;
    tok->column         = col;
    tok->leading_trivia = pending;
    tok->value          = ncc_option_set(ncc_string_t,
                                         ncc_string_from_raw(at,
                                                             (int64_t)len));

    pending      = nullptr;
    t->at[t->n]  = off;
    t->end[t->n] = end;
    t->v[t->n++] = tok;
}

static void free_trivia(ncc_trivia_t *t)
{
    while (t) {
        ncc_trivia_t *next = t->next;

        free(t->text.data);
        free(t);
        t = next;
    }
}

void ir_tokens_free(ir_tokens_t *t)
{
    free_trivia(pending);
    pending = nullptr;

    free(t->v);
    free(t->at);
    free(t->end);
    t->v   = nullptr;
    t->at  = nullptr;
    t->end = nullptr;
    t->n   = 0;
    t->cap = 0;
}

bool ir_tokenize(const char *src, size_t len, ir_tokens_t *out)
{
    ncc_grammar_t *g = ir_grammar();

    if (!g || !src) {
        return false;
    }

    int64_t  tid_int   = tid_of_name(g, "INT_LIT");
    int64_t  tid_float = tid_of_name(g, "FLOAT_LIT");
    int64_t  tid_str   = tid_of_name(g, "STRING_LIT");
    size_t   i         = 0;
    uint32_t line      = 1;
    uint32_t col       = 1;

    while (i < len && src[i]) {
        unsigned char c = (unsigned char)src[i];

        if (c == '\n') {
            line++;
            col = 1;
            i++;
            continue;
        }

        if (isspace(c)) {
            i++;
            col++;
            continue;
        }

        if (c == '#') {
            size_t start = ++i;

            while (i < len && src[i] != '\n') {
                i++;
            }

            while (start < i && isspace((unsigned char)src[start])) {
                start++;
            }

            ncc_trivia_t *node = calloc(1, sizeof(*node));
            ncc_trivia_t *tail = pending;

            node->text = ncc_string_from_raw(src + start,
                                             (int64_t)(i - start));

            if (!tail) {
                pending = node;
            }
            else {
                while (tail->next) {
                    tail = tail->next;
                }

                tail->next = node;
            }

            continue;
        }

        size_t   start = i;
        uint32_t at_col = col;

        if (c == '"') {
            i++;

            while (i < len && src[i] != '"') {
                i += src[i] == '\\' && i + 1 < len ? 2 : 1;
            }

            if (i >= len) {
                return false;
            }

            i++;
            col += (uint32_t)(i - start);
            emit(out, tid_str, src + start + 1, i - start - 2, line, at_col,
                 start, i);
            continue;
        }

        if (isdigit(c)) {
            bool is_float = false;

            if (c == '0' && i + 1 < len && (src[i + 1] == 'x'
                                            || src[i + 1] == 'X')) {
                i += 2;

                while (i < len && isxdigit((unsigned char)src[i])) {
                    i++;
                }
            }
            else {
                while (i < len && isdigit((unsigned char)src[i])) {
                    i++;
                }

                if (i + 1 < len && src[i] == '.'
                    && isdigit((unsigned char)src[i + 1])) {
                    is_float = true;
                    i++;

                    while (i < len && isdigit((unsigned char)src[i])) {
                        i++;
                    }
                }

                if (i < len && (src[i] == 'e' || src[i] == 'E')) {
                    size_t save = i;

                    i++;

                    if (i < len && (src[i] == '+' || src[i] == '-')) {
                        i++;
                    }

                    if (i < len && isdigit((unsigned char)src[i])) {
                        is_float = true;

                        while (i < len && isdigit((unsigned char)src[i])) {
                            i++;
                        }
                    }
                    else {
                        i = save;
                    }
                }
            }

            col += (uint32_t)(i - start);
            emit(out, is_float ? tid_float : tid_int, src + start, i - start,
                 line, at_col, start, i);
            continue;
        }

        if (id_start(c)) {
            while (i < len && id_more((unsigned char)src[i])) {
                i++;
            }

            size_t  n  = i - start;
            int64_t kw = 0;

            bool after_sigil = out->n > 0
                            && (out->v[out->n - 1]->tid == '%'
                                || out->v[out->n - 1]->tid == '$'
                                || out->v[out->n - 1]->tid == '@');

            if (!after_sigil) {
                if (n == 1) {
                    unsigned char ch = (unsigned char)src[start];

                    kw = ch < 128 && char_terminals[ch] ? ch : 0;
                }
                else if (n < 128) {
                    char word[128];

                    memcpy(word, src + start, n);
                    word[n] = 0;
                    kw      = tid_of_name(g, word);
                }
            }

            col += (uint32_t)n;
            emit(out, kw ? kw : NCC_TOK_IDENTIFIER, src + start, n, line,
                 at_col, start, i);
            continue;
        }

        if (i + 1 < len) {
            char    two[3] = {src[i], src[i + 1], 0};
            int64_t op     = tid_of_name(g, two);

            if (op && op > 0xFF) {
                i += 2;
                col += 2;
                emit(out, op, src + start, 2, line, at_col, start, i);
                continue;
            }
        }

        i++;
        col++;
        emit(out, c, src + start, 1, line, at_col, start, i);
    }

    return true;
}
