
#include "scanner/token_stream.h"
#include "unicode/encoding.h"
#include "lib/string.h"
#include "lib/option.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SCAN_EDIT_CHECKPOINT_TOKENS 32

struct scan_stream_checkpoint_t {
    int32_t token_count;
    int32_t next_index;
    size_t  cursor;
    size_t  mark;
    uint32_t line;
    uint32_t column;
    uint32_t mark_line;
    uint32_t mark_col;
    uint8_t trivia_attach;
    core_string_t lexer_context;
    void *state;
    parse_trivia_t *pending_leading;
    parse_trivia_t *pending_trailing;
};

static parse_trivia_t *
clone_trivia(const parse_trivia_t *source)
{
    parse_trivia_t *head = nullptr;
    parse_trivia_t **tail = &head;

    for (const parse_trivia_t *at = source; at; at = at->next) {
        parse_trivia_t *copy = core_alloc(parse_trivia_t);
        *copy = *at;
        copy->text = core_string_dup(at->text);
        copy->region_name = at->region_name.data
            ? core_string_dup(at->region_name) : (core_string_t){0};
        copy->regions = nullptr;
        parse_trivia_region_link_t **region_tail = &copy->regions;
        for (const parse_trivia_region_link_t *link = at->regions;
             link; link = link->next) {
            *region_tail = core_alloc(parse_trivia_region_link_t);
            **region_tail = *link;
            (*region_tail)->next = nullptr;
            region_tail = &(*region_tail)->next;
        }
        copy->owns_text = true;
        copy->owns_region_name = copy->region_name.data != nullptr;
        copy->next = nullptr;
        *tail = copy;
        tail = &copy->next;
    }
    return head;
}

static parse_trivia_t *
trivia_tail(parse_trivia_t *head)
{
    while (head && head->next) {
        head = head->next;
    }
    return head;
}

static void
free_checkpoint(parse_token_stream_t *ts, scan_stream_checkpoint_t *cp)
{
    if (cp->state && ts->scanner->state_free_cb) {
        ts->scanner->state_free_cb(cp->state);
    }
    parse_trivia_free(cp->pending_leading);
    parse_trivia_free(cp->pending_trailing);
    *cp = (scan_stream_checkpoint_t){0};
}

static void
truncate_checkpoints(parse_token_stream_t *ts, size_t keep)
{
    for (size_t i = keep; i < ts->checkpoint_count; i++) {
        free_checkpoint(ts, &ts->checkpoints[i]);
    }
    ts->checkpoint_count = keep;
}

static void
maybe_checkpoint(parse_token_stream_t *ts)
{
    parse_scanner_t *s = ts->scanner;

    if (!ts->edits_enabled || !s->state_clone_cb
        || s->source_base || s->input_offset
        || (ts->token_count && ts->token_count % SCAN_EDIT_CHECKPOINT_TOKENS)) {
        return;
    }
    if (ts->checkpoint_count
        && ts->checkpoints[ts->checkpoint_count - 1].token_count
               == ts->token_count) {
        return;
    }
    if (ts->checkpoint_count == ts->checkpoint_cap) {
        size_t cap = ts->checkpoint_cap ? ts->checkpoint_cap * 2 : 16;
        ts->checkpoints = core_realloc(ts->checkpoints,
                                       cap * sizeof(*ts->checkpoints));
        memset(ts->checkpoints + ts->checkpoint_cap, 0,
               (cap - ts->checkpoint_cap) * sizeof(*ts->checkpoints));
        ts->checkpoint_cap = cap;
    }

    scan_stream_checkpoint_t *cp = &ts->checkpoints[ts->checkpoint_count++];
    *cp = (scan_stream_checkpoint_t){
        .token_count = ts->token_count,
        .next_index = ts->next_index,
        .cursor = s->cursor,
        .mark = s->mark,
        .line = s->line,
        .column = s->column,
        .mark_line = s->mark_line,
        .mark_col = s->mark_col,
        .trivia_attach = s->trivia_attach,
        .lexer_context = s->lexer_context,
        .state = s->state_clone_cb(s->user_state),
        .pending_leading = clone_trivia(s->pending_leading),
        .pending_trailing = clone_trivia(s->pending_trailing),
    };
}

static void
ensure_checkpoint_capacity(parse_token_stream_t *ts, size_t needed)
{
    if (needed <= ts->checkpoint_cap) {
        return;
    }
    size_t cap = ts->checkpoint_cap ? ts->checkpoint_cap : 16;
    while (cap < needed) {
        cap *= 2;
    }
    ts->checkpoints = core_realloc(ts->checkpoints,
                                   cap * sizeof(*ts->checkpoints));
    memset(ts->checkpoints + ts->checkpoint_cap, 0,
           (cap - ts->checkpoint_cap) * sizeof(*ts->checkpoints));
    ts->checkpoint_cap = cap;
}

static void free_stream_token(parse_token_info_t *tok);

static bool
option_string_eq(core_option_t(core_string_t) a,
                 core_option_t(core_string_t) b)
{
    bool a_set = core_option_is_set(a);
    bool b_set = core_option_is_set(b);
    return a_set == b_set
        && (!a_set || core_string_eq(core_option_get(a), core_option_get(b)));
}

static bool
trivia_eq(const parse_trivia_t *a, const parse_trivia_t *b)
{
    while (a && b) {
        if (a->kind != b->kind || !core_string_eq(a->text, b->text)
            || !core_string_eq(a->region_name, b->region_name)) {
            return false;
        }
        a = a->next;
        b = b->next;
    }
    return !a && !b;
}

static bool
shifted_span_eq(size_t fresh, size_t old, ptrdiff_t delta)
{
    if (delta < 0) {
        size_t amount = (size_t)-delta;
        return old >= amount && fresh == old - amount;
    }
    return fresh == old + (size_t)delta;
}

static bool
shifted_optional_span_eq(bool fresh_set, size_t fresh_start, size_t fresh_end,
                         bool old_set, size_t old_start, size_t old_end,
                         ptrdiff_t delta)
{
    return fresh_set == old_set
        && (!fresh_set || (shifted_span_eq(fresh_start, old_start, delta)
                           && shifted_span_eq(fresh_end, old_end, delta)));
}

static bool
same_shifted_token(const parse_token_info_t *fresh,
                   const parse_token_info_t *old,
                   ptrdiff_t delta)
{
    size_t fresh_content_start = 0, fresh_content_end = 0;
    size_t old_content_start = 0, old_content_end = 0;
    size_t fresh_label_start = 0, fresh_label_end = 0;
    size_t old_label_start = 0, old_label_end = 0;
    bool fresh_content = parse_token_content_span(fresh, &fresh_content_start,
                                                  &fresh_content_end);
    bool old_content = parse_token_content_span(old, &old_content_start,
                                                &old_content_end);
    bool fresh_label = parse_token_label_span(fresh, &fresh_label_start,
                                              &fresh_label_end);
    bool old_label = parse_token_label_span(old, &old_label_start,
                                            &old_label_end);

    return parse_token_tid(fresh) == parse_token_tid(old)
        && parse_token_origin(fresh) == parse_token_origin(old)
        && option_string_eq(parse_token_value(fresh), parse_token_value(old))
        && option_string_eq(parse_token_literal_type(fresh),
                            parse_token_literal_type(old))
        && option_string_eq(parse_token_postfix_modifier(fresh),
                            parse_token_postfix_modifier(old))
        && trivia_eq(parse_token_leading_trivia(fresh),
                     parse_token_leading_trivia(old))
        && trivia_eq(parse_token_trailing_trivia(fresh),
                     parse_token_trailing_trivia(old))
        && shifted_span_eq(parse_token_start_byte(fresh),
                           parse_token_start_byte(old), delta)
        && shifted_span_eq(parse_token_end_byte(fresh),
                           parse_token_end_byte(old), delta)
        && shifted_optional_span_eq(fresh_content, fresh_content_start,
                                    fresh_content_end, old_content,
                                    old_content_start, old_content_end,
                                    delta)
        && shifted_optional_span_eq(fresh_label, fresh_label_start,
                                    fresh_label_end, old_label,
                                    old_label_start, old_label_end, delta);
}

static void
clear_stale_tokens(parse_token_stream_t *ts)
{
    for (int32_t i = 0; i < ts->stale_count; i++) {
        free_stream_token(ts->stale_tokens[i]);
    }
    core_free(ts->stale_tokens);
    ts->stale_tokens = nullptr;
    ts->stale_count = 0;
}

static void
clear_stale_checkpoints(parse_token_stream_t *ts)
{
    for (size_t i = 0; i < ts->stale_checkpoint_count; i++) {
        free_checkpoint(ts, &ts->stale_checkpoints[i]);
    }
    core_free(ts->stale_checkpoints);
    ts->stale_checkpoints = nullptr;
    ts->stale_checkpoint_count = 0;
}

static void
reconcile_stale_suffix(parse_token_stream_t *ts)
{
    int32_t fresh = ts->token_count - 1;
    int32_t old = ts->stale_count - 1;

    while (fresh >= 0 && old >= 0
           && parse_token_start_byte(ts->tokens[fresh]) >= ts->stale_new_end
           && parse_token_start_byte(ts->stale_tokens[old])
                  >= ts->stale_old_end
           && same_shifted_token(ts->tokens[fresh], ts->stale_tokens[old],
                                 ts->stale_byte_delta)) {
        parse_token_info_t *fresh_token = ts->tokens[fresh];
        parse_token_info_t *old_token = ts->stale_tokens[old];
        int32_t stable_index = parse_token_index(old_token);

        parse_token_free_contents(old_token);
        *old_token = *fresh_token;
        parse_token_set_index(old_token, stable_index);
        core_free(fresh_token);
        ts->tokens[fresh] = old_token;
        ts->stale_tokens[old] = nullptr;
        fresh--;
        old--;
    }
    clear_stale_tokens(ts);
    clear_stale_checkpoints(ts);
}

static size_t
shift_byte(size_t value, ptrdiff_t delta)
{
    return delta < 0 ? value - (size_t)-delta : value + (size_t)delta;
}

static uint32_t
shift_u32(uint32_t value, int64_t delta)
{
    return (uint32_t)((int64_t)value + delta);
}

static void
shift_trivia(parse_trivia_t *trivia,
             const scan_stream_checkpoint_t *checkpoint,
             const parse_scanner_t *scanner, ptrdiff_t byte_delta)
{
    int64_t line_delta = (int64_t)scanner->line - checkpoint->line;
    int64_t column_delta = (int64_t)scanner->column - checkpoint->column;

    for (; trivia; trivia = trivia->next) {
        trivia->start_byte = shift_byte(trivia->start_byte, byte_delta);
        trivia->end_byte = shift_byte(trivia->end_byte, byte_delta);
        if (trivia->line == checkpoint->line) {
            trivia->column = shift_u32(trivia->column, column_delta);
        }
        if (trivia->endline == checkpoint->line) {
            trivia->endcol = shift_u32(trivia->endcol, column_delta);
        }
        trivia->line = shift_u32(trivia->line, line_delta);
        trivia->endline = shift_u32(trivia->endline, line_delta);
    }
}

static void
shift_reused_token(parse_token_info_t *token,
                   const scan_stream_checkpoint_t *checkpoint,
                   const parse_scanner_t *scanner, ptrdiff_t byte_delta)
{
    size_t start = 0;
    size_t end = 0;

    shift_trivia(parse_token_leading_trivia(token), checkpoint, scanner,
                 byte_delta);
    shift_trivia(parse_token_trailing_trivia(token), checkpoint, scanner,
                 byte_delta);
    parse_token_set_span(token,
                         shift_byte(parse_token_start_byte(token), byte_delta),
                         shift_byte(parse_token_end_byte(token), byte_delta));
    if (parse_token_content_span(token, &start, &end)) {
        parse_token_set_content_span(token, shift_byte(start, byte_delta),
                                     shift_byte(end, byte_delta));
    }
    if (parse_token_label_span(token, &start, &end)) {
        parse_token_set_label_span(token, shift_byte(start, byte_delta),
                                   shift_byte(end, byte_delta));
    }
}

static bool
try_converge(parse_token_stream_t *ts)
{
    parse_scanner_t *s = ts->scanner;
    if (!ts->stale_checkpoint_count || !s->state_equal_cb
        || ts->stale_final_status != SCAN_STATUS_COMPLETE) {
        return false;
    }

    for (size_t i = 0; i < ts->stale_checkpoint_count; i++) {
        scan_stream_checkpoint_t *cp = &ts->stale_checkpoints[i];
        if (cp->cursor < ts->stale_old_end
            || shift_byte(cp->cursor, ts->stale_byte_delta) != s->cursor
            || !s->state_equal_cb(s->user_state, cp->state)) {
            continue;
        }

        int32_t stale_at = cp->token_count - ts->stale_base_token_count;
        if (stale_at < 0 || stale_at > ts->stale_count) {
            continue;
        }
        int32_t add = ts->stale_count - stale_at;
        if (ts->token_count + add > ts->token_cap) {
            int32_t cap = ts->token_cap;
            while (cap < ts->token_count + add) {
                cap *= 2;
            }
            ts->tokens = core_realloc(ts->tokens,
                                      (size_t)cap * sizeof(*ts->tokens));
            ts->token_cap = cap;
        }
        for (int32_t j = stale_at; j < ts->stale_count; j++) {
            parse_token_info_t *token = ts->stale_tokens[j];
            shift_reused_token(token, cp, s, ts->stale_byte_delta);
            ts->tokens[ts->token_count++] = token;
            ts->stale_tokens[j] = nullptr;
        }

        int32_t token_delta = ts->token_count
                            - (cp->token_count
                               + ts->stale_count - stale_at);
        ensure_checkpoint_capacity(
            ts, ts->checkpoint_count + ts->stale_checkpoint_count - i);
        for (size_t j = i; j < ts->stale_checkpoint_count; j++) {
            scan_stream_checkpoint_t moved = ts->stale_checkpoints[j];
            moved.cursor = shift_byte(moved.cursor, ts->stale_byte_delta);
            moved.mark = shift_byte(moved.mark, ts->stale_byte_delta);
            int64_t line_shift = (int64_t)s->line - cp->line;
            int64_t column_shift = (int64_t)s->column - cp->column;
            if (moved.line == cp->line) {
                moved.column = shift_u32(moved.column, column_shift);
            }
            if (moved.mark_line == cp->line) {
                moved.mark_col = shift_u32(moved.mark_col, column_shift);
            }
            moved.line = shift_u32(moved.line, line_shift);
            moved.mark_line = shift_u32(moved.mark_line, line_shift);
            moved.token_count += token_delta;
            if (s->state_shift_cb) {
                s->state_shift_cb(moved.state, ts->stale_edit_start,
                                  ts->stale_old_end,
                                  ts->stale_byte_delta);
            }
            ts->checkpoints[ts->checkpoint_count++] = moved;
            ts->stale_checkpoints[j] = (scan_stream_checkpoint_t){0};
        }

        int64_t line_delta = (int64_t)s->line - cp->line;
        int64_t column_delta = (int64_t)s->column - cp->column;
        s->line = shift_u32(ts->stale_final_line, line_delta);
        s->column = ts->stale_final_line == cp->line
                  ? shift_u32(ts->stale_final_column, column_delta)
                  : ts->stale_final_column;
        s->cursor = s->input.u8_bytes;
        s->mark = s->cursor;
        s->mark_line = s->line;
        s->mark_col = s->column;
        s->at_eof = true;
        s->status = s->had_recovery ? SCAN_STATUS_RECOVERED
                                    : SCAN_STATUS_COMPLETE;
        s->status_message = s->had_recovery ? s->recovery_message
                                            : (core_string_t){0};
        s->status_lexer = s->had_recovery ? s->recovery_lexer
                                          : (core_string_t){0};
        s->status_start_byte = s->had_recovery
                             ? s->recovery_start_byte : 0;
        s->status_end_byte = s->had_recovery ? s->recovery_end_byte : 0;
        clear_stale_tokens(ts);
        clear_stale_checkpoints(ts);
        return true;
    }
    return false;
}

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
        if (try_converge(ts)) {
            finalize_metadata(ts);
            return ts->token_count > target;
        }
        if (ts->scanner->at_eof) {
            finalize_metadata(ts);
            return ts->token_count > target
                && ts->scanner->status != SCAN_STATUS_INCOMPLETE;
        }

        maybe_checkpoint(ts);
        bool got = ts->scanner->cb(ts->scanner);

        if (!got) {
            ts->scanner->at_eof = true;
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
            if (ts->stale_count) {
                reconcile_stale_suffix(ts);
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

    truncate_checkpoints(ts, 0);
    core_free(ts->checkpoints);
    clear_stale_tokens(ts);
    clear_stale_checkpoints(ts);
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

parse_token_info_t *
scan_stream_previous_token(parse_token_stream_t *ts,
                           const parse_token_info_t *token)
{
    if (!ts || !token) return nullptr;
    int32_t lo = 0;
    int32_t hi = ts->token_count;
    while (lo < hi) {
        int32_t mid = lo + (hi - lo) / 2;
        parse_token_info_t *candidate = ts->tokens[mid];
        if (parse_token_index(candidate) < parse_token_index(token))
            lo = mid + 1;
        else hi = mid;
    }
    if (lo >= ts->token_count || ts->tokens[lo] != token || lo == 0)
        return nullptr;
    return ts->tokens[lo - 1];
}

bool
scan_stream_token_to_trivia(parse_token_stream_t *ts, int32_t pos)
{
    if (!ts || ts->edits_enabled || pos < 0
        || !ensure_filled(ts, pos) || pos >= ts->token_count) {
        return false;
    }

    parse_token_info_t *token = ts->tokens[pos];
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

    if (pos + 1 < ts->token_count) {
        parse_token_info_t *next = ts->tokens[pos + 1];
        *tail = parse_token_leading_trivia(next);
        parse_token_set_leading_trivia(next, head);
    }
    else if (pos > 0) {
        parse_token_info_t *previous = ts->tokens[pos - 1];
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
    memmove(ts->tokens + pos, ts->tokens + pos + 1,
            (size_t)(ts->token_count - pos - 1) * sizeof(*ts->tokens));
    ts->token_count--;
    if (pos < ts->pos) {
        ts->pos--;
    }
    ts->exhausted = false;
    return true;
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

    truncate_checkpoints(ts, 0);
    clear_stale_tokens(ts);
    clear_stale_checkpoints(ts);

    scan_scanner_reset(ts->scanner);
    maybe_checkpoint(ts);
}

bool
scan_stream_append(parse_token_stream_t *ts, core_string_t data)
{
    if (!ts || !scan_scanner_append(ts->scanner, data)) {
        return false;
    }

    ts->exhausted = false;
    ts->metadata_finalized = false;
    return true;
}

bool
scan_stream_finish(parse_token_stream_t *ts)
{
    if (!ts || !scan_scanner_finish(ts->scanner)) {
        return false;
    }
    ts->metadata_finalized = false;
    return true;
}

bool
scan_stream_enable_edits(parse_token_stream_t *ts)
{
    if (!ts || !ts->scanner || !ts->scanner->state_clone_cb
        || ts->token_count || ts->scanner->source_base
        || ts->scanner->input_offset) {
        return false;
    }
    ts->edits_enabled = true;
    maybe_checkpoint(ts);
    return ts->checkpoint_count == 1;
}

static void
free_stream_token(parse_token_info_t *tok)
{
    parse_token_free(tok);
}

bool
scan_stream_replace(parse_token_stream_t *ts,
                    size_t start_byte, size_t end_byte,
                    core_string_t replacement)
{
    size_t replacement_len = replacement.u8_bytes;

    if (!ts || !ts->scanner || !ts->scanner->buffer
        || !ts->edits_enabled || !ts->scanner->state_clone_cb
        || !ts->scanner->source_final
        || ts->scanner->source_base || ts->scanner->input_offset
        || start_byte > end_byte
        || end_byte > ts->scanner->buffer->byte_len
        || (replacement_len && !replacement.data)) {
        return false;
    }

    while (!ts->scanner->at_eof) {
        if (!ensure_filled(ts, ts->token_count)) {
            break;
        }
    }
    maybe_checkpoint(ts);
    if (!ts->checkpoint_count) {
        return false;
    }

    size_t chosen = 0;
    for (size_t i = 1; i < ts->checkpoint_count; i++) {
        if (ts->checkpoints[i].cursor > start_byte) {
            break;
        }
        chosen = i;
    }
    scan_stream_checkpoint_t *cp = &ts->checkpoints[chosen];
    parse_scanner_t *s = ts->scanner;

    clear_stale_checkpoints(ts);
    ts->stale_checkpoint_count = ts->checkpoint_count - chosen - 1;
    if (ts->stale_checkpoint_count) {
        ts->stale_checkpoints = core_alloc_array(
            scan_stream_checkpoint_t, ts->stale_checkpoint_count);
        memcpy(ts->stale_checkpoints, ts->checkpoints + chosen + 1,
               ts->stale_checkpoint_count
                   * sizeof(*ts->stale_checkpoints));
        memset(ts->checkpoints + chosen + 1, 0,
               ts->stale_checkpoint_count * sizeof(*ts->checkpoints));
    }
    ts->checkpoint_count = chosen + 1;

    ts->stale_count = ts->token_count - cp->token_count;
    if (ts->stale_count) {
        ts->stale_tokens = core_alloc_array(parse_token_info_t *,
                                             ts->stale_count);
        memcpy(ts->stale_tokens, ts->tokens + cp->token_count,
               (size_t)ts->stale_count * sizeof(*ts->stale_tokens));
        memset(ts->tokens + cp->token_count, 0,
               (size_t)ts->stale_count * sizeof(*ts->tokens));
    }
    ts->stale_old_end = end_byte;
    ts->stale_new_end = start_byte + replacement_len;
    ts->stale_edit_start = start_byte;
    ts->stale_byte_delta = (ptrdiff_t)replacement_len
                         - (ptrdiff_t)(end_byte - start_byte);
    ts->stale_base_token_count = cp->token_count;
    ts->stale_final_line = s->line;
    ts->stale_final_column = s->column;
    ts->stale_final_status = s->status;
    ts->token_count = cp->token_count;
    if (ts->pos > ts->token_count) {
        ts->pos = ts->token_count;
    }

    if (s->state_free_cb) {
        s->state_free_cb(s->user_state);
    }
    s->user_state = s->state_clone_cb(cp->state);
    parse_trivia_free(s->pending_leading);
    parse_trivia_free(s->pending_trailing);
    s->pending_leading = clone_trivia(cp->pending_leading);
    s->pending_leading_tail = trivia_tail(s->pending_leading);
    s->pending_trailing = clone_trivia(cp->pending_trailing);
    s->pending_trailing_tail = trivia_tail(s->pending_trailing);
    s->last_trailing_tail = ts->token_count
        ? trivia_tail(parse_token_trailing_trivia(
              ts->tokens[ts->token_count - 1]))
        : nullptr;

    core_buffer_t *buffer = s->buffer;
    size_t removed = end_byte - start_byte;
    core_string_t owned_replacement = {};
    uintptr_t replacement_at = (uintptr_t)replacement.data;
    uintptr_t buffer_start = (uintptr_t)buffer->data;
    uintptr_t buffer_end = buffer_start + buffer->byte_len;
    if (replacement_len && replacement_at >= buffer_start
        && replacement_at < buffer_end) {
        owned_replacement = core_string_dup(replacement);
        replacement = owned_replacement;
    }
    if (replacement_len > removed) {
        core_buffer_ensure(buffer, replacement_len - removed);
    }
    size_t suffix = buffer->byte_len - end_byte;
    if (replacement_len != removed) {
        memmove(buffer->data + start_byte + replacement_len,
                buffer->data + end_byte, suffix);
    }
    if (replacement_len) {
        memcpy(buffer->data + start_byte, replacement.data, replacement_len);
    }
    parse_source_replace(s->source, start_byte, end_byte, replacement);
    core_free(owned_replacement.data);
    buffer->byte_len = buffer->byte_len - removed + replacement_len;
    buffer->data[buffer->byte_len] = '\0';

    s->input = core_buffer_string(buffer);
    s->cursor = cp->cursor;
    s->mark = cp->mark;
    s->line = cp->line;
    s->column = cp->column;
    s->mark_line = cp->mark_line;
    s->mark_col = cp->mark_col;
    s->trivia_attach = cp->trivia_attach;
    s->lexer_context = cp->lexer_context;
    s->at_eof = false;
    s->status = SCAN_STATUS_ACTIVE;
    s->status_message = (core_string_t){0};
    s->status_lexer = (core_string_t){0};
    s->status_start_byte = 0;
    s->status_end_byte = 0;
    s->had_recovery = false;
    s->recovery_message = (core_string_t){0};
    s->recovery_lexer = (core_string_t){0};
    s->recovery_start_byte = 0;
    s->recovery_end_byte = 0;
    s->source_final = true;
    s->append_closed = true;
    ts->exhausted = false;
    ts->metadata_finalized = false;
    return true;
}

bool
scan_stream_insert(parse_token_stream_t *ts, size_t at_byte,
                   core_string_t text)
{
    return scan_stream_replace(ts, at_byte, at_byte, text);
}

bool
scan_stream_delete(parse_token_stream_t *ts,
                   size_t start_byte, size_t end_byte)
{
    return scan_stream_replace(ts, start_byte, end_byte, (core_string_t){});
}

bool
scan_stream_discard_consumed(parse_token_stream_t *ts)
{
    if (!ts || ts->edits_enabled) {
        return false;
    }
    int32_t drop = ts->pos > 1 ? ts->pos - 1 : 0;
    for (int32_t i = 0; i < drop; i++) {
        parse_token_free(ts->tokens[i]);
    }

    if (drop) {
        memmove(ts->tokens, ts->tokens + drop,
                (size_t)(ts->token_count - drop) * sizeof(*ts->tokens));
        ts->token_count -= drop;
        ts->pos -= drop;
    }

    bool source_discarded = scan_scanner_discard_consumed(ts->scanner);
    return source_discarded || drop > 0;
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
