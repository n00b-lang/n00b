
#include "private/scanner/token_stream.h"
#include "unicode/encoding.h"
#include "lib/list.h"
#include "lib/string.h"
#include "lib/option.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void
scan_stream_reserve_tokens(parse_token_stream_t *ts, size_t needed)
{
    void *data = ts->tokens;
    if (!core_list_reserve_raw(&data, &ts->token_cap, needed,
                               sizeof(*ts->tokens))) {
        abort();
    }
    ts->tokens = data;
}

static void free_stream_token(parse_token_info_t *tok);

static void
append_pending_eof_trivia(parse_token_stream_t *ts)
{
    parse_scanner_t *scanner = ts->scanner;

    if (!scanner
        || (!scanner->pending_leading && !scanner->pending_trailing)) {
        return;
    }

    if (ts->token_count <= 0) {
        return;
    }

    parse_token_info_t *last = ts->tokens[ts->token_count - 1];
    parse_trivia_t *trailing = parse_token_trailing_trivia(last);
    parse_trivia_t **tail = &trailing;

    while (*tail) {
        tail = &(*tail)->next;
    }

    *tail = scanner->pending_leading;
    scanner->pending_leading = nullptr;

    while (*tail) {
        tail = &(*tail)->next;
    }

    *tail = scanner->pending_trailing;
    scanner->pending_trailing = nullptr;
    parse_token_set_trailing_trivia(last, trailing);
}

static void
finalize_metadata(parse_token_stream_t *ts)
{
    if (!ts->metadata_finalized && ts->scanner->finalize_cb
        && ts->scanner->status != SCAN_STATUS_INVALID) {
        ts->metadata_finalized = ts->scanner->finalize_cb(ts->scanner);
    }
}

static bool
ensure_filled(parse_token_stream_t *ts, int32_t target)
{
    int32_t required = target + (ts->scanner->token_suffix_lookahead ? 1 : 0);
    while (ts->token_count <= required) {
        if (ts->scanner->at_eof) {
            finalize_metadata(ts);
            return ts->token_count > target
                && ts->scanner->status != SCAN_STATUS_INCOMPLETE;
        }

        bool got = ts->scanner->cb(ts->scanner);

        if (!got) {
            ts->scanner->at_eof = true;
            scan_scanner_check_rest(ts->scanner);
            if (ts->scanner->status == SCAN_STATUS_ACTIVE) {
                ts->scanner->status = ts->scanner->source_final
                                          ? (ts->scanner->had_recovery
                                                 ? SCAN_STATUS_RECOVERED
                                                 : SCAN_STATUS_COMPLETE)
                                          : SCAN_STATUS_INCOMPLETE;
            }
            if (ts->scanner->status != SCAN_STATUS_INCOMPLETE) {
                append_pending_eof_trivia(ts);
            }
            finalize_metadata(ts);
            return ts->token_count > target
                && ts->scanner->status != SCAN_STATUS_INCOMPLETE;
        }
    }

    return ts->token_count > target;
}

parse_token_stream_t *
scan_token_stream_new(parse_scanner_t *scanner)
{
    assert(scanner);

    parse_token_stream_t *ts = core_alloc(parse_token_stream_t);

    ts->scanner     = scanner;
    ts->token_cap   = 128;
    ts->tokens      = core_alloc_array(parse_token_info_t *, 128);
    ts->token_count = 0;
    ts->pos         = 0;
    ts->next_index  = 0;
    ts->exhausted   = false;
    ts->metadata_finalized = false;

    scanner->stream = ts;
    ts->source = scanner->source;
    parse_source_retain(ts->source);

    return ts;
}

void
scan_token_stream_take_scanner(parse_token_stream_t *ts)
{
    if (ts) {
        ts->owns_scanner = true;
    }
}

void
scan_token_stream_free(parse_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    for (int32_t i = 0; i < ts->token_count; i++) {
        parse_token_free(ts->tokens[i]);
    }

    parse_source_release(ts->source);

    parse_scanner_t *scanner = ts->scanner;
    bool owns_scanner = ts->owns_scanner;

    if (scanner) {
        scanner->stream = nullptr;
    }

    core_free(ts->tokens);
    core_free(ts);

    if (owns_scanner) {
        scan_scanner_free(scanner);
    }
}

parse_token_info_t *
scan_stream_next(parse_token_stream_t *ts)
{
    if (!ensure_filled(ts, ts->pos)) {
        ts->exhausted = true;
        return nullptr;
    }

    return ts->tokens[ts->pos++];
}

bool
scan_stream_rewind(parse_token_stream_t *ts, int32_t n)
{
    int32_t new_pos = ts->pos - n;

    if (new_pos < 0) {
        return false;
    }

    ts->pos       = new_pos;
    ts->exhausted = false;

    return true;
}

parse_token_info_t *
scan_stream_get(parse_token_stream_t *ts, int32_t pos)
{
    if (pos < 0) {
        return nullptr;
    }

    if (!ensure_filled(ts, pos)) {
        return nullptr;
    }

    return ts->tokens[pos];
}

static bool
fold_token_into_trivia(parse_token_stream_t *ts, parse_token_info_t *token,
                       parse_token_info_t *next, parse_token_info_t *previous)
{
    size_t start = parse_token_start_byte(token);
    size_t end = parse_token_end_byte(token);
    parse_scanner_t *scanner = ts->scanner;
    if (start < scanner->source_base || end < start
        || end - scanner->source_base > scanner->input.u8_bytes) {
        return false;
    }

    core_string_t text = core_string_slice(
        scanner->input, start - scanner->source_base, end - start);
    parse_trivia_t *trivia = core_alloc(parse_trivia_t);
    *trivia = (parse_trivia_t){
        .text = core_string_dup(text),
        .kind = PARSE_TRIVIA_SPACE,
        .line = parse_token_line(token),
        .column = parse_token_column(token),
        .endline = parse_token_endline(token),
        .endcol = parse_token_endcol(token),
        .start_byte = start,
        .end_byte = end,
        .owns_text = true,
    };

    parse_trivia_t *head = parse_token_leading_trivia(token);
    parse_trivia_t *trailing = parse_token_trailing_trivia(token);
    parse_trivia_t **tail = &head;
    while (*tail) {
        tail = &(*tail)->next;
    }
    *tail = trivia;
    tail = &trivia->next;
    while (trailing) {
        parse_trivia_t *next = trailing->next;
        trailing->next = nullptr;
        *tail = trailing;
        tail = &(*tail)->next;
        trailing = next;
    }

    if (next) {
        *tail = parse_token_leading_trivia(next);
        parse_token_set_leading_trivia(next, head);
    }
    else if (previous) {
        parse_trivia_t *previous_head = parse_token_trailing_trivia(previous);
        parse_trivia_t **previous_tail = &previous_head;
        while (*previous_tail) {
            previous_tail = &(*previous_tail)->next;
        }
        *previous_tail = head;
        parse_token_set_trailing_trivia(previous, previous_head);
    }
    else {
        parse_token_set_leading_trivia(token, nullptr);
        parse_token_set_trailing_trivia(token, nullptr);
        parse_trivia_free(head);
        return false;
    }

    parse_token_set_leading_trivia(token, nullptr);
    parse_token_set_trailing_trivia(token, nullptr);
    free_stream_token(token);
    return true;
}

bool
scan_stream_tokens_to_trivia(parse_token_stream_t *ts,
                             bool (*drop)(parse_token_info_t *, void *),
                             void *ud)
{
    if (!ts || !drop) {
        return false;
    }

    while (ensure_filled(ts, ts->token_count)) {
    }

    int32_t count    = ts->token_count;
    int32_t kept     = 0;
    int32_t kept_pos = 0;
    bool    ok       = true;
    int32_t read     = 0;

    for (; read < count; read++) {
        parse_token_info_t *token = ts->tokens[read];

        if (drop(token, ud)) {
            parse_token_info_t *next = read + 1 < count
                                           ? ts->tokens[read + 1]
                                           : nullptr;
            parse_token_info_t *previous = kept > 0 ? ts->tokens[kept - 1]
                                                    : nullptr;

            if (fold_token_into_trivia(ts, token, next, previous)) {
                continue;
            }
            ok = false;
            break;
        }

        if (read < ts->pos) {
            kept_pos++;
        }
        ts->tokens[kept++] = token;
    }

    for (; read < count; read++) {
        if (read < ts->pos) {
            kept_pos++;
        }
        ts->tokens[kept++] = ts->tokens[read];
    }

    if (kept != count) {
        ts->token_count = kept;
        ts->pos         = kept_pos;
        ts->exhausted   = false;
    }

    return ok;
}

int32_t
scan_stream_token_count(parse_token_stream_t *ts)
{
    return ts->token_count;
}

void
scan_stream_reset(parse_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    for (int32_t i = 0; i < ts->token_count; i++) {
        if (ts->tokens[i]) {
            parse_token_free(ts->tokens[i]);
            ts->tokens[i] = nullptr;
        }
    }

    ts->token_count = 0;
    ts->pos         = 0;
    ts->next_index  = 0;
    ts->exhausted   = false;
    ts->metadata_finalized = false;

    scan_scanner_reset(ts->scanner);
}

static void
free_stream_token(parse_token_info_t *tok)
{
    parse_token_free(tok);
}

core_list_t(parse_token_info_t)
scan_stream_collect(parse_token_stream_t *ts)
{
    core_list_t(parse_token_info_t) tl = core_list_new_cap(parse_token_info_t, 64);

    parse_token_info_t *tok;

    while ((tok = scan_stream_next(ts)) != nullptr) {
        core_list_push(tl, *tok);
    }

    return tl;
}

typedef struct {
    parse_token_info_t **tokens;
    int32_t             count;
    int32_t             pos;
} array_scan_state_t;

static bool
array_scan_cb(parse_scanner_t *s)
{
    array_scan_state_t *st = (array_scan_state_t *)s->user_state;

    if (st->pos >= st->count) {
        return false;
    }

    parse_token_info_t *src = st->tokens[st->pos];

    parse_token_stream_t *ts = s->stream;

    scan_stream_reserve_tokens(ts, (size_t)ts->token_count + 1);

    parse_token_info_t *copy = parse_token_copy(src, false);

    parse_token_set_index(copy, ts->next_index++);

    ts->tokens[ts->token_count++] = copy;

    st->pos++;
    return true;
}

static void
array_scan_reset_cb(parse_scanner_t *s)
{
    array_scan_state_t *st = (array_scan_state_t *)s->user_state;
    st->pos = 0;
}

parse_token_stream_t *
scan_token_stream_from_array(parse_token_info_t **tokens, int32_t count)
{
    array_scan_state_t *st = core_alloc(array_scan_state_t);

    st->tokens = tokens;
    st->count  = count;
    st->pos    = 0;

    core_buffer_t *dummy_buf = core_buffer_empty();

    parse_scanner_t *sc = scan_scanner_new(dummy_buf, array_scan_cb, nullptr,
                                            core_option_none(core_string_t),
                                            st,
                                            array_scan_reset_cb);

    scan_scanner_take_state(sc, core_free);
    scan_scanner_take_buffer(sc);
    parse_token_stream_t *ts = scan_token_stream_new(sc);
    scan_token_stream_take_scanner(ts);

    return ts;
}

typedef struct {
    core_string_t input;
    uint32_t      byte_pos;
    int32_t       tok_index;
    uint32_t      line;
    uint32_t      column;
} cp_scan_state_t;

static void
cp_scan_emit(parse_token_stream_t *ts, parse_token_info_t *tok)
{
    scan_stream_reserve_tokens(ts, (size_t)ts->token_count + 1);
    ts->tokens[ts->token_count++] = tok;
}

static bool
cp_scan_cb(parse_scanner_t *s)
{
    cp_scan_state_t     *st = (cp_scan_state_t *)s->user_state;
    parse_token_stream_t *ts = s->stream;

    if (st->byte_pos >= (uint32_t)st->input.u8_bytes) {
        return false;
    }

    uint32_t start_pos = st->byte_pos;
    int32_t  cp = unicode_utf8_decode(st->input.data,
                                      (uint32_t)st->input.u8_bytes,
                                      &st->byte_pos);

    if (cp < 0) {
        st->byte_pos = start_pos + 1;
        st->column++;
        return true;
    }

    parse_token_info_t *tok = parse_token_new();
    parse_token_set_tid(tok, (int32_t)cp);
    parse_token_set_index(tok, ts->next_index++);
    parse_token_set_span(tok, start_pos, st->byte_pos);
    parse_token_set_position(tok, st->line, st->column, st->line,
                             st->column + 1);
    parse_token_set_origin(tok, PARSE_TOKEN_SOURCE);

    uint32_t cp_byte_len = st->byte_pos - start_pos;
    core_string_t val = core_string_from_raw(st->input.data + start_pos,
                                             (int64_t)cp_byte_len);
    parse_token_set_value(tok, core_option_set(core_string_t, val), true);

    cp_scan_emit(ts, tok);

    if (cp == '\n') {
        st->line++;
        st->column = 1;
    }
    else {
        st->column++;
    }

    return true;
}

static void
cp_scan_reset_cb(parse_scanner_t *s)
{
    cp_scan_state_t *st = (cp_scan_state_t *)s->user_state;
    st->byte_pos  = 0;
    st->tok_index = 0;
    st->line      = 1;
    st->column    = 1;
}

parse_token_stream_t *
scan_token_stream_from_codepoints(core_string_t input)
{
    cp_scan_state_t *st = core_alloc(cp_scan_state_t);

    st->input     = input;
    st->byte_pos  = 0;
    st->tok_index = 0;
    st->line      = 1;
    st->column    = 1;

    core_buffer_t *dummy_buf = core_buffer_empty();

    parse_scanner_t *sc = scan_scanner_new(dummy_buf, cp_scan_cb, nullptr,
                                            core_option_none(core_string_t),
                                            st,
                                            cp_scan_reset_cb);

    scan_scanner_take_state(sc, core_free);
    scan_scanner_take_buffer(sc);
    parse_token_stream_t *ts = scan_token_stream_new(sc);
    scan_token_stream_take_scanner(ts);

    return ts;
}
