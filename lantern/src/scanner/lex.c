#include "scanner/lex.h"
#include "scanner/tokenizer.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include "lib/alloc.h"
#include "lib/list.h"
#include "lib/dict.h"

#include <string.h>

typedef struct {
    const scan_lex_string_t *str;
    int32_t                 depth;
    bool                    in_hole;
    bool                    at_open;
} lex_mode_t;

core_list_decl(lex_mode_t);
core_list_decl(int32_t);

struct scan_lex_t {
    const scan_lex_spec_t *spec;
    parse_grammar_t        *g;

    uint8_t opwidth[256];

    int32_t tid_newline;
    int32_t tid_indent;
    int32_t tid_dedent;
    int32_t tid_eof;
    int32_t tid_ident;
    int32_t tid_ident_upper;
    int32_t tid_terminator;

    core_dict_t *is_literal;
    core_list_t(core_string_t) term_after;
    core_list_t(lex_mode_t) modes;
    core_list_t(int32_t) indents;
    int32_t pending_dedents;

    int32_t bracket_depth;
    bool    line_start;
    bool    line_has_token;
    bool    sent_eof;
    bool    outside;

    const scan_lex_directive_t *in_directive;

    size_t skip_from;
    size_t skip_to;
};

static void
mark_literal(scan_lex_t *lex, int32_t tid)
{
    if (tid == PARSE_TOK_IGNORED) {
        return;
    }

    if (!lex->is_literal) {
        lex->is_literal = core_alloc(core_dict_t);
        core_dict_init(lex->is_literal, core_hash_word, nullptr);
    }

    core_dict_put(lex->is_literal, (void *)(intptr_t)tid, (void *)(intptr_t)1);
}

static bool
is_literal_tid(const scan_lex_t *lex, int32_t tid)
{
    if (!lex->is_literal) {
        return false;
    }

    bool found = false;

    core_dict_get(lex->is_literal, (void *)(intptr_t)tid, &found);

    return found;
}

static int32_t
tid_for(parse_grammar_t *g, core_string_t name, int32_t fallback)
{
    if (!name.data || !name.u8_bytes) {
        return fallback;
    }

    int64_t id = parse_terminal_id(g, name);

    return id ? (int32_t)id : fallback;
}

scan_lex_t *
scan_lex_compile(const scan_lex_spec_t *spec, parse_grammar_t *g)
{
    if (!spec || !g) {
        return nullptr;
    }

    scan_lex_t *lex = core_alloc(scan_lex_t);

    lex->spec = spec;
    lex->g    = g;

    for (size_t i = 0; i < core_list_len(g->named_terms); i++) {
        core_string_t v = g->named_terms.data[i].value;

        if (!v.data || !v.u8_bytes || v.u8_bytes > 255) {
            continue;
        }

        unsigned char first = (unsigned char)v.data[0];

        if (v.u8_bytes > lex->opwidth[first]) {
            lex->opwidth[first] = (uint8_t)v.u8_bytes;
        }
    }

    lex->tid_newline = tid_for(g, spec->tok_newline, PARSE_TOK_IGNORED);
    lex->tid_indent  = tid_for(g, spec->tok_indent, PARSE_TOK_IGNORED);
    lex->tid_dedent  = tid_for(g, spec->tok_dedent, PARSE_TOK_IGNORED);
    lex->tid_eof     = tid_for(g, spec->tok_eof, PARSE_TOK_IGNORED);
    lex->tid_ident     = tid_for(g, spec->tok_ident, PARSE_TOK_IDENTIFIER);
    lex->tid_terminator = tid_for(g, spec->tok_terminator, PARSE_TOK_IGNORED);
    lex->tid_ident_upper = tid_for(g, spec->tok_ident_upper,
                                   PARSE_TOK_IGNORED);

    mark_literal(lex, lex->tid_ident);
    mark_literal(lex, lex->tid_ident_upper);

    for (const scan_lex_string_t *st = spec->strings; st && st->opener.data;
         st++) {
        int32_t ids[] = {
            tid_for(g, st->tok, PARSE_TOK_IGNORED),
            tid_for(g, st->tok_end, PARSE_TOK_IGNORED),
        };

        for (size_t i = 0; i < 2; i++) {
            mark_literal(lex, ids[i]);
        }
    }

    for (const scan_lex_suffix_t *sf = spec->number.suffixes;
         sf && sf->suffix.data;
         sf++) {
        mark_literal(lex, tid_for(g, sf->tok, PARSE_TOK_IGNORED));
    }

    int32_t more[] = {
        tid_for(g, spec->number.tok_int, PARSE_TOK_IGNORED),
        tid_for(g, spec->number.tok_float, PARSE_TOK_IGNORED),
    };

    for (size_t i = 0; i < 2; i++) {
        mark_literal(lex, more[i]);
    }

    core_string_t after = spec->terminator_after;

    for (size_t at = 0; after.data && at < after.u8_bytes;) {
        size_t end = at;

        while (end < after.u8_bytes && after.data[end] != ' ') {
            end++;
        }

        if (end > at) {
            core_list_push(lex->term_after,
                           core_string_borrow(after.data + at, end - at));
        }

        at = end + 1;
    }

    lex->line_start = true;
    lex->outside    = spec->embeds && spec->embeds[0].open.data;

    return lex;
}


static bool
ends_a_statement(scan_lex_t *lex, parse_scanner_t *s)
{
    parse_token_info_t *prev = scan_prev_token(s);

    if (!prev) {
        return false;
    }

    if (is_literal_tid(lex, prev->tid)) {
        return true;
    }

    if (core_option_is_set(prev->value)) {
        core_string_t v = core_option_get(prev->value);

        if (v.u8_bytes == 1
            && (v.data[0] == ')' || v.data[0] == ']' || v.data[0] == '}')) {
            return true;
        }

        for (size_t i = 0; i < lex->term_after.len; i++) {
            if (core_string_eq(lex->term_after.data[i], v)) {
                return true;
            }
        }
    }

    return false;
}

void
scan_lex_free(scan_lex_t *lex)
{
    if (!lex) {
        return;
    }

    if (lex->is_literal) {
        core_dict_free(lex->is_literal);
        core_free(lex->is_literal);
    }

    core_list_free(lex->term_after);
    core_list_free(lex->modes);
    core_list_free(lex->indents);
    core_free(lex);
}

size_t
scan_lex_operator_width(const scan_lex_t *lex, char first)
{
    return lex ? lex->opwidth[(unsigned char)first] : 0;
}

void
scan_lex_reset_state(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    if (!lex) {
        return;
    }

    lex->modes.len       = 0;
    lex->indents.len     = 0;
    lex->pending_dedents = 0;
    lex->bracket_depth   = 0;
    lex->line_start      = true;
    lex->line_has_token  = false;
    lex->sent_eof        = false;
    lex->skip_from       = 0;
    lex->skip_to         = 0;
    lex->outside         = lex->spec->embeds
                        && lex->spec->embeds[0].open.data;
    lex->in_directive    = nullptr;
}

bool
scan_grammar_set_lex(parse_grammar_t *g, const scan_lex_spec_t *spec)
{
    if (!g) {
        return false;
    }

    if (g->lex_compiled) {
        scan_lex_free((scan_lex_t *)g->lex_compiled);
        g->lex_compiled = nullptr;
    }

    if (g->lex_spec_owned) {
        scan_lex_spec_free((const scan_lex_spec_t *)g->lex_spec);
        g->lex_spec_owned = false;
    }

    g->lex_spec     = spec;
    g->lex_resolved = true;

    return true;
}

const scan_lex_spec_t *
scan_grammar_lex(parse_grammar_t *g)
{
    if (!g) {
        return nullptr;
    }

    if (!g->lex_resolved) {
        const scan_lex_spec_t *built = scan_lex_spec_from_grammar(g);

        g->lex_resolved = true;

        if (built) {
            g->lex_spec       = built;
            g->lex_spec_owned = true;
        }
        else {
            g->lex_spec = scan_lex_for(scan_grammar_tokenizer(g));
        }
    }

    return (const scan_lex_spec_t *)g->lex_spec;
}

scan_lex_t *
scan_grammar_lex_compiled(parse_grammar_t *g)
{
    const scan_lex_spec_t *spec = scan_grammar_lex(g);

    if (!spec) {
        return nullptr;
    }

    if (!g->lex_compiled) {
        g->lex_compiled = scan_lex_compile(spec, g);
    }

    return (scan_lex_t *)g->lex_compiled;
}

static bool
at_str(parse_scanner_t *s, core_string_t lit)
{
    if (!lit.data || !lit.u8_bytes) {
        return false;
    }

    if (s->cursor + lit.u8_bytes > s->input_len) {
        return false;
    }

    return memcmp(s->input + s->cursor, lit.data, lit.u8_bytes) == 0;
}

static bool
in_set(core_string_t set, core_codepoint_t c)
{
    if (!set.data || c > 127) {
        return false;
    }

    for (size_t i = 0; i < set.u8_bytes; i++) {
        if ((unsigned char)set.data[i] == (unsigned char)c) {
            return true;
        }
    }

    return false;
}

static bool
id_start_here(const scan_lex_t *lex, core_codepoint_t c)
{
    return unicode_is_id_start(c)
        || in_set(lex->spec->ident_start_extra, c);
}

static bool
id_cont_here(const scan_lex_t *lex, core_codepoint_t c)
{
    return unicode_is_id_continue(c) || in_set(lex->spec->ident_extra, c);
}

static void
emit_named(parse_scanner_t *s, core_string_t name, int32_t fallback)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    scan_emit_marked(s, tid_for(lex->g, name, fallback));
}

static bool
scan_outside(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    scan_mark(s);

    const scan_lex_embed_t *hit = nullptr;

    while (!scan_at_eof(s) && !hit) {
        for (const scan_lex_embed_t *e = lex->spec->embeds; e->open.data; e++) {
            if (at_str(s, e->open)) {
                hit = e;

                break;
            }
        }

        if (!hit) {
            scan_advance(s);
        }
    }

    if (scan_mark_len(s) > 0) {
        emit_named(s,
                   hit ? hit->tok : lex->spec->embeds[0].tok,
                   PARSE_TOK_OTHER);

        return true;
    }

    if (!hit) {
        return false;
    }

    scan_mark(s);
    scan_advance_bytes(s, hit->open.u8_bytes);

    lex->outside        = false;
    lex->line_start     = true;
    lex->line_has_token = false;

    if (hit->open_tok.data) {
        emit_named(s, hit->open_tok, PARSE_TOK_OTHER);

        return true;
    }

    return false;
}

#define EMBED_NOT_HERE 0
#define EMBED_EMITTED  1
#define EMBED_CONSUMED 2

static int
at_embed_close(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    for (const scan_lex_embed_t *e = lex->spec->embeds;
         e && e->open.data;
         e++) {
        if (e->close.u8_bytes && at_str(s, e->close)) {
            scan_mark(s);
            scan_advance_bytes(s, e->close.u8_bytes);
            lex->outside = true;

            if (e->close_tok.data) {
                emit_named(s, e->close_tok, PARSE_TOK_OTHER);

                return EMBED_EMITTED;
            }

            return EMBED_CONSUMED;
        }
    }

    return EMBED_NOT_HERE;
}

typedef enum : uint8_t {
    BODY_CLOSED,
    BODY_HOLE,
    BODY_UNTERMINATED,
} body_stop;

static body_stop
scan_body(parse_scanner_t *s, const scan_lex_string_t *str)
{
    bool raw   = (str->flags & SCAN_LEX_STR_RAW) != 0;
    bool multi = (str->flags & SCAN_LEX_STR_MULTILINE) != 0;

    while (!scan_at_eof(s)) {
        if (!raw && str->escape.u8_bytes && at_str(s, str->escape)) {
            scan_advance_bytes(s, str->escape.u8_bytes);

            if (!scan_at_eof(s)) {
                scan_advance(s);
            }

            continue;
        }

        if (str->interp_open.u8_bytes && at_str(s, str->interp_open)) {
            if (!(str->flags & SCAN_LEX_STR_HOLE_TOKENS)) {
                scan_advance_bytes(s, str->interp_open.u8_bytes);
            }

            return BODY_HOLE;
        }

        if (at_str(s, str->closer)) {
            scan_advance_bytes(s, str->closer.u8_bytes);

            return BODY_CLOSED;
        }

        if (!multi && scan_peek(s, 0) == '\n') {
            return BODY_UNTERMINATED;
        }

        scan_advance(s);
    }

    return BODY_UNTERMINATED;
}

static bool
push_mode(scan_lex_t *lex, const scan_lex_string_t *str)
{
    core_list_push(lex->modes,
                   ((lex_mode_t){
                       .str     = str,
                       .depth   = 0,
                       .in_hole = true,
                       .at_open = (str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0,
                   }));

    return true;
}

static bool
try_string(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    const scan_lex_string_t *best = nullptr;

    for (const scan_lex_string_t *st = lex->spec->strings;
         st && st->opener.data;
         st++) {
        if (!at_str(s, st->opener)) {
            continue;
        }

        if (!best || st->opener.u8_bytes > best->opener.u8_bytes) {
            best = st;
        }
    }

    if (!best) {
        return false;
    }

    scan_mark(s);
    scan_advance_bytes(s, best->opener.u8_bytes);

    body_stop stop = scan_body(s, best);

    if (stop == BODY_HOLE) {
        emit_named(s, best->tok, PARSE_TOK_STRING_LIT);
        push_mode(lex, best);

        return true;
    }

    emit_named(s, best->tok, PARSE_TOK_STRING_LIT);

    return true;
}

static bool
resume_string(parse_scanner_t *s, bool mark)
{
    scan_lex_t  *lex = (scan_lex_t *)s->user_state;
    lex_mode_t *m   = &lex->modes.data[lex->modes.len - 1];

    if (mark) {
        scan_mark(s);
    }

    body_stop stop = scan_body(s, m->str);

    if (stop == BODY_HOLE) {
        emit_named(s, m->str->tok_mid, PARSE_TOK_STRING_LIT);
        m->in_hole = true;
        m->at_open = (m->str->flags & SCAN_LEX_STR_HOLE_TOKENS) != 0;

        return true;
    }

    emit_named(s, m->str->tok_end, PARSE_TOK_STRING_LIT);
    lex->modes.len--;

    return true;
}

static bool
try_comment(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    const scan_lex_comment_t *c = nullptr;

    for (const scan_lex_comment_t *k = lex->spec->comments;
         k && k->opener.data;
         k++) {
        if (!at_str(s, k->opener)) {
            continue;
        }

        if (!c || k->opener.u8_bytes > c->opener.u8_bytes) {
            c = k;
        }
    }

    if (c) {
        scan_mark(s);
        scan_advance_bytes(s, c->opener.u8_bytes);

        if (c->kind == SCAN_LEX_LINE) {
            while (!scan_at_eof(s) && scan_peek(s, 0) != '\n') {
                scan_advance(s);
            }
        }
        else {
            int32_t depth = 1;

            while (!scan_at_eof(s) && depth > 0) {
                if (c->nested && at_str(s, c->opener)) {
                    scan_advance_bytes(s, c->opener.u8_bytes);
                    depth++;

                    continue;
                }

                if (at_str(s, c->closer)) {
                    scan_advance_bytes(s, c->closer.u8_bytes);
                    depth--;

                    continue;
                }

                scan_advance(s);
            }
        }

        if (c->tok.data && c->tok.u8_bytes) {
            emit_named(s, c->tok, PARSE_TOK_OTHER);
        }
        else {
            scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);
        }

        return true;
    }

    return false;
}

static bool
try_number(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    const scan_lex_number_t *n = &lex->spec->number;

    core_codepoint_t c    = scan_peek(s, 0);
    int32_t         lead = 0;

    if (in_set(n->sign, c) && unicode_cp_is_ascii_digit(scan_peek(s, 1))) {
        lead = 1;
        c    = scan_peek(s, 1);
    }

    if (!unicode_cp_is_ascii_digit(c)
        && !(c == '.' && unicode_cp_is_ascii_digit(scan_peek(s, lead + 1)))) {
        return false;
    }

    scan_mark(s);

    if (lead) {
        scan_advance(s);
    }

    for (const scan_lex_radix_t *r = n->radixes; r && r->prefix.data; r++) {
        if (!at_str(s, r->prefix)) {
            continue;
        }

        scan_advance_bytes(s, r->prefix.u8_bytes);

        while (!scan_at_eof(s)) {
            core_codepoint_t k = scan_peek(s, 0);

            if (in_set(r->digits, k) || in_set(n->sep, k)) {
                scan_advance(s);

                continue;
            }

            break;
        }

        for (const scan_lex_suffix_t *sf = n->suffixes;
             sf && sf->suffix.data;
             sf++) {
            if (at_str(s, sf->suffix)) {
                scan_advance_bytes(s, sf->suffix.u8_bytes);
                emit_named(s, sf->tok, PARSE_TOK_INTEGER);

                return true;
            }
        }

        emit_named(s, r->tok.data ? r->tok : n->tok_int, PARSE_TOK_INTEGER);

        return true;
    }

    bool is_float = false;

    while (!scan_at_eof(s)) {
        core_codepoint_t k = scan_peek(s, 0);

        if (unicode_cp_is_ascii_digit(k) || in_set(n->sep, k)) {
            scan_advance(s);

            continue;
        }

        if (k == '.' && !is_float && unicode_cp_is_ascii_digit(scan_peek(s, 1))) {
            is_float = true;
            scan_advance(s);

            continue;
        }

        if (in_set(n->exponent, k)) {
            core_codepoint_t sign = scan_peek(s, 1);
            int32_t         at   = (sign == '+' || sign == '-') ? 2 : 1;

            if (!unicode_cp_is_ascii_digit(scan_peek(s, at))) {
                break;
            }

            is_float = true;
            scan_advance_n(s, at);

            continue;
        }

        break;
    }

    for (const scan_lex_suffix_t *sf = n->suffixes; sf && sf->suffix.data;
         sf++) {
        if (at_str(s, sf->suffix)) {
            scan_advance_bytes(s, sf->suffix.u8_bytes);
            emit_named(s, sf->tok, is_float ? PARSE_TOK_FLOAT : PARSE_TOK_INTEGER);

            return true;
        }
    }

    emit_named(s,
               is_float ? n->tok_float : n->tok_int,
               is_float ? PARSE_TOK_FLOAT : PARSE_TOK_INTEGER);

    return true;
}

static bool
try_sigil(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    const scan_lex_sigil_t *best = nullptr;

    for (const scan_lex_sigil_t *g = lex->spec->sigils; g && g->prefix.data;
         g++) {
        if (!at_str(s, g->prefix)) {
            continue;
        }

        core_codepoint_t after = scan_peek(s, (int32_t)g->prefix.u8_bytes);

        bool ok = g->digits_only ? unicode_cp_is_ascii_digit(after)
                                 : unicode_is_id_start(after);

        if (!ok) {
            continue;
        }

        if (!best || g->prefix.u8_bytes > best->prefix.u8_bytes) {
            best = g;
        }
    }

    if (!best) {
        return false;
    }

    scan_mark(s);
    scan_advance_bytes(s, best->prefix.u8_bytes);

    if (best->digits_only) {
        while (unicode_cp_is_ascii_digit(scan_peek(s, 0))) {
            scan_advance(s);
        }
    }
    else {
        while (!scan_at_eof(s)
               && unicode_is_id_continue(scan_peek(s, 0))) {
            scan_advance(s);
        }
    }

    emit_named(s, best->tok, lex->tid_ident);

    return true;
}

static bool
regex_may_start(parse_scanner_t *s)
{
    parse_token_info_t *prev = scan_prev_token(s);

    if (!prev) {
        return true;
    }

    if (prev->tid == PARSE_TOK_IDENTIFIER || prev->tid == PARSE_TOK_INTEGER
        || prev->tid == PARSE_TOK_FLOAT || prev->tid == PARSE_TOK_STRING_LIT) {
        return false;
    }

    if (core_option_is_set(prev->value)) {
        core_string_t v = core_option_get(prev->value);

        if (v.u8_bytes == 1
            && (v.data[0] == ')' || v.data[0] == ']' || v.data[0] == '}')) {
            return false;
        }

        if (v.u8_bytes
            && unicode_is_id_continue((core_codepoint_t)(unsigned char)
                                              v.data[v.u8_bytes - 1])) {
            return false;
        }
    }

    return true;
}

static bool
try_regex(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    for (const scan_lex_regex_t *r = lex->spec->regexes; r && r->opener.data;
         r++) {
        if (!at_str(s, r->opener) || !regex_may_start(s)) {
            continue;
        }

        size_t save = s->cursor;

        scan_mark(s);
        scan_advance_bytes(s, r->opener.u8_bytes);

        bool in_class = false;
        bool closed   = false;

        while (!scan_at_eof(s)) {
            core_codepoint_t k = scan_peek(s, 0);

            if (k == '\n') {
                break;
            }

            if (k == '\\') {
                scan_advance_n(s, 2);

                continue;
            }

            if (k == '[') {
                in_class = true;
                scan_advance(s);

                continue;
            }

            if (k == ']') {
                in_class = false;
                scan_advance(s);

                continue;
            }

            if (!in_class && at_str(s, r->closer)) {
                scan_advance_bytes(s, r->closer.u8_bytes);
                closed = true;

                break;
            }

            scan_advance(s);
        }

        if (!closed) {
            s->cursor = save;

            continue;
        }

        while (!scan_at_eof(s)
               && in_set(r->suffix_chars, scan_peek(s, 0))) {
            scan_advance(s);
        }

        emit_named(s, r->tok, PARSE_TOK_STRING_LIT);

        return true;
    }

    return false;
}

static bool
try_heredoc(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    for (const scan_lex_heredoc_t *h = lex->spec->heredocs;
         h && h->opener.data;
         h++) {
        if (!at_str(s, h->opener)) {
            continue;
        }

        size_t after = s->cursor + h->opener.u8_bytes;
        size_t tag_a = after;

        while (tag_a < s->input_len
               && (s->input[tag_a] == '"' || s->input[tag_a] == '\'')) {
            tag_a++;
        }

        size_t tag_b = tag_a;

        while (tag_b < s->input_len
               && unicode_is_id_continue(
                   (core_codepoint_t)(unsigned char)s->input[tag_b])) {
            tag_b++;
        }

        if (tag_b == tag_a) {
            continue;
        }

        scan_mark(s);
        scan_advance_bytes(s, tag_b - s->cursor);

        while (!scan_at_eof(s)
               && (s->input[s->cursor] == '"' || s->input[s->cursor] == '\'')) {
            scan_advance(s);
        }

        size_t eol = s->cursor;

        while (eol < s->input_len && s->input[eol] != '\n') {
            eol++;
        }

        size_t body = eol < s->input_len ? eol + 1 : eol;
        size_t scan = body;
        size_t end  = s->input_len;

        while (scan < s->input_len) {
            size_t ls = scan;
            size_t le = ls;

            while (le < s->input_len && s->input[le] != '\n') {
                le++;
            }

            size_t t = ls;

            if (h->indent_strip) {
                while (t < le
                       && (s->input[t] == ' ' || s->input[t] == '\t')) {
                    t++;
                }
            }

            if ((size_t)(le - t) == tag_b - tag_a
                && !memcmp(s->input + t, s->input + tag_a, tag_b - tag_a)) {
                end = le < s->input_len ? le + 1 : le;

                break;
            }

            scan = le < s->input_len ? le + 1 : le;
        }

        lex->skip_from = body;
        lex->skip_to   = end;

        emit_named(s, h->tok, PARSE_TOK_STRING_LIT);

        return true;
    }

    return false;
}

static bool
try_terminal_min(parse_scanner_t *s, size_t min_len)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    unsigned char first = (unsigned char)s->input[s->cursor];
    size_t        width = lex->opwidth[first];

    if (s->cursor + width > s->input_len) {
        width = s->input_len - s->cursor;
    }

    for (size_t len = width; len >= min_len; len--) {
        core_string_t slice = core_string_borrow(s->input + s->cursor, len);
        int64_t      id    = parse_lookup_terminal(lex->g, slice);

        if (!id) {
            continue;
        }

        unsigned char last = (unsigned char)slice.data[len - 1];

        if (id_cont_here(lex, (core_codepoint_t)last)) {
            core_codepoint_t next = scan_peek(s, (int32_t)len);

            if (next > 0 && id_cont_here(lex, next)) {
                continue;
            }
        }

        scan_mark(s);
        scan_advance_bytes(s, len);
        scan_emit_marked(s, (int32_t)id);

        return true;
    }

    return false;
}

static bool
try_terminal(parse_scanner_t *s)
{
    return try_terminal_min(s, 1);
}

static bool
try_identifier(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    if (!id_start_here(lex, scan_peek(s, 0))) {
        return false;
    }

    core_codepoint_t first = scan_peek(s, 0);

    scan_mark(s);

    while (!scan_at_eof(s) && id_cont_here(lex, scan_peek(s, 0))) {
        scan_advance(s);
    }

    bool upper = lex->tid_ident_upper != PARSE_TOK_IGNORED
              && first >= 'A' && first <= 'Z';

    scan_emit_marked(s, upper ? lex->tid_ident_upper : lex->tid_ident);

    return true;
}

static void
track_bracket(scan_lex_t *lex, core_codepoint_t c)
{
    if (!lex->spec->brackets.data) {
        return;
    }

    core_string_t b = lex->spec->brackets;

    for (size_t i = 0; i + 1 < b.u8_bytes; i += 2) {
        if ((core_codepoint_t)(unsigned char)b.data[i] == c) {
            lex->bracket_depth++;

            return;
        }

        if ((core_codepoint_t)(unsigned char)b.data[i + 1] == c) {
            if (lex->bracket_depth > 0) {
                lex->bracket_depth--;
            }

            return;
        }
    }
}

static int32_t
measure_indent(parse_scanner_t *s)
{
    int32_t col = 0;
    size_t  at  = s->cursor;

    while (at < s->input_len) {
        if (s->input[at] == ' ') {
            col++;
        }
        else if (s->input[at] == '\t') {
            col += 8 - (col % 8);
        }
        else {
            break;
        }

        at++;
    }

    return col;
}

static bool
blank_or_comment_line(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;
    size_t     at  = s->cursor;

    while (at < s->input_len
           && (s->input[at] == ' ' || s->input[at] == '\t')) {
        at++;
    }

    if (at >= s->input_len || s->input[at] == '\n' || s->input[at] == '\r') {
        return true;
    }

    for (const scan_lex_comment_t *c = lex->spec->comments;
         c && c->opener.data;
         c++) {
        if (c->kind != SCAN_LEX_LINE || c->tok.data) {
            continue;
        }

        if (at + c->opener.u8_bytes <= s->input_len
            && !memcmp(s->input + at, c->opener.data, c->opener.u8_bytes)) {
            return true;
        }
    }

    return false;
}

static bool
do_layout(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    if (!(lex->spec->layout & SCAN_LEX_LAYOUT_INDENT) || !lex->line_start) {
        return false;
    }

    if (lex->bracket_depth > 0 || blank_or_comment_line(s)) {
        return false;
    }

    int32_t col = measure_indent(s);
    int32_t cur = lex->indents.len
                    ? lex->indents.data[lex->indents.len - 1]
                    : 0;

    if (col > cur) {
        core_list_push(lex->indents, col);

        scan_mark(s);
        scan_emit_marked(s, lex->tid_indent);
        lex->line_start = false;

        return true;
    }

    if (col < cur) {
        while (lex->indents.len
               && lex->indents.data[lex->indents.len - 1] > col) {
            lex->indents.len--;
            lex->pending_dedents++;
        }

        lex->line_start = false;

        if (lex->pending_dedents > 0) {
            lex->pending_dedents--;
            scan_mark(s);
            scan_emit_marked(s, lex->tid_dedent);

            return true;
        }

        return false;
    }

    lex->line_start = false;

    return false;
}

bool
scan_lex_scan(parse_scanner_t *s)
{
    scan_lex_t *lex = (scan_lex_t *)s->user_state;

    if (!lex) {
        return false;
    }

    if (lex->pending_dedents > 0) {
        lex->pending_dedents--;
        lex->line_start = false;
        scan_mark(s);
        scan_emit_marked(s, lex->tid_dedent);

        return true;
    }

    if (lex->outside) {
        if (scan_outside(s)) {
            return true;
        }

        if (scan_at_eof(s)) {
            return false;
        }
    }

    if (lex->modes.len && !lex->modes.data[lex->modes.len - 1].in_hole) {
        return resume_string(s, true);
    }

    if (do_layout(s)) {
        return true;
    }

    for (;;) {
        if (lex->skip_to > lex->skip_from && s->cursor >= lex->skip_from) {
            scan_advance_bytes(s, lex->skip_to - s->cursor);
            lex->skip_from = 0;
            lex->skip_to   = 0;
            lex->line_start = true;

            continue;
        }

        if (scan_at_eof(s)) {
            break;
        }

        core_codepoint_t c = scan_peek(s, 0);

        if (c == '\n') {
            bool significant = (lex->spec->layout & (SCAN_LEX_LAYOUT_NEWLINE
                                                     | SCAN_LEX_LAYOUT_INDENT))
                            && lex->bracket_depth == 0
                            && lex->line_has_token;

            if (lex->in_directive) {
                core_string_t eol = lex->in_directive->tok_eol;

                lex->in_directive   = nullptr;
                lex->line_start     = true;
                lex->line_has_token = false;

                scan_advance(s);
                scan_mark(s);
                emit_named(s, eol, PARSE_TOK_IGNORED);

                return true;
            }

            bool insert = lex->tid_terminator != PARSE_TOK_IGNORED
                       && lex->bracket_depth == 0
                       && lex->line_has_token
                       && ends_a_statement(lex, s);

            scan_mark(s);
            scan_advance(s);
            lex->line_start     = true;
            lex->line_has_token = false;

            if (insert) {
                scan_mark(s);
                scan_emit_marked(s, lex->tid_terminator);

                return true;
            }

            if (significant) {
                scan_mark(s);
                scan_emit_marked(s, lex->tid_newline);

                return true;
            }

            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            scan_advance(s);

            continue;
        }

        if (lex->spec->line_continue.data
            && at_str(s, lex->spec->line_continue)) {
            size_t at = s->cursor + lex->spec->line_continue.u8_bytes;

            while (at < s->input_len
                   && (s->input[at] == ' ' || s->input[at] == '\t')) {
                at++;
            }

            if (at < s->input_len && s->input[at] == '\n') {
                scan_advance_bytes(s, at + 1 - s->cursor);

                continue;
            }
        }

        if (lex->spec->embeds) {
            int what = at_embed_close(s);

            if (what == EMBED_EMITTED) {
                return true;
            }

            if (what == EMBED_CONSUMED) {
                return scan_lex_scan(s);
            }
        }

        if (try_comment(s)) {
            if (s->pending_leading || s->pending_trailing) {
                continue;
            }

            return true;
        }

        break;
    }

    if (scan_at_eof(s)) {
        if (lex->tid_terminator != PARSE_TOK_IGNORED && lex->line_has_token
            && lex->bracket_depth == 0 && ends_a_statement(lex, s)) {
            lex->line_has_token = false;
            scan_mark(s);
            scan_emit_marked(s, lex->tid_terminator);

            return true;
        }

        if (lex->tid_newline != PARSE_TOK_IGNORED && lex->line_has_token
            && lex->bracket_depth == 0) {
            lex->line_has_token = false;
            scan_mark(s);
            scan_emit_marked(s, lex->tid_newline);

            return true;
        }

        while (lex->indents.len) {
            lex->indents.len--;
            lex->pending_dedents++;
        }

        if (lex->pending_dedents > 0) {
            lex->pending_dedents--;
            scan_mark(s);
            scan_emit_marked(s, lex->tid_dedent);

            return true;
        }

        if (!lex->sent_eof && lex->tid_eof != PARSE_TOK_IGNORED) {
            lex->sent_eof = true;
            scan_mark(s);
            scan_emit_marked(s, lex->tid_eof);

            return true;
        }

        return false;
    }

    bool at_line_start = lex->line_start;

    lex->line_start     = false;
    lex->line_has_token = true;

    const scan_lex_directive_t *dir = nullptr;

    for (const scan_lex_directive_t *d = lex->spec->directives;
         at_line_start && d && d->open.data;
         d++) {
        if (at_str(s, d->open)) {
            dir = d;

            break;
        }
    }

    if (dir) {
        if (dir->tok_other.data
            && try_terminal_min(s, dir->open.u8_bytes + 1)) {
            lex->in_directive = dir;

            return true;
        }

        scan_mark(s);

        while (!scan_at_eof(s)) {
            core_codepoint_t k = scan_peek(s, 0);

            if (k == '\\') {
                int32_t at = 1;

                while (scan_peek(s, at) == ' '
                       || scan_peek(s, at) == '\t'
                       || scan_peek(s, at) == '\r') {
                    at++;
                }

                if (scan_peek(s, at) == '\n') {
                    scan_advance_n(s, at + 1);

                    continue;
                }
            }

            if (k == '\n') {
                break;
            }

            scan_advance(s);
        }

        if (!dir->tok_other.data) {
            scan_add_trivia(s, scan_extract(s), PARSE_TRIVIA_COMMENT);

            return scan_lex_scan(s);
        }

        emit_named(s, dir->tok_other, PARSE_TOK_OTHER);

        return true;
    }

    if (try_heredoc(s)) {
        return true;
    }

    if (try_string(s)) {
        return true;
    }

    if (try_number(s)) {
        return true;
    }

    if (try_sigil(s)) {
        return true;
    }

    if (try_regex(s)) {
        return true;
    }

    core_codepoint_t c = scan_peek(s, 0);

    if (lex->modes.len && lex->modes.data[lex->modes.len - 1].in_hole) {
        lex_mode_t *m = &lex->modes.data[lex->modes.len - 1];

        core_string_t open = m->str->interp_open;
        core_codepoint_t nest = open.u8_bytes
                                 ? (core_codepoint_t)(unsigned char)
                                       open.data[open.u8_bytes - 1]
                                 : 0;

        if (m->at_open && at_str(s, open)) {
            m->at_open = false;

            goto dispatch;
        }

        if (at_str(s, m->str->interp_close)) {
            if (m->depth == 0) {
                m->in_hole = false;

                if (m->str->flags & SCAN_LEX_STR_HOLE_TOKENS) {
                    goto dispatch;
                }

                scan_mark(s);
                scan_advance_bytes(s, m->str->interp_close.u8_bytes);

                return resume_string(s, false);
            }

            m->depth--;
        }
        else if (nest && c == nest) {
            m->depth++;
        }
    }

dispatch:
    track_bracket(lex, c);

    if (try_terminal(s)) {
        return true;
    }

    if (try_identifier(s)) {
        return true;
    }

    scan_mark(s);
    scan_advance(s);
    scan_emit_marked(s, (int32_t)c);

    return true;
}
