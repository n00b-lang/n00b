#ifndef WRIT_WRIT_H
#define WRIT_WRIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WRIT_API_VERSION 1u

typedef struct writ_session writ_session_t;
typedef struct writ_unit writ_unit_t;
typedef struct writ_model writ_model_t;
typedef struct writ_platform writ_platform_t;

typedef uint32_t writ_status_t;
enum {
    WRIT_OK,
    WRIT_HAS_ERRORS,
    WRIT_INVALID_ARGUMENT,
    WRIT_TOOL_FAILED,
    WRIT_IO_FAILED,
    WRIT_OUT_OF_MEMORY,
};

typedef struct {
    const char *data;
    size_t      length;
} writ_str_t;

typedef uint32_t writ_source_kind_t;
enum {
    WRIT_SOURCE_CONTRACT_FILE,
    WRIT_SOURCE_CONTRACT_DECLARATIONS,
    WRIT_SOURCE_PROFILE,
    WRIT_SOURCE_BINDING,
    WRIT_SOURCE_EMBEDDED,
};

typedef struct {
    writ_str_t host_path;
    uint32_t   first_line;
    uint32_t   first_column;
    uint64_t   first_byte;
} writ_origin_t;

typedef struct {
    uint32_t source_id;
    uint64_t start_byte;
    uint64_t end_byte;
    uint32_t line;
    uint32_t column;
} writ_span_t;

typedef uint32_t writ_severity_t;
enum {
    WRIT_ERROR,
    WRIT_WARNING,
    WRIT_NOTE,
};

typedef struct {
    writ_span_t span;
    writ_str_t  label;
} writ_diag_label_t;

typedef struct {
    writ_span_t span;
    writ_str_t  replacement;
    writ_str_t  description;
} writ_diag_suggestion_t;

typedef struct {
    size_t                        struct_size;
    writ_str_t                    code;
    writ_severity_t               severity;
    writ_str_t                    message;
    writ_str_t                    path;
    writ_diag_label_t             primary;
    size_t                        secondary_count;
    const writ_diag_label_t      *secondary;
    size_t                        note_count;
    const writ_diag_label_t      *notes;
    size_t                        suggestion_count;
    const writ_diag_suggestion_t *suggestions;
} writ_diag_t;

typedef void (*writ_diag_sink_fn)(void *context, const writ_diag_t *diag);

typedef struct {
    writ_status_t (*run)(void *context, const char *const *argv,
                         writ_str_t stdin_text, writ_str_t *stdout_text,
                         writ_str_t *stderr_text, int *exit_code);
    void *context;
} writ_process_runner_t;

typedef struct {
    size_t                struct_size;
    writ_process_runner_t process_runner;
    writ_str_t            llvm_prefix;
} writ_session_config_t;

typedef struct {
    writ_status_t (*write_file)(void *context, writ_str_t relative_path,
                                writ_str_t contents);
    void *context;
} writ_output_t;

uint32_t        writ_api_version(void);

writ_session_t *writ_session_new(const writ_session_config_t *config);
void            writ_session_free(writ_session_t *session);
writ_status_t   writ_session_add_contract_dir(writ_session_t *session, writ_str_t dir);

void              writ_session_set_diag_sink(writ_session_t *session,
                                             writ_diag_sink_fn sink, void *context);
size_t            writ_diag_count(const writ_session_t *session);
const writ_diag_t *writ_diag_at(const writ_session_t *session, size_t index);
writ_status_t     writ_diag_render(const writ_session_t *session, const writ_diag_t *diag,
                                   writ_str_t *rendered);

writ_unit_t  *writ_unit_new(writ_session_t *session);
writ_status_t writ_unit_add_source(writ_unit_t *unit, writ_source_kind_t kind,
                                   writ_origin_t origin, writ_str_t text,
                                   uint32_t *source_id);
writ_status_t writ_unit_add_file(writ_unit_t *unit, writ_str_t path, uint32_t *source_id);
writ_status_t writ_unit_add_embedded(writ_unit_t *unit, writ_str_t language,
                                     writ_origin_t origin, writ_str_t text,
                                     uint32_t *source_id);
size_t        writ_unit_file_count(const writ_unit_t *unit);
writ_str_t    writ_unit_file_at(const writ_unit_t *unit, size_t index);
writ_status_t writ_unit_dump(const writ_unit_t *unit, writ_str_t *rendered);
writ_status_t writ_format_unit(const writ_unit_t *unit, const writ_output_t *output);

writ_str_t    writ_platform_host_triple(void);
writ_status_t writ_platform_from_triple(writ_session_t *session, writ_str_t triple,
                                        writ_platform_t **platform);
writ_str_t    writ_platform_triple(const writ_platform_t *platform);
writ_status_t writ_platform_set_version(writ_platform_t *platform, writ_str_t library,
                                        writ_str_t version);
writ_status_t writ_platform_set_feature(writ_platform_t *platform, writ_str_t feature,
                                        bool enabled);

writ_status_t writ_resolve(writ_unit_t *unit, const writ_platform_t *platform,
                           const writ_model_t **model);
writ_status_t writ_derive_transport(writ_unit_t *unit, const writ_output_t *output);

writ_status_t writ_output_directory(writ_session_t *session, writ_str_t directory,
                                    writ_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
