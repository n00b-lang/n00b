#pragma once

#include "scanner/scanner.h"

typedef struct scan_stream_checkpoint_t scan_stream_checkpoint_t;

struct parse_token_stream_t {
    parse_scanner_t    *scanner;
    bool                owns_scanner;
    uint32_t            source;

    parse_token_info_t **tokens;
    int32_t             token_cap;
    int32_t             token_count;

    int32_t            pos;

    int32_t            next_index;

    bool               exhausted;
    bool               edits_enabled;
    bool               metadata_finalized;

    scan_stream_checkpoint_t *checkpoints;
    size_t                    checkpoint_count;
    size_t                    checkpoint_cap;

    parse_token_info_t **stale_tokens;
    int32_t             stale_count;
    size_t              stale_old_end;
    size_t              stale_new_end;
    size_t              stale_edit_start;
    ptrdiff_t           stale_byte_delta;
    int32_t             stale_base_token_count;
    scan_stream_checkpoint_t *stale_checkpoints;
    size_t                    stale_checkpoint_count;
    uint32_t             stale_final_line;
    uint32_t             stale_final_column;
    scan_status_t         stale_final_status;
};

extern parse_token_stream_t *
scan_token_stream_new(parse_scanner_t *scanner);

extern void scan_token_stream_take_scanner(parse_token_stream_t *ts);

extern void scan_token_stream_free(parse_token_stream_t *ts);

extern parse_token_info_t *scan_stream_next(parse_token_stream_t *ts);

extern bool scan_stream_is_done(parse_token_stream_t *ts);

extern parse_token_info_t *scan_stream_peek(parse_token_stream_t *ts, int32_t n);

extern bool scan_stream_rewind(parse_token_stream_t *ts, int32_t n);

extern parse_token_info_t *scan_stream_lookback(parse_token_stream_t *ts,
                                                int32_t n);

extern parse_token_info_t *scan_stream_get(parse_token_stream_t *ts, int32_t pos);

/* Find the token preceding a buffered token; stable token indices are not
 * stream positions after edits or token-to-trivia conversion. */
extern parse_token_info_t *scan_stream_previous_token(
    parse_token_stream_t *ts, const parse_token_info_t *token);

/* Replace a buffered token with whitespace trivia, removing it from the stream. */
extern bool scan_stream_token_to_trivia(parse_token_stream_t *ts, int32_t pos);

extern int32_t scan_stream_token_count(parse_token_stream_t *ts);

extern void scan_stream_reset(parse_token_stream_t *ts);

extern bool scan_stream_append(parse_token_stream_t *ts, core_string_t data);

extern bool scan_stream_finish(parse_token_stream_t *ts);

extern bool scan_stream_enable_edits(parse_token_stream_t *ts);

extern bool scan_stream_replace(parse_token_stream_t *ts,
                                size_t start_byte, size_t end_byte,
                                core_string_t replacement);
extern bool scan_stream_insert(parse_token_stream_t *ts, size_t at_byte,
                               core_string_t text);
extern bool scan_stream_delete(parse_token_stream_t *ts,
                               size_t start_byte, size_t end_byte);

extern bool scan_stream_discard_consumed(parse_token_stream_t *ts);

typedef struct {
    int32_t pos;
} scan_stream_mark_t;

extern scan_stream_mark_t scan_stream_save(parse_token_stream_t *ts);

extern bool scan_stream_restore(parse_token_stream_t *ts,
                                 scan_stream_mark_t mark);


extern core_list_t(parse_token_info_t) scan_stream_collect(parse_token_stream_t *ts);

extern parse_token_stream_t *
scan_token_stream_from_array(parse_token_info_t **tokens, int32_t count);

extern parse_token_stream_t *
scan_token_stream_from_codepoints(core_string_t input);

#define scan_stream_foreach(ts, tok_var)                       \
    for (parse_token_info_t *(tok_var) = scan_stream_next(ts);  \
         (tok_var) != nullptr;                                    \
         (tok_var) = scan_stream_next(ts))
