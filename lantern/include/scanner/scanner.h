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
typedef bool (*scan_finalize_cb_t)(parse_scanner_t *s);
typedef void (*scan_span_check_cb_t)(parse_scanner_t *s, size_t start,
                                     size_t end, int32_t tid);

typedef void (*scan_reset_cb_t)(parse_scanner_t *s);
typedef void (*scan_state_free_cb_t)(void *state);

typedef enum : uint8_t {
    SCAN_STATUS_ACTIVE,
    SCAN_STATUS_COMPLETE,
    SCAN_STATUS_INCOMPLETE,
    SCAN_STATUS_INVALID,
    SCAN_STATUS_RECOVERED,
} scan_status_t;

typedef enum : uint8_t {
#define SCAN_DIAG(id, text) SCAN_DIAG_##id,
#include "scanner/scan_diags.def"
#undef SCAN_DIAG
    SCAN_DIAG_COUNT,
} scan_diag_kind_t;

typedef struct {
    scan_diag_kind_t kind;
    core_string_t    detail;
} scan_problem_t;

#define SCAN_PROBLEM(id) ((scan_problem_t){.kind = SCAN_DIAG_##id})
#define SCAN_PROBLEM_WITH(id, text) \
    ((scan_problem_t){.kind = SCAN_DIAG_##id, .detail = (text)})

typedef struct {
    scan_status_t  status;
    scan_problem_t problem;
    core_string_t  lexer;
    size_t         start_byte;
    size_t         end_byte;
} scan_diagnostic_t;

typedef struct {
    scan_problem_t problem;
    size_t         start_byte;
    size_t         end_byte;
} scan_warning_t;

core_list_decl(scan_warning_t);
core_list_decl(scan_diagnostic_t);

struct parse_scanner_t {
    core_buffer_t     *buffer;
    bool               owns_buffer;
    core_string_t     input;
    size_t            input_offset;
    size_t            cursor;
    size_t            source_base;

    uint32_t                       line;
    uint32_t                       column;
    core_option_t(core_string_t)   file;
    uint32_t                       source;

    size_t            mark;
    uint32_t          mark_line;
    uint32_t          mark_col;

    parse_scan_cb_t        cb;
    scan_finalize_cb_t     finalize_cb;
    scan_reset_cb_t  reset_cb;
    scan_state_free_cb_t state_free_cb;
    void                 *user_state;

    parse_grammar_t   *grammar;

    parse_token_stream_t *stream;


    parse_trivia_t    *pending_leading;
    parse_trivia_t    *pending_leading_tail;
    parse_trivia_t    *pending_trailing;
    parse_trivia_t    *pending_trailing_tail;

    bool              at_eof;
    bool              source_final;
    bool              token_suffix_lookahead;
    scan_status_t     status;
    scan_problem_t    status_problem;
    core_string_t     lexer_context;
    core_string_t     status_lexer;
    size_t            status_start_byte;
    size_t            status_end_byte;
    bool              recovery_enabled;
    bool              had_recovery;
    scan_problem_t    recovery_problem;
    core_string_t     recovery_lexer;
    size_t            recovery_start_byte;
    size_t            recovery_end_byte;
    core_list_t(scan_warning_t) warnings;
    core_list_t(scan_diagnostic_t) recoveries;
    scan_span_check_cb_t span_check_cb;
    size_t            checked_to;
};

extern parse_scanner_t *
scan_scanner_new(core_buffer_t                *buf,
                 parse_scan_cb_t                cb,
                 parse_grammar_t               *grammar,
                 core_option_t(core_string_t)  file,
                 void                         *state,
                 scan_reset_cb_t          reset_cb);

extern void scan_scanner_free(parse_scanner_t *s);

extern void scan_scanner_take_state(parse_scanner_t       *s,
                                    scan_state_free_cb_t free_cb);

extern void scan_scanner_take_buffer(parse_scanner_t *s);

extern void scan_scanner_set_finalize_callback(parse_scanner_t *s,
                                                scan_finalize_cb_t finalize_cb);

extern void scan_set_incomplete(parse_scanner_t *s, scan_problem_t problem,
                                size_t start_byte);

extern void scan_set_invalid(parse_scanner_t *s, scan_problem_t problem,
                             size_t start_byte);

extern void scan_set_recovered(parse_scanner_t *s, scan_problem_t problem,
                               size_t start_byte, size_t end_byte);

extern core_string_t scan_problem_text(scan_problem_t problem);
extern scan_problem_t scan_problem_copy(scan_problem_t problem);

extern bool scan_scanner_set_recovery(parse_scanner_t *s, bool enable);

extern scan_status_t scan_status(const parse_scanner_t *s);

extern bool scan_diagnostic(const parse_scanner_t *s, scan_diagnostic_t *out);

extern void scan_add_warning(parse_scanner_t *s, scan_problem_t problem,
                             size_t start_byte, size_t end_byte);
extern size_t scan_warning_count(const parse_scanner_t *s);
extern void scan_scanner_set_span_check(parse_scanner_t *s,
                                        scan_span_check_cb_t cb);
extern void scan_scanner_check_rest(parse_scanner_t *s);
extern const scan_warning_t *scan_warning_at(const parse_scanner_t *s,
                                             size_t index);
extern size_t scan_recovery_count(const parse_scanner_t *s);
extern const scan_diagnostic_t *scan_recovery_at(const parse_scanner_t *s,
                                                 size_t index);

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

extern int32_t scan_skip_while(parse_scanner_t *s, scan_cp_predicate_fn pred);

extern void scan_mark(parse_scanner_t *s);

extern core_string_t scan_extract(parse_scanner_t *s);

extern size_t scan_mark_len(parse_scanner_t *s);

extern void scan_emit_owned(parse_scanner_t *s, int32_t tid,
                            core_option_t(core_string_t) value);

extern void scan_emit_marked(parse_scanner_t *s, int32_t tid);

extern void scan_add_leading_trivia_kind(parse_scanner_t     *s,
                                             core_string_t       text,
                                             parse_trivia_kind_t  kind);

extern parse_trivia_t *scan_add_trivia(parse_scanner_t    *s,
                                       core_string_t       text,
                                       parse_trivia_kind_t kind);

extern int32_t scan_skip_whitespace(parse_scanner_t *s);

extern int64_t scan_terminal_id(parse_scanner_t *s, core_string_t name);

extern parse_token_info_t *scan_prev_token(parse_scanner_t *s);
