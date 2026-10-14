#pragma once

#include <stdarg.h>

#include "writ/writ.h"
#include "lib/arena.h"
#include "lib/buffer.h"
#include "lib/diagnostic.h"
#include "lib/dict.h"
#include "lib/indexed_table.h"
#include "lib/list.h"

typedef uint32_t writ_sym_t;

typedef struct {
    uint32_t source;
    uint32_t start;
    uint32_t end;
} writ_loc_t;

core_list_decl(uint32_t);

#define WRIT_NO_LOC ((writ_loc_t){UINT32_MAX, 0, 0})
#define S(text) (int)(text).length, (text).data

typedef struct {
    writ_source_kind_t    kind;
    writ_origin_t         origin;
    writ_str_t            path;
    writ_str_t            text;
    core_list_t(uint32_t) line_starts;
} writ_source_t;

typedef struct {
    writ_loc_t loc;
    writ_str_t text;
    writ_str_t extra;
} writ_diag_part_t;

core_list_decl(writ_diag_part_t);

typedef struct {
    uint32_t                      code;
    writ_str_t                    message;
    writ_loc_t                    primary;
    core_list_t(writ_diag_part_t) secondary;
    core_list_t(writ_diag_part_t) suggestions;
    const writ_diag_t            *view;
} writ_diag_record_t;

typedef struct {
    void (*release)(void *object);
    void *object;
} writ_cleanup_t;

core_list_decl(writ_source_t);
core_list_decl(writ_diag_record_t);
core_list_decl(writ_cleanup_t);

typedef enum {
#define WRIT_DIAG(id, code, severity, text) WRIT_D_##id,
#include "diag_codes.def"
#undef WRIT_DIAG
    WRIT_D_COUNT,
} writ_diag_code_t;

typedef enum {
    WRIT_GRAMMAR_WRIT,
    WRIT_GRAMMAR_JSON,
    WRIT_GRAMMAR_COUNT,
} writ_grammar_id_t;

struct writ_session {
    core_arena_t                    arena;
    core_indexed_table_t           *strings;
    core_list_t(writ_source_t)      sources;
    core_diagnostics_t             *diagnostics;
    core_list_t(writ_diag_record_t) diags;
    writ_diag_sink_fn               sink;
    void                           *sink_context;
    writ_process_runner_t           runner;
    writ_str_t                      llvm_prefix;
    core_list_t(writ_cleanup_t)     cleanups;
    void                           *grammars[WRIT_GRAMMAR_COUNT];
};

void      *writ_alloc(writ_session_t *session, size_t size);
void       writ_on_session_free(writ_session_t *session, void (*release)(void *), void *object);
writ_str_t writ_str_copy(writ_session_t *session, const char *data, size_t length);
writ_str_t writ_str_format(writ_session_t *session, const char *format, ...);
writ_str_t writ_str_from_buffer(writ_session_t *session, core_buffer_t *buffer);
bool       writ_str_eq(writ_str_t a, writ_str_t b);
writ_str_t writ_cstr(const char *text);

writ_sym_t writ_intern(writ_session_t *session, const char *data, size_t length);
writ_sym_t writ_intern_str(writ_session_t *session, writ_str_t text);
writ_str_t writ_sym_str(const writ_session_t *session, writ_sym_t sym);

typedef enum {
    WRIT_CASE_KEEP,
    WRIT_CASE_SNAKE,
    WRIT_CASE_UPPER,
    WRIT_CASE_PASCAL,
    WRIT_CASE_CAMEL,
} writ_case_t;

writ_str_t writ_name_case(writ_session_t *session, writ_str_t name, writ_case_t style);

uint32_t             writ_source_add(writ_session_t *session, writ_source_kind_t kind,
                                     writ_origin_t origin, writ_str_t path, writ_str_t text);
const writ_source_t *writ_source_at(const writ_session_t *session, uint32_t id);
writ_span_t          writ_loc_span(const writ_session_t *session, writ_loc_t loc);
writ_str_t           writ_loc_text(const writ_session_t *session, writ_loc_t loc);
writ_loc_t           writ_loc_join(writ_loc_t first, writ_loc_t last);

uint32_t writ_report(writ_session_t *session, writ_diag_code_t code, writ_loc_t loc,
                     const char *format, ...);
void     writ_report_label(writ_session_t *session, uint32_t diag, writ_loc_t loc,
                           writ_str_t label);
void     writ_report_suggest(writ_session_t *session, uint32_t diag, writ_loc_t loc,
                             writ_str_t replacement, writ_str_t description);
bool     writ_session_failed(const writ_session_t *session);
void     writ_diag_flush(writ_session_t *session, size_t from);

typedef struct writ_map writ_map_t;

writ_map_t *writ_map_new(writ_session_t *session);
void        writ_map_put(writ_map_t *map, uint64_t key, uint64_t value);
bool        writ_map_get(const writ_map_t *map, uint64_t key, uint64_t *value);

typedef struct {
    writ_str_t out;
    writ_str_t err;
    int        exit_code;
} writ_run_result_t;

writ_status_t writ_run(writ_session_t *session, const char *const *argv, writ_str_t input, writ_run_result_t *result);
writ_str_t    writ_tool_path(writ_session_t *session, const char *tool);
writ_str_t    writ_tool_line(writ_session_t *session, const char *const *argv);
writ_str_t    writ_sysroot(writ_session_t *session, writ_str_t requested);

typedef struct {
    core_buffer_t *buffer;
    uint32_t       depth;
    bool           at_line_start;
} writ_emit_t;

void       writ_emit_init(writ_emit_t *emit);
void       writ_emit_text(writ_emit_t *emit, const char *format, ...);
void       writ_emit_line(writ_emit_t *emit, const char *format, ...);
void       writ_emit_open(writ_emit_t *emit, const char *format, ...);
void       writ_emit_close(writ_emit_t *emit, const char *text);
void       writ_emit_indent(writ_emit_t *emit, int delta);
writ_str_t writ_emit_finish(writ_emit_t *emit, writ_session_t *session);
void       writ_emit_discard(writ_emit_t *emit);
