#pragma once

#include "scanner/scanner.h"

struct ncc_token_stream_t {
    ncc_scanner_t    *scanner;

    ncc_token_info_t **tokens;
    int32_t             token_cap;
    int32_t             token_count;

    int32_t            pos;

    int32_t            next_index;

    bool               exhausted;
};

extern ncc_token_stream_t *
ncc_token_stream_new(ncc_scanner_t *scanner);

extern void ncc_token_stream_free(ncc_token_stream_t *ts);

extern ncc_token_info_t *ncc_stream_next(ncc_token_stream_t *ts);

extern bool ncc_stream_is_done(ncc_token_stream_t *ts);

extern ncc_token_info_t *ncc_stream_peek(ncc_token_stream_t *ts, int32_t n);

extern bool ncc_stream_rewind(ncc_token_stream_t *ts, int32_t n);

extern ncc_token_info_t *ncc_stream_lookback(ncc_token_stream_t *ts,
                                                int32_t n);

extern ncc_token_info_t *ncc_stream_get(ncc_token_stream_t *ts, int32_t pos);

extern int32_t ncc_stream_token_count(ncc_token_stream_t *ts);

extern void ncc_stream_reset(ncc_token_stream_t *ts);

typedef struct {
    int32_t pos;
} ncc_stream_mark_t;

extern ncc_stream_mark_t ncc_stream_save(ncc_token_stream_t *ts);

extern bool ncc_stream_restore(ncc_token_stream_t *ts,
                                 ncc_stream_mark_t mark);

static inline void
ncc_stream_commit(ncc_stream_mark_t mark)
{
    (void)mark;
}

extern ncc_list_t(ncc_token_info_t) ncc_stream_collect(ncc_token_stream_t *ts);

extern ncc_token_stream_t *
ncc_token_stream_from_array(ncc_token_info_t **tokens, int32_t count);

extern ncc_token_stream_t *
ncc_token_stream_from_codepoints(ncc_string_t input);

#define ncc_stream_foreach(ts, tok_var)                       \
    for (ncc_token_info_t *(tok_var) = ncc_stream_next(ts);  \
         (tok_var) != nullptr;                                    \
         (tok_var) = ncc_stream_next(ts))
