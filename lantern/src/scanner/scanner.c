
#include "scanner/scanner.h"
#include "scanner/token_stream.h"
#include "private/parse/grammar.h"
#include <assert.h>
#include <string.h>

static bool
is_line_terminator(core_codepoint_t cp)
{
    return cp == '\n' || cp == 0x2028;
}

static core_codepoint_t
decode_at(parse_scanner_t *s, size_t off, int *cp_len)
{
    if (off >= s->input.u8_bytes) {
        *cp_len = 0;
        return 0;
    }

    uint32_t pos = (uint32_t)off;
    int32_t  cp  = unicode_utf8_decode(s->input.data,
                                       (uint32_t)s->input.u8_bytes, &pos);

    if (cp < 0) {
        *cp_len = 1;
        return (core_codepoint_t)(uint8_t)s->input.data[off];
    }

    *cp_len = (int)(pos - (uint32_t)off);
    return (core_codepoint_t)cp;
}

static bool
partial_utf8_at_end(const parse_scanner_t *s)
{
    if (!s || s->source_final || s->cursor >= s->input.u8_bytes) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)s->input.data + s->cursor;
    size_t remaining = s->input.u8_bytes - s->cursor;
    size_t width = p[0] >= 0xC2 && p[0] <= 0xDF ? 2
                 : p[0] >= 0xE0 && p[0] <= 0xEF ? 3
                 : p[0] >= 0xF0 && p[0] <= 0xF4 ? 4 : 1;

    if (width == 1 || remaining >= width) {
        return false;
    }

    for (size_t i = 1; i < remaining; i++) {
        if (p[i] < 0x80 || p[i] > 0xBF) {
            return false;
        }
    }

    if (remaining > 1
        && ((p[0] == 0xE0 && p[1] < 0xA0)
            || (p[0] == 0xED && p[1] > 0x9F)
            || (p[0] == 0xF0 && p[1] < 0x90)
            || (p[0] == 0xF4 && p[1] > 0x8F))) {
        return false;
    }

    return true;
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
make_trivia(parse_scanner_t *s, core_string_t text)
{
    parse_trivia_t *t = core_alloc(parse_trivia_t);

    *t = (parse_trivia_t){
        .text = text,
        .line = s ? s->mark_line : 0,
        .column = s ? s->mark_col : 0,
        .endline = s ? s->line : 0,
        .endcol = s ? s->column : 0,
        .start_byte = s ? s->source_base + s->mark : 0,
        .end_byte = s ? s->source_base + s->cursor : 0,
        .owns_text = true,
    };
    return t;
}

static void
append_trivia(parse_trivia_t **head, parse_trivia_t **tail,
              parse_trivia_t *node)
{
    if (*tail) {
        (*tail)->next = node;
    }
    else {
        *head = node;
    }

    *tail = node;
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

    s->buffer    = buf;
    s->input     = core_buffer_string(buf);
    s->input_offset = 0;
    s->cursor    = 0;
    s->line      = 1;
    s->column    = 1;
    s->file      = file;
    s->cb        = cb;
    s->reset_cb  = reset_cb;
    s->user_state = state;
    s->grammar   = grammar;
    s->at_eof    = false;
    s->status    = SCAN_STATUS_ACTIVE;
    s->source_final = true;

    s->mark      = 0;
    s->mark_line = 1;
    s->mark_col  = 1;
    s->source    = parse_source_new(file);
    parse_source_append(s->source, core_buffer_string(buf));

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

    if (s->state_free_cb) {
        s->state_free_cb(s->user_state);
    }

    if (s->owns_buffer) {
        core_buffer_free(s->buffer);
    }

    parse_source_release(s->source);
    core_free(s);
}

void
scan_scanner_take_state(parse_scanner_t *s, scan_state_free_cb_t free_cb)
{
    if (s) {
        s->state_free_cb = free_cb;
    }
}

void
scan_scanner_set_finalize_callback(parse_scanner_t *s,
                                   scan_finalize_cb_t finalize_cb)
{
    if (s) {
        s->finalize_cb = finalize_cb;
    }
}

void
scan_scanner_set_state_clone(parse_scanner_t *s,
                             scan_state_clone_cb_t clone_cb)
{
    if (s) {
        s->state_clone_cb = clone_cb;
    }
}

void
scan_scanner_set_state_equal(parse_scanner_t *s,
                             scan_state_equal_cb_t equal_cb)
{
    if (s) {
        s->state_equal_cb = equal_cb;
    }
}

void
scan_scanner_set_state_shift(parse_scanner_t *s,
                             scan_state_shift_cb_t shift_cb)
{
    if (s) {
        s->state_shift_cb = shift_cb;
    }
}

void
scan_scanner_take_buffer(parse_scanner_t *s)
{
    if (s) {
        s->owns_buffer = true;
    }
}

void
scan_scanner_set_compact_callback(parse_scanner_t *s,
                                  scan_compact_cb_t compact_cb)
{
    if (s) {
        s->compact_cb = compact_cb;
    }
}

bool
scan_scanner_discard_consumed(parse_scanner_t *s)
{
    if (!s || !s->buffer || s->source_final || s->append_closed
        || !s->mark || s->mark > s->cursor || s->mark > s->input.u8_bytes) {
        return false;
    }

    size_t removed  = s->mark;
    size_t remain   = s->input.u8_bytes - removed;

    s->input_offset += removed;
    s->input.u8_bytes  = remain;
    s->cursor    -= removed;
    s->mark       = 0;
    s->source_base += removed;

    if (!remain || s->input_offset >= remain) {
        memmove(s->buffer->data,
                s->buffer->data + s->input_offset,
                remain);
        s->buffer->byte_len = remain;
        s->buffer->data[remain] = '\0';
        s->input_offset = 0;
    }

    s->input = core_string_borrow(s->buffer->data + s->input_offset, remain);

    if (s->compact_cb) {
        s->compact_cb(s, removed);
    }

    return true;
}

void
scan_set_incomplete(parse_scanner_t *s, core_string_t message,
                    size_t start_byte)
{
    if (!s || s->status == SCAN_STATUS_INVALID) {
        return;
    }

    s->status            = SCAN_STATUS_INCOMPLETE;
    s->status_message    = message;
    s->status_lexer      = s->lexer_context;
    s->status_start_byte = s->source_base + start_byte;
    s->status_end_byte   = s->source_base + s->cursor;
}

void
scan_set_invalid(parse_scanner_t *s, core_string_t message,
                 size_t start_byte)
{
    if (!s || s->status == SCAN_STATUS_INVALID) {
        return;
    }

    s->status            = SCAN_STATUS_INVALID;
    s->status_message    = message;
    s->status_lexer      = s->lexer_context;
    s->status_start_byte = s->source_base + start_byte;
    s->status_end_byte   = s->source_base + s->cursor;
}

void
scan_set_recovered(parse_scanner_t *s, core_string_t message,
                   size_t start_byte, size_t end_byte)
{
    if (!s) {
        return;
    }

    if (!s->had_recovery) {
        s->recovery_message    = message;
        s->recovery_lexer      = s->lexer_context;
        s->recovery_start_byte = s->source_base + start_byte;
        s->recovery_end_byte   = s->source_base + end_byte;
        s->had_recovery        = true;
    }

    s->status            = SCAN_STATUS_RECOVERED;
    s->status_message    = s->recovery_message;
    s->status_lexer      = s->recovery_lexer;
    s->status_start_byte = s->recovery_start_byte;
    s->status_end_byte   = s->recovery_end_byte;
}

bool
scan_scanner_set_recovery(parse_scanner_t *s, bool enable)
{
    if (!s || s->status != SCAN_STATUS_ACTIVE || s->cursor) {
        return false;
    }
    s->recovery_enabled = enable;
    return true;
}

bool
scan_scanner_recovery_enabled(const parse_scanner_t *s)
{
    return s && s->recovery_enabled;
}

scan_status_t
scan_status(const parse_scanner_t *s)
{
    return s ? s->status : SCAN_STATUS_INVALID;
}

bool
scan_diagnostic(const parse_scanner_t *s, scan_diagnostic_t *out)
{
    if (!s || !out || (s->status != SCAN_STATUS_INCOMPLETE
                       && s->status != SCAN_STATUS_INVALID
                       && s->status != SCAN_STATUS_RECOVERED)) {
        return false;
    }

    *out = (scan_diagnostic_t){
        .status = s->status,
        .message = s->status_message,
        .lexer = s->status_lexer,
        .start_byte = s->status_start_byte,
        .end_byte = s->status_end_byte,
    };
    return true;
}

bool
scan_scanner_append(parse_scanner_t *s, core_string_t data)
{
    if (!s || !s->buffer || s->append_closed
        || (s->source_final && s->status != SCAN_STATUS_INCOMPLETE)
        || (!s->source_final && s->status == SCAN_STATUS_INVALID)
        || !data.data || !data.u8_bytes) {
        return false;
    }

    core_buffer_append_string(s->buffer, data);
    parse_source_append(s->source, data);

    s->input             = core_string_borrow(
        s->buffer->data + s->input_offset,
        s->buffer->byte_len - s->input_offset);
    s->at_eof            = false;
    s->status            = SCAN_STATUS_ACTIVE;
    s->status_message    = (core_string_t){0};
    s->status_lexer      = (core_string_t){0};
    s->status_start_byte = 0;
    s->status_end_byte   = 0;

    if (s->stream) {
        s->stream->exhausted = false;
    }

    return true;
}

bool
scan_scanner_finish(parse_scanner_t *s)
{
    if (!s || s->append_closed || s->source_final) {
        return false;
    }

    s->source_final = true;
    s->append_closed = true;
    s->at_eof = false;
    s->status = SCAN_STATUS_ACTIVE;
    s->status_message = (core_string_t){0};
    s->status_lexer = (core_string_t){0};
    s->status_start_byte = 0;
    s->status_end_byte = 0;

    if (s->stream) {
        s->stream->exhausted = false;
    }

    return true;
}

void
scan_scanner_reset(parse_scanner_t *s)
{
    if (!s) {
        return;
    }

    s->cursor   = 0;
    s->source_base = 0;
    s->work_bytes = 0;
    s->line     = 1;
    s->column   = 1;
    s->mark     = 0;
    s->mark_line = 1;
    s->mark_col  = 1;
    s->at_eof   = false;
    s->status   = SCAN_STATUS_ACTIVE;
    s->status_message = (core_string_t){0};
    s->status_lexer = (core_string_t){0};
    s->status_start_byte = 0;
    s->status_end_byte = 0;
    s->had_recovery = false;
    s->recovery_message = (core_string_t){0};
    s->recovery_lexer = (core_string_t){0};
    s->recovery_start_byte = 0;
    s->recovery_end_byte = 0;

    parse_trivia_free(s->pending_leading);
    parse_trivia_free(s->pending_trailing);
    s->pending_leading  = nullptr;
    s->pending_leading_tail = nullptr;
    s->pending_trailing = nullptr;
    s->pending_trailing_tail = nullptr;
    s->last_trailing_tail = nullptr;

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

    if (pos >= s->input.u8_bytes) {
        return 0;
    }

    return (uint8_t)s->input.data[pos];
}

bool
scan_at_eof(parse_scanner_t *s)
{
    return s->cursor >= s->input.u8_bytes || partial_utf8_at_end(s);
}

size_t
scan_remaining(parse_scanner_t *s)
{
    if (s->cursor >= s->input.u8_bytes) {
        return 0;
    }

    return s->input.u8_bytes - s->cursor;
}

size_t
scan_offset(parse_scanner_t *s)
{
    return s->cursor;
}

size_t
scan_work_bytes(const parse_scanner_t *s)
{
    return s ? s->work_bytes : 0;
}

void
scan_advance(parse_scanner_t *s)
{
    if (s->cursor >= s->input.u8_bytes) {
        return;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    s->cursor += (size_t)cp_len;
    s->work_bytes += (size_t)cp_len;

    if (is_line_terminator(cp)) {
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
    for (int32_t i = 0; i < n && s->cursor < s->input.u8_bytes; i++) {
        scan_advance(s);
    }
}

void
scan_advance_bytes(parse_scanner_t *s, size_t n)
{
    size_t start = s->cursor;
    size_t end = s->cursor + n;

    if (end > s->input.u8_bytes) {
        end = s->input.u8_bytes;
    }

    for (size_t i = s->cursor; i < end;) {
        int cp_len;
        core_codepoint_t cp = decode_at(s, i, &cp_len);
        if (is_line_terminator(cp)) {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
        i += (size_t)cp_len;
    }

    s->cursor = end;
    s->work_bytes += end - start;
}

size_t
scan_match_str(parse_scanner_t *s, core_string_t lit)
{
    size_t len = lit.u8_bytes;

    if (s->cursor + len > s->input.u8_bytes) {
        return 0;
    }

    if (memcmp(s->input.data + s->cursor, lit.data, len) != 0) {
        s->work_bytes += len;
        return 0;
    }

    s->work_bytes += len;

    for (size_t i = 0; i < len; ) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (is_line_terminator(cp)) {
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
    if (s->cursor >= s->input.u8_bytes) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!pred(cp, nullptr)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;
    s->work_bytes += (size_t)cp_len;

    if (is_line_terminator(cp)) {
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
    if (s->cursor >= s->input.u8_bytes) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (!cc_match(cc, cp)) {
        return 0;
    }

    s->cursor += (size_t)cp_len;
    s->work_bytes += (size_t)cp_len;

    if (is_line_terminator(cp)) {
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
    if (s->cursor >= s->input.u8_bytes) {
        return 0;
    }

    int              cp_len;
    core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

    if (cp != target) {
        return 0;
    }

    s->cursor += (size_t)cp_len;
    s->work_bytes += (size_t)cp_len;

    if (is_line_terminator(cp)) {
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

    while (s->cursor < s->input.u8_bytes) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!pred(cp, nullptr)) {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->work_bytes += (size_t)cp_len;

        if (is_line_terminator(cp)) {
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

    while (s->cursor < s->input.u8_bytes) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(cc, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->work_bytes += (size_t)cp_len;

        if (is_line_terminator(cp)) {
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

    return core_string_from_raw(s->input.data + s->mark, (int64_t)len);
}

size_t
scan_mark_len(parse_scanner_t *s)
{
    return s->cursor - s->mark;
}

static void
take_token_value(parse_scanner_t *s, parse_token_info_t *tok,
                 core_option_t(core_string_t) value, bool owned)
{
    if (!core_option_is_set(value)) {
        return;
    }

    core_string_t text = core_option_get(value);
    core_string_t span = {0};

    if (parse_source_slice(s->source, parse_token_start_byte(tok),
                           parse_token_end_byte(tok), &span)
        && span.u8_bytes == text.u8_bytes
        && (!text.u8_bytes || memcmp(span.data, text.data, text.u8_bytes) == 0)
        && parse_token_use_source_value(tok)) {
        if (owned) {
            core_free(text.data);
        }
        return;
    }

    uintptr_t at = (uintptr_t)text.data;
    uintptr_t buffer_start = s->buffer ? (uintptr_t)s->buffer->data : 0;
    uintptr_t buffer_end = buffer_start
                         + (s->buffer ? s->buffer->byte_len : 0);

    if (!owned && text.data && at >= buffer_start && at <= buffer_end) {
        value = core_option_set(core_string_t, core_string_dup(text));
        owned = true;
    }

    parse_token_set_value(tok, value, owned);
}

static void
emit_token(parse_scanner_t *s, int32_t tid,
           core_option_t(core_string_t) value, bool owned)
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

    parse_token_info_t *tok = parse_token_new();

    parse_token_set_tid(tok, tid);
    parse_token_set_stream_source(tok, s->source);
    parse_token_set_span(tok, s->source_base + s->mark,
                         s->source_base + s->cursor);
    parse_token_set_index(tok, ts->next_index++);
    parse_token_set_origin(tok, PARSE_TOKEN_SOURCE);
    take_token_value(s, tok, value, owned);
    if (!parse_source_slice(s->source, parse_token_start_byte(tok),
                            parse_token_end_byte(tok), nullptr)) {
        parse_token_set_position(tok, s->mark_line, s->mark_col, s->line,
                                 s->column);
    }
    parse_token_set_leading_trivia(tok, s->pending_leading);
    parse_token_set_trailing_trivia(tok, s->pending_trailing);

    s->last_trailing_tail = s->pending_trailing_tail;

    s->pending_leading  = nullptr;
    s->pending_leading_tail = nullptr;
    s->pending_trailing = nullptr;
    s->pending_trailing_tail = nullptr;

    ts->tokens[ts->token_count++] = tok;

    s->mark      = s->cursor;
    s->mark_line = s->line;
    s->mark_col  = s->column;
}

void
scan_emit(parse_scanner_t *s, int32_t tid, core_option_t(core_string_t) value)
{
    emit_token(s, tid, value, false);
}

void
scan_emit_owned(parse_scanner_t *s, int32_t tid,
                core_option_t(core_string_t) value)
{
    emit_token(s, tid, value, true);
}

void
scan_emit_marked(parse_scanner_t *s, int32_t tid)
{
    core_string_t val = {
        .data = s->input.data + s->mark,
        .u8_bytes = s->cursor - s->mark,
    };

    emit_token(s, tid, core_option_set(core_string_t, val), false);
}

void
scan_add_leading_trivia_kind(parse_scanner_t    *s,
                                 core_string_t      text,
                                 parse_trivia_kind_t kind)
{
    parse_trivia_t *t = make_trivia(s, text);

    t->kind = kind;

    append_trivia(&s->pending_leading, &s->pending_leading_tail, t);
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

parse_trivia_t *
scan_add_trivia(parse_scanner_t *s, core_string_t text,
                    parse_trivia_kind_t kind)
{
    if (!s) {
        return nullptr;
    }

    if (s->trivia_attach == (uint8_t)SCAN_TRIVIA_ATTACH_LEFT) {
        parse_token_info_t *prev = scan_prev_token(s);
        parse_trivia_t     *t    = make_trivia(s, text);

        t->kind = kind;

        if (prev && !parse_token_no_trivia(prev)) {
            parse_trivia_t *trailing = parse_token_trailing_trivia(prev);

            if (s->pending_leading) {
                if (s->last_trailing_tail) {
                    s->last_trailing_tail->next = s->pending_leading;
                }
                else {
                    trailing = s->pending_leading;
                }
                s->last_trailing_tail = s->pending_leading_tail;
                s->pending_leading = nullptr;
                s->pending_leading_tail = nullptr;
            }

            append_trivia(&trailing, &s->last_trailing_tail, t);
            parse_token_set_trailing_trivia(prev, trailing);
        }
        else if (!prev) {
            append_trivia(&s->pending_leading,
                          &s->pending_leading_tail, t);
        }
        else {
            parse_trivia_free(t);
            t = nullptr;
        }

        return t;
    }

    scan_add_leading_trivia_kind(s, text, kind);
    return s->pending_leading_tail;
}

void
scan_add_trailing_trivia(parse_scanner_t *s, core_string_t text)
{
    parse_trivia_t *t = make_trivia(s, text);

    t->kind = PARSE_TRIVIA_COMMENT;

    append_trivia(&s->pending_trailing, &s->pending_trailing_tail, t);
}

int32_t
scan_skip_whitespace(parse_scanner_t *s)
{
    size_t  start = s->cursor;
    int32_t count = 0;

    while (s->cursor < s->input.u8_bytes) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (!cc_match(PARSE_CC_WHITESPACE, cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;

        if (is_line_terminator(cp)) {
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

        core_string_t text = core_string_from_raw(s->input.data + start,
                                                   (int64_t)byte_len);
        scan_add_leading_trivia_kind(s, text, PARSE_TRIVIA_SPACE);
    }

    return count;
}

void
scan_skip_line_comment(parse_scanner_t *s)
{
    size_t start = s->cursor;

    while (s->cursor < s->input.u8_bytes) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        if (is_line_terminator(cp)) {
            break;
        }

        s->cursor += (size_t)cp_len;
        s->column++;
    }

    size_t byte_len = s->cursor - start;

    if (byte_len > 0) {
        core_string_t text = core_string_from_raw(s->input.data + start,
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

    if (s->cursor + opener_len > s->input.u8_bytes
        || memcmp(s->input.data + s->cursor, opener.data, opener_len) != 0) {
        return false;
    }

    for (size_t i = 0; i < opener_len; ) {
        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;
        i += (size_t)cp_len;

        if (is_line_terminator(cp)) {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    while (s->cursor + closer_len <= s->input.u8_bytes) {
        if (memcmp(s->input.data + s->cursor, closer.data, closer_len) == 0) {
            for (size_t i = 0; i < closer_len; ) {
                int              cp_len;
                core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

                s->cursor += (size_t)cp_len;
                i += (size_t)cp_len;

                if (is_line_terminator(cp)) {
                    s->line++;
                    s->column = 1;
                }
                else {
                    s->column++;
                }
            }

            size_t byte_len = s->cursor - start;

            core_string_t text = core_string_from_raw(s->input.data + start,
                                                       (int64_t)byte_len);
            scan_add_trivia(s, text, PARSE_TRIVIA_COMMENT);
            return true;
        }

        int              cp_len;
        core_codepoint_t cp = decode_at(s, s->cursor, &cp_len);

        s->cursor += (size_t)cp_len;

        if (is_line_terminator(cp)) {
            s->line++;
            s->column = 1;
        }
        else {
            s->column++;
        }
    }

    while (s->cursor < s->input.u8_bytes) {
        scan_advance(s);
    }

    size_t byte_len = s->cursor - start;

    core_string_t text = core_string_from_raw(s->input.data + start,
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
