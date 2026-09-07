#pragma once

#include "core.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/dict.h"
#include "parse/token.h"
#include "parse/types.h"
#include "unicode/encoding.h"
#include "unicode/identifiers.h"
#include "unicode/query.h"

typedef struct parse_scanner_t      parse_scanner_t;
typedef struct parse_token_stream_t parse_token_stream_t;

typedef bool (*parse_scan_cb_t)(parse_scanner_t *s);

typedef void (*scan_reset_cb_t)(parse_scanner_t *s);

struct parse_scanner_t {
    const char       *input;
    size_t            input_len;
    size_t            cursor;

    uint32_t                       line;
    uint32_t                       column;
    core_option_t(core_string_t)   file;

    size_t            mark;
    uint32_t          mark_line;
    uint32_t          mark_col;

    parse_scan_cb_t        cb;
    scan_reset_cb_t  reset_cb;
    void                 *user_state;

    parse_grammar_t   *grammar;

    parse_token_stream_t *stream;


    parse_trivia_t    *pending_leading;
    parse_trivia_t    *pending_trailing;

    uint8_t           trivia_attach;
    bool              at_eof;
};

extern parse_scanner_t *
scan_scanner_new(core_buffer_t                *buf,
                 parse_scan_cb_t                cb,
                 parse_grammar_t               *grammar,
                 core_option_t(core_string_t)  file,
                 void                         *state,
                 scan_reset_cb_t          reset_cb);

extern void scan_scanner_free(parse_scanner_t *s);

extern void scan_scanner_reset(parse_scanner_t *s);

extern core_codepoint_t scan_peek(parse_scanner_t *s, int32_t offset);

extern uint8_t scan_peek_byte(parse_scanner_t *s, int32_t offset);

extern bool scan_at_eof(parse_scanner_t *s);

extern size_t scan_remaining(parse_scanner_t *s);

extern size_t scan_offset(parse_scanner_t *s);

extern void scan_advance(parse_scanner_t *s);

extern void scan_advance_n(parse_scanner_t *s, int32_t n);

extern void scan_advance_bytes(parse_scanner_t *s, size_t n);

extern size_t scan_match_str(parse_scanner_t *s, core_string_t lit);

extern size_t scan_match_if(parse_scanner_t *s, scan_cp_predicate_fn pred);

extern size_t scan_match_class(parse_scanner_t *s, parse_char_class_t cc);

extern size_t scan_match_cp(parse_scanner_t *s, core_codepoint_t cp);

extern int32_t scan_skip_while(parse_scanner_t *s, scan_cp_predicate_fn pred);

extern int32_t scan_skip_class(parse_scanner_t *s, parse_char_class_t cc);

extern void scan_mark(parse_scanner_t *s);

extern core_string_t scan_extract(parse_scanner_t *s);

extern size_t scan_mark_len(parse_scanner_t *s);

extern void scan_emit(parse_scanner_t *s, int32_t tid,
                            core_option_t(core_string_t) value);

extern void scan_emit_marked(parse_scanner_t *s, int32_t tid);

extern void scan_add_leading_trivia(parse_scanner_t *s,
                                          core_string_t text);

extern void scan_add_leading_trivia_kind(parse_scanner_t     *s,
                                             core_string_t       text,
                                             parse_trivia_kind_t  kind);

typedef enum : uint8_t {
    SCAN_TRIVIA_ATTACH_RIGHT,
    SCAN_TRIVIA_ATTACH_LEFT,
} scan_trivia_attach_t;

extern void scan_set_trivia_attach(parse_scanner_t       *s,
                                       scan_trivia_attach_t  how);

extern scan_trivia_attach_t scan_trivia_attach(const parse_scanner_t *s);

extern void scan_add_trivia(parse_scanner_t     *s,
                                core_string_t       text,
                                parse_trivia_kind_t  kind);

extern void scan_add_trailing_trivia(parse_scanner_t *s,
                                           core_string_t text);

extern int32_t scan_skip_whitespace(parse_scanner_t *s);

extern void scan_skip_line_comment(parse_scanner_t *s);

extern bool scan_skip_block_comment(parse_scanner_t *s,
                                        core_string_t opener,
                                        core_string_t closer);

extern int64_t scan_terminal_id(parse_scanner_t *s, core_string_t name);

extern parse_token_info_t *scan_prev_token(parse_scanner_t *s);
