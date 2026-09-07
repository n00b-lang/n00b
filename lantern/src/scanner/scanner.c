
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include <assert.h>
#include <string.h>

static core_codepoint_t
decode_at(parse_scanner_t *s, size_t off, int *cp_len)
{
    if (off >= s->input_len) {
        *cp_len = 0;
        return 0;
    }

    uint32_t pos = (uint32_t)off;
    int32_t  cp  = unicode_utf8_decode(s->input, (uint32_t)s->input_len, &pos);

    if (cp < 0) {
        *cp_len = 1;
        return (core_codepoint_t)(uint8_t)s->input[off];
    }

    *cp_len = (int)(pos - (uint32_t)off);
    return (core_codepoint_t)cp;
}

static bool
cc_match(parse_char_class_t cc, core_codepoint_t cp)
{
    switch (cc) {
    case PARSE_CC_ID_START:
        return unicode_is_id_start(cp);
    case PARSE_CC_ID_CONTINUE:
        return unicode_is_id_continue(cp);
    case PARSE_CC_ASCII_DIGIT:
        return cp >= '0' && cp <= '9';
    case PARSE_CC_UNICODE_DIGIT: {
        unicode_gc_t gc = unicode_general_category(cp);
        return gc == UNICODE_GC_ND;
    }
    case PARSE_CC_ASCII_UPPER:
        return cp >= 'A' && cp <= 'Z';
    case PARSE_CC_ASCII_LOWER:
        return cp >= 'a' && cp <= 'z';
    case PARSE_CC_ASCII_ALPHA:
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    case PARSE_CC_WHITESPACE:
        return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r'
            || cp == '\f' || cp == '\v'
            || cp == 0x85
            || cp == 0xA0
            || cp == 0x2028
            || cp == 0x2029;
    case PARSE_CC_HEX_DIGIT:
        return (cp >= '0' && cp <= '9')
            || (cp >= 'a' && cp <= 'f')
            || (cp >= 'A' && cp <= 'F');
    case PARSE_CC_NONZERO_DIGIT:
        return cp >= '1' && cp <= '9';
    case PARSE_CC_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F;
    case PARSE_CC_NON_WS_PRINTABLE:
        return cp > 0x20 && cp != 0x7F;
    case PARSE_CC_NON_NL_WS:
        return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\f' || cp == '\v';
    case PARSE_CC_NON_NL_PRINTABLE:
        return cp >= 0x20 && cp != 0x7F && cp != '\n';
    case PARSE_CC_JSON_STRING_CHAR:
        return cp >= 0x20 && cp != '"' && cp != '\\';
    case PARSE_CC_REGEX_BODY_CHAR:
        return cp >= 0x20 && cp != '/' && cp != '\\';
    }
    return false;
}

static parse_trivia_t *
make_trivia(core_string_t text)
{
    parse_trivia_t *t = core_alloc(parse_trivia_t);

    t->text = text;
    t->next = nullptr;
    return t;
}

static void
append_trivia(parse_trivia_t **head, parse_trivia_t *node)
{
    if (!*head) {
        *head = node;
        return;
    }

    parse_trivia_t *tail = *head;

    while (tail->next) {
        tail = tail->next;
    }

    tail->next = node;
}

parse_scanner_t *
scan_scanner_new(core_buffer_t                *buf,
                 parse_scan_cb_t                cb,
                 parse_grammar_t               *grammar,
                 core_option_t(core_string_t)  file,
                 void                         *state,
                 scan_reset_cb_t          reset_cb)
{
    assert(buf);
    assert(cb);

    parse_scanner_t *s = core_alloc(parse_scanner_t);

    s->input     = buf->data;
    s->input_len = buf->byte_len;
    s->cursor    = 0;
    s->line      = 1;
    s->column    = 1;
    s->file      = file;
    s->cb        = cb;
    s->reset_cb  = reset_cb;
    s->user_state = state;
    s->grammar   = grammar;
    s->at_eof    = false;

    s->mark      = 0;
    s->mark_line = 1;
    s->mark_col  = 1;


    return s;
}

void
scan_scanner_free(parse_scanner_t *s)
{
    if (!s) {
        return;
    }

    parse_trivia_free(s->pending_leading);
    parse_trivia_free(s->pending_trailing);

    core_free(s);
}

void
scan_scanner_reset(parse_scanner_t *s)
{
    if (!s) {
        return;
    }

    s->cursor   = 0;
    s->line     = 1;
    s->column   = 1;
    s->mark     = 0;
    s->mark_line = 1;
    s->mark_col  = 1;
    s->at_eof   = false;

    parse_trivia_free(s->pending_leading);
    parse_trivia_free(s->pending_trailing);
    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    if (s->reset_cb) {
        s->reset_cb(s);
    }
}

core_codepoint_t
scan_peek(parse_scanner_t *s, int32_t offset)
{
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;
    int    cp_len;

    return decode_at(s, pos, &cp_len);
}

uint8_t
scan_peek_byte(parse_scanner_t *s, int32_t offset)
{
    if (offset < 0 && (size_t)(-offset) > s->cursor) {
        return 0;
    }

    size_t pos = s->cursor + (size_t)offset;

    if (pos >= s->input_len) {
        return 0;
    }

    return (uint8_t)s->input[pos];
}

bool
scan_at_eof(parse_scanner_t *s)
{
    return s->cursor >= s->input_len;
}

size_t
scan_remaining(parse_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    return s->input_len - s->cursor;
}

size_t
scan_offset(parse_scanner_t *s)
{
    return s->cursor;
}

void
scan_advance(parse_scanner_t *s)
{
    if (s->cursor >= s->input_len) {
        return;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }
}

void
scan_advance_n(parse_scanner_t *s, int32_t n)
{
    for (int32_t i = 0; i < n && s->cursor < s->input_len; i++) {
        scan_advance(s);
    }
}

void
scan_advance_bytes(parse_scanner_t *s, size_t n)
{
    size_t end = s->cursor + n;

    if (end > s->input_len) {
        end = s->input_len;
    }

    size_t last_nl = 0;
    uint32_t lines = 0;

    for (size_t i = s->cursor; i < end; i++) {
        if (s->input[i] == '\n') {
            lines++;
            last_nl = i + 1;
        }
    }

    if (lines) {
        s->line += lines;
        s->column = (uint32_t)(end - last_nl) + 1;
    }
    else {
        s->column += (uint32_t)(end - s->cursor);
    }

    s->cursor = end;
}

size_t
scan_match_str(parse_scanner_t *s, core_string_t lit)
{
    size_t len = lit.u8_bytes;

    if (s->cursor + len > s->input_len) {
        return 0;
    }

    if (memcmp(s->input + s->cursor, lit.data, len) != 0) {
        return 0;
    }

    for (size_t i = 0; i < len; ) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    return len;
}

size_t
scan_match_if(parse_scanner_t *s, scan_cp_predicate_fn pred)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!pred(cp, nullptr)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
scan_match_class(parse_scanner_t *s, parse_char_class_t cc)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!cc_match(cc, cp)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

size_t
scan_match_cp(parse_scanner_t *s, core_codepoint_t target)
{
    if (s->cursor >= s->input_len) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (cp != target) {
        return 0;
    }

    s->cursor += (size_t)cp_len;

    if (cp == '\n') {
        s->line++;
        s->column = 1;
    }
    else {
        s->column++;
    }

    return (size_t)cp_len;
}

int32_t
scan_skip_while(parse_scanner_t *s, scan_cp_predicate_fn pred)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!pred(cp, nullptr)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

int32_t
scan_skip_class(parse_scanner_t *s, parse_char_class_t cc)
{
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(cc, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    return count;
}

void
scan_mark(parse_scanner_t *s)
{
    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

core_string_t
scan_extract(parse_scanner_t *s)
{
    size_t len = s->cursor - s->mark;

    return core_string_from_raw(s->input + s->mark, (int64_t)len);
}

size_t
scan_mark_len(parse_scanner_t *s)
{
    return s->cursor - s->mark;
}

void
scan_emit(parse_scanner_t *s, int32_t tid, core_option_t(core_string_t) value)
{
    assert(s->stream);

    parse_token_stream_t *ts = s->stream;

    if (ts->token_count >= ts->token_cap) {
        int32_t new_cap = ts->token_cap ? ts->token_cap * 2 : 128;
        parse_token_info_t **new_tokens = core_alloc_array(parse_token_info_t *, new_cap);

        if (ts->tokens && ts->token_count > 0) {
            memcpy(new_tokens, ts->tokens,
                   (size_t)ts->token_count * sizeof(parse_token_info_t *));
        }

        core_free(ts->tokens);
        ts->tokens    = new_tokens;
        ts->token_cap = new_cap;
    }

    parse_token_info_t *tok = core_alloc(parse_token_info_t);
    memset(tok, 0, sizeof(*tok));

    tok->tid             = tid;
    tok->value           = value;
    tok->file            = s->file;
    tok->line            = s->mark_line;
    tok->column          = s->mark_col;
    tok->endcol          = s->column;
    tok->start_byte      = s->mark;
    tok->end_byte        = s->cursor;
    tok->index           = ts->next_index++;
    tok->leading_trivia  = s->pending_leading;
    tok->trailing_trivia = s->pending_trailing;

    s->pending_leading  = nullptr;
    s->pending_trailing = nullptr;

    ts->tokens[ts->token_count++] = tok;

    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

void
scan_emit_marked(parse_scanner_t *s, int32_t tid)
{
    core_string_t val = scan_extract(s);

    scan_emit(s, tid, core_option_set(core_string_t, val));
}

void
scan_add_leading_trivia_kind(parse_scanner_t    *s,
                                 core_string_t      text,
                                 parse_trivia_kind_t kind)
{
    parse_trivia_t *t = make_trivia(text);

    t->kind = kind;

    append_trivia(&s->pending_leading, t);
}


void
scan_add_leading_trivia(parse_scanner_t *s, core_string_t text)
{
    scan_add_leading_trivia_kind(s, text, PARSE_TRIVIA_COMMENT);
}

void
scan_set_trivia_attach(parse_scanner_t *s, scan_trivia_attach_t how)
{
    if (s) {
        s->trivia_attach = (uint8_t)how;
    }
}

scan_trivia_attach_t
scan_trivia_attach(const parse_scanner_t *s)
{
    return s ? (scan_trivia_attach_t)s->trivia_attach
             : SCAN_TRIVIA_ATTACH_RIGHT;
}

void
scan_add_trivia(parse_scanner_t *s, core_string_t text,
                    parse_trivia_kind_t kind)
{
    if (!s) {
        return;
    }

    if (s->trivia_attach == (uint8_t)SCAN_TRIVIA_ATTACH_LEFT) {
        parse_token_info_t *prev = scan_prev_token(s);
        parse_trivia_t     *t    = make_trivia(text);

        t->kind = kind;

        if (prev && !prev->no_trivia) {
            append_trivia(&prev->trailing_trivia, t);
        }
        else if (!prev) {
            append_trivia(&s->pending_leading, t);
        }

        return;
    }

    scan_add_leading_trivia_kind(s, text, kind);
}

void
scan_add_trailing_trivia(parse_scanner_t *s, core_string_t text)
{
    parse_trivia_t *t = make_trivia(text);

    t->kind = PARSE_TRIVIA_COMMENT;

    append_trivia(&s->pending_trailing, t);
}

int32_t
scan_skip_whitespace(parse_scanner_t *s)
{
    size_t  start = s->cursor;
    int32_t count = 0;

    while (s->cursor < s->input_len) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(PARSE_CC_WHITESPACE, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }

        count++;
    }

    if (count > 0) {
        size_t byte_len = s->cursor - start;

        core_string_t text = core_string_from_raw(s->input + start,
                                                   (int64_t)byte_len);
        scan_add_leading_trivia_kind(s, text, PARSE_TRIVIA_SPACE);
    }

    return count;
}

void
scan_skip_line_comment(parse_scanner_t *s)
{
    size_t start = s->cursor;

    while (s->cursor < s->input_len) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (cp == '\n') {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->column++;
    }

    size_t byte_len = s->cursor - start;

    if (byte_len > 0) {
        core_string_t text = core_string_from_raw(s->input + start,
                                                   (int64_t)byte_len);
        scan_add_trivia(s, text, PARSE_TRIVIA_COMMENT);
    }
}

bool
scan_skip_block_comment(parse_scanner_t *s,
                            core_string_t opener,
                            core_string_t closer)
{
    size_t opener_len = opener.u8_bytes;
    size_t closer_len = closer.u8_bytes;
    size_t start      = s->cursor;

    if (s->cursor + opener_len > s->input_len
        || memcmp(s->input + s->cursor, opener.data, opener_len) != 0) {
        return false;
    }

    for (size_t i = 0; i < opener_len; ) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    while (s->cursor + closer_len <= s->input_len) {
        if (memcmp(s->input + s->cursor, closer.data, closer_len) == 0) {
            for (size_t i = 0; i < closer_len; ) {
                int              cp_len;
                core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

                s->cursor += (size_t)cp_len;
                i += (size_t)cp_len;

                if (cp == '\n') {
                    s->line++;
                    s->column = 1;
                }
                else {
                    s->column++;
                }
            }

            size_t byte_len = s->cursor - start;

            core_string_t text = core_string_from_raw(s->input + start,
                                                       (int64_t)byte_len);
            scan_add_trivia(s, text, PARSE_TRIVIA_COMMENT);
            return true;
        }

        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (cp == '\n') {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    while (s->cursor < s->input_len) {
        scan_advance(s);
    }

    size_t byte_len = s->cursor - start;

    core_string_t text = core_string_from_raw(s->input + start,
                                               (int64_t)byte_len);
    scan_add_trivia(s, text, PARSE_TRIVIA_COMMENT);
    return false;
}

int64_t
scan_terminal_id(parse_scanner_t *s, core_string_t name)
{
    if (!s->grammar) {
        return PARSE_TOK_OTHER;
    }

    int64_t tid = parse_lookup_terminal(s->grammar, name);

    return tid ? tid : PARSE_TOK_OTHER;
}

parse_token_info_t *
scan_prev_token(parse_scanner_t *s)
{
    if (!s || !s->stream || s->stream->token_count <= 0) {
        return nullptr;
    }

    return s->stream->tokens[s->stream->token_count - 1];
}
