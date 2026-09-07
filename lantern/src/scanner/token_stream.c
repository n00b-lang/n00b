
#include "scanner/token_stream.h"
#include "unicode/encoding.h"
#include "lib/string.h"
#include "lib/option.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static void
free_token_trivia(parse_token_info_t *tok)
{
    parse_trivia_free(tok->leading_trivia);
    parse_trivia_free(tok->trailing_trivia);
    tok->leading_trivia  = nullptr;
    tok->trailing_trivia = nullptr;
}

static bool
ensure_filled(parse_token_stream_t *ts, int32_t target)
{
    while (ts->token_count <= target) {
        if (ts->scanner->at_eof) {
            return false;
        }

        bool got = ts->scanner->cb(ts->scanner);

        if (!got) {
            ts->scanner->at_eof = true;
            return ts->token_count > target;
        }
    }

    return true;
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

    scanner->stream = ts;

    return ts;
}

void
scan_token_stream_free(parse_token_stream_t *ts)
{
    if (!ts) {
        return;
    }

    for (int32_t i = 0; i < ts->token_count; i++) {
        if (ts->tokens[i]) {
            free_token_trivia(ts->tokens[i]);
            core_free(ts->tokens[i]);
        }
    }

    core_free(ts->tokens);
    core_free(ts);
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
scan_stream_is_done(parse_token_stream_t *ts)
{
    if (ts->exhausted) {
        return true;
    }

    if (!ensure_filled(ts, ts->pos)) {
        ts->exhausted = true;
        return true;
    }

    return false;
}

parse_token_info_t *
scan_stream_peek(parse_token_stream_t *ts, int32_t n)
{
    int32_t target = ts->pos + n;

    if (target < 0) {
        return nullptr;
    }

    if (!ensure_filled(ts, target)) {
        return nullptr;
    }

    return ts->tokens[target];
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
scan_stream_lookback(parse_token_stream_t *ts, int32_t n)
{
    int32_t target = ts->pos - n;

    if (target < 0 || target >= ts->token_count) {
        return nullptr;
    }

    return ts->tokens[target];
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
            free_token_trivia(ts->tokens[i]);
            core_free(ts->tokens[i]);
            ts->tokens[i] = nullptr;
        }
    }

    ts->token_count = 0;
    ts->pos         = 0;
    ts->next_index  = 0;
    ts->exhausted   = false;

    scan_scanner_reset(ts->scanner);
}

scan_stream_mark_t
scan_stream_save(parse_token_stream_t *ts)
{
    return (scan_stream_mark_t){ .pos = ts->pos };
}

bool
scan_stream_restore(parse_token_stream_t *ts, scan_stream_mark_t mark)
{
    if (mark.pos < 0 || mark.pos > ts->token_count) {
        return false;
    }

    ts->pos       = mark.pos;
    ts->exhausted = false;

    return true;
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

    parse_token_info_t *copy = core_alloc(parse_token_info_t);
    *copy = *src;
    copy->index = ts->next_index++;

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

    return scan_token_stream_new(sc);
}

typedef struct {
    const char *data;
    uint32_t    byte_len;
    uint32_t    byte_pos;
    int32_t     tok_index;
    uint32_t    line;
    uint32_t    column;
} cp_scan_state_t;

static void
cp_scan_emit(parse_token_stream_t *ts, parse_token_info_t *tok)
{
    if (ts->token_count >= ts->token_cap) {
        int32_t new_cap = ts->token_cap ? ts->token_cap * 2 : 128;
        parse_token_info_t **new_tokens
            = core_alloc_array(parse_token_info_t *, new_cap);

        if (ts->tokens && ts->token_count > 0) {
            memcpy(new_tokens, ts->tokens,
                   (size_t)ts->token_count * sizeof(parse_token_info_t *));
        }

        core_free(ts->tokens);
        ts->tokens    = new_tokens;
        ts->token_cap = new_cap;
    }

    ts->tokens[ts->token_count++] = tok;
}

static bool
cp_scan_cb(parse_scanner_t *s)
{
    cp_scan_state_t     *st = (cp_scan_state_t *)s->user_state;
    parse_token_stream_t *ts = s->stream;

    if (st->byte_pos >= st->byte_len) {
        return false;
    }

    uint32_t start_pos = st->byte_pos;
    int32_t  cp = unicode_utf8_decode(st->data, st->byte_len,
                                            &st->byte_pos);

    if (cp < 0) {
        st->byte_pos = start_pos + 1;
        st->column++;
        return true;
    }

    parse_token_info_t *tok = core_alloc(parse_token_info_t);
    tok->tid    = (int32_t)cp;
    tok->index  = ts->next_index++;
    tok->line   = st->line;
    tok->column = st->column;

    uint32_t cp_byte_len = st->byte_pos - start_pos;
    core_string_t val = core_string_from_raw(st->data + start_pos,
                                               (int64_t)cp_byte_len);
    tok->value = core_option_set(core_string_t, val);

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

    st->data      = input.data;
    st->byte_len  = (uint32_t)input.u8_bytes;
    st->byte_pos  = 0;
    st->tok_index = 0;
    st->line      = 1;
    st->column    = 1;

    core_buffer_t *dummy_buf = core_buffer_empty();

    parse_scanner_t *sc = scan_scanner_new(dummy_buf, cp_scan_cb, nullptr,
                                            core_option_none(core_string_t),
                                            st,
                                            cp_scan_reset_cb);

    return scan_token_stream_new(sc);
}
