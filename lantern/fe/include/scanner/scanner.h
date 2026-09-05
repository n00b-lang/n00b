#pragma once

#include "ncc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/dict.h"
#include "lib/result.h"
#include "parse/token.h"
#include "parse/types.h"
#include "unicode/encoding.h"
#include "unicode/identifiers.h"
#include "unicode/query.h"

typedef struct ncc_scanner_t      ncc_scanner_t;
typedef struct ncc_token_stream_t ncc_token_stream_t;

#define NCC_ERR_SCAN_NOT_FOUND  (-10)

typedef bool (*ncc_scan_cb_t)(ncc_scanner_t *s);

typedef void (*ncc_scan_reset_cb_t)(ncc_scanner_t *s);

struct ncc_scanner_t {
    const char       *input;
    size_t            input_len;
    size_t            cursor;

    uint32_t                       line;
    uint32_t                       column;
    ncc_option_t(ncc_string_t)   file;

    size_t            mark;
    uint32_t          mark_line;
    uint32_t          mark_col;

    ncc_scan_cb_t        cb;
    ncc_scan_reset_cb_t  reset_cb;
    void                 *user_state;

    ncc_grammar_t   *grammar;

    ncc_token_stream_t *stream;

    ncc_dict_t *terminal_ids;

    ncc_trivia_t    *pending_leading;
    ncc_trivia_t    *pending_trailing;

    bool              at_eof;
};

extern ncc_scanner_t *
ncc_scanner_new(ncc_buffer_t                *buf,
                 ncc_scan_cb_t                cb,
                 ncc_grammar_t               *grammar,
                 ncc_option_t(ncc_string_t)  file,
                 void                         *state,
                 ncc_scan_reset_cb_t          reset_cb);

extern void ncc_scanner_free(ncc_scanner_t *s);

extern void ncc_scanner_reset(ncc_scanner_t *s);

extern ncc_codepoint_t ncc_scan_peek(ncc_scanner_t *s, int32_t offset);

extern uint8_t ncc_scan_peek_byte(ncc_scanner_t *s, int32_t offset);

extern bool ncc_scan_at_eof(ncc_scanner_t *s);

extern size_t ncc_scan_remaining(ncc_scanner_t *s);

extern size_t ncc_scan_offset(ncc_scanner_t *s);

extern void ncc_scan_advance(ncc_scanner_t *s);

extern void ncc_scan_advance_n(ncc_scanner_t *s, int32_t n);

extern void ncc_scan_advance_bytes(ncc_scanner_t *s, size_t n);

extern size_t ncc_scan_match_str(ncc_scanner_t *s, const char *lit);

extern size_t ncc_scan_match_if(ncc_scanner_t *s, ncc_cp_predicate_fn pred);

extern size_t ncc_scan_match_class(ncc_scanner_t *s, ncc_char_class_t cc);

extern size_t ncc_scan_match_cp(ncc_scanner_t *s, ncc_codepoint_t cp);

extern int32_t ncc_scan_skip_while(ncc_scanner_t *s, ncc_cp_predicate_fn pred);

extern int32_t ncc_scan_skip_class(ncc_scanner_t *s, ncc_char_class_t cc);

extern size_t ncc_scan_skip_until_cp(ncc_scanner_t *s, ncc_codepoint_t stop);

extern ncc_result_t(size_t)
ncc_scan_skip_until_str(ncc_scanner_t *s, const char *needle);

extern void ncc_scan_mark(ncc_scanner_t *s);

extern ncc_string_t ncc_scan_extract(ncc_scanner_t *s);

extern size_t ncc_scan_mark_len(ncc_scanner_t *s);

extern void ncc_scan_emit(ncc_scanner_t *s, int32_t tid,
                            ncc_option_t(ncc_string_t) value);

extern void ncc_scan_emit_marked(ncc_scanner_t *s, int32_t tid);

extern void ncc_scan_add_leading_trivia(ncc_scanner_t *s,
                                          ncc_string_t text);

extern void ncc_scan_add_trailing_trivia(ncc_scanner_t *s,
                                           ncc_string_t text);

extern int32_t ncc_scan_skip_whitespace(ncc_scanner_t *s);

extern void ncc_scan_skip_line_comment(ncc_scanner_t *s);

extern bool ncc_scan_skip_block_comment(ncc_scanner_t *s,
                                          const char *opener,
                                          const char *closer);

extern int64_t ncc_scan_terminal_id(ncc_scanner_t *s, const char *name);
