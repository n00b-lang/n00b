#pragma once

#include "scanner/scanner.h"

struct parse_token_stream_t {
    parse_scanner_t    *scanner;
    bool                owns_scanner;
    uint32_t            source;

    parse_token_info_t **tokens;
    size_t              token_cap;
    int32_t             token_count;

    int32_t            pos;

    int32_t            next_index;

    bool               exhausted;
    bool               metadata_finalized;
};

extern parse_token_stream_t *
scan_token_stream_new(parse_scanner_t *scanner);

extern void scan_token_stream_take_scanner(parse_token_stream_t *ts);

extern void scan_token_stream_free(parse_token_stream_t *ts);

extern parse_token_info_t *scan_stream_next(parse_token_stream_t *ts);

extern bool scan_stream_rewind(parse_token_stream_t *ts, int32_t n);

extern parse_token_info_t *scan_stream_get(parse_token_stream_t *ts, int32_t pos);

extern bool scan_stream_tokens_to_trivia(
    parse_token_stream_t *ts, bool (*drop)(parse_token_info_t *, void *),
    void *ud);

extern int32_t scan_stream_token_count(parse_token_stream_t *ts);

extern void scan_stream_reset(parse_token_stream_t *ts);


extern core_list_t(parse_token_info_t) scan_stream_collect(parse_token_stream_t *ts);

extern parse_token_stream_t *
scan_token_stream_from_array(parse_token_info_t **tokens, int32_t count);

extern parse_token_stream_t *
scan_token_stream_from_codepoints(core_string_t input);

#define scan_stream_foreach(ts, tok_var)                       \
    for (parse_token_info_t *(tok_var) = scan_stream_next(ts);  \
         (tok_var) != nullptr;                                    \
         (tok_var) = scan_stream_next(ts))
