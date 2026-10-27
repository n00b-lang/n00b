#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lib/list.h"
#include "lib/string.h"

typedef struct core_translation_unit_t core_translation_unit_t;
typedef struct core_diagnostics_t core_diagnostics_t;

typedef enum {
    CORE_DIAG_FATAL,
    CORE_DIAG_ERROR,
    CORE_DIAG_WARNING,
    CORE_DIAG_NOTE,
    CORE_DIAG_HELP,
    CORE_DIAG_INFO,
    CORE_DIAG_INTERNAL,
} core_diag_severity_t;

typedef struct {
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
    bool has_location;
} core_diag_location_t;

core_diag_location_t core_diag_location_span(core_string_t text, size_t start,
                                             size_t end);
bool core_diag_is_advisory(core_diag_severity_t severity);

typedef enum {
    CORE_DIAG_PART_SPAN,
    CORE_DIAG_PART_NOTE,
    CORE_DIAG_PART_HELP,
} core_diag_part_kind_t;

typedef struct {
    core_diag_part_kind_t kind;
    core_string_t text;
    core_string_t path;
    core_diag_location_t location;
} core_diag_part_t;

core_list_decl(core_diag_part_t);

typedef struct {
    core_diag_severity_t severity;
    core_string_t code;
    core_string_t message;
    core_string_t label;
    core_string_t module;
    core_string_t path;
    core_diag_location_t location;
    core_list_t(core_diag_part_t) parts;
    void *user_data;
} core_diagnostic_t;

typedef core_string_t (*core_diag_source_fn)(void *context,
                                             core_string_t path);

typedef struct {
    core_string_t error;
    core_string_t warning;
    core_string_t note;
    core_string_t help;
    core_string_t internal;
    core_string_t related;
} core_diag_words_t;

typedef struct {
    bool color;
    bool terse;
    bool hints;
    bool sorted;
    core_diag_source_fn source;
    void *source_context;
    const core_diag_words_t *words;
} core_diag_render_t;

typedef bool (*core_diag_policy_fn)(void *context,
                                    core_diag_severity_t *severity,
                                    core_string_t code);

core_diagnostics_t *core_diagnostics_new(void);
void core_diagnostics_free(core_diagnostics_t *diagnostics);
void core_diagnostics_set_unit(core_diagnostics_t *diagnostics,
                               const core_translation_unit_t *unit);
void core_diagnostics_set_policy(core_diagnostics_t *diagnostics,
                                 core_diag_policy_fn policy, void *context);
typedef bool (*core_diag_filter_fn)(void *context,
                                    const core_diagnostic_t *diagnostic);
void core_diagnostics_set_filter(core_diagnostics_t *diagnostics,
                                 core_diag_filter_fn filter, void *context);
bool core_diagnostics_report(core_diagnostics_t *diagnostics,
                             const core_translation_unit_t *unit,
                             core_diag_severity_t severity, core_string_t code,
                             core_string_t message);
bool core_diagnostics_report_at(core_diagnostics_t *diagnostics,
                                const core_translation_unit_t *unit,
                                core_diag_severity_t severity,
                                core_string_t code, core_string_t message,
                                core_diag_location_t location);
void core_diag_label(core_diagnostics_t *diagnostics, core_string_t text);
void core_diag_secondary(core_diagnostics_t *diagnostics,
                         const core_translation_unit_t *unit,
                         core_diag_location_t location, core_string_t text);
void core_diag_secondary_path(core_diagnostics_t *diagnostics,
                              core_string_t path,
                              core_diag_location_t location,
                              core_string_t text);
void core_diag_note(core_diagnostics_t *diagnostics, core_string_t text);
void core_diag_help(core_diagnostics_t *diagnostics, core_string_t text);
bool core_diagnostic_set_label(core_diagnostics_t *diagnostics, size_t index,
                               core_string_t text);
bool core_diagnostic_add_part(core_diagnostics_t *diagnostics, size_t index,
                              core_diag_part_kind_t kind, core_string_t path,
                              core_diag_location_t location,
                              core_string_t text);
bool core_diagnostic_set_message(core_diagnostics_t *diagnostics,
                                 size_t index,
                                 core_string_t message);
bool core_diagnostic_set_path(core_diagnostics_t *diagnostics, size_t index,
                              core_string_t path);
bool core_diagnostic_set_location(core_diagnostics_t *diagnostics,
                                  size_t index,
                                  core_diag_location_t location);
bool core_diagnostic_set_part_location(core_diagnostics_t *diagnostics,
                                       size_t index, size_t part,
                                       core_string_t path,
                                       core_diag_location_t location);
bool core_diagnostics_append(core_diagnostics_t *destination,
                             const core_diagnostic_t *diagnostic);
bool core_diagnostics_merge(core_diagnostics_t *destination,
                            const core_diagnostics_t *source);
size_t core_diagnostics_count(const core_diagnostics_t *diagnostics);
const core_diagnostic_t *core_diagnostics_at(
    const core_diagnostics_t *diagnostics, size_t index);
size_t core_diagnostics_count_severity(const core_diagnostics_t *diagnostics,
                                       core_diag_severity_t severity);
size_t core_diagnostics_failures(const core_diagnostics_t *diagnostics);
bool core_diagnostics_failed(const core_diagnostics_t *diagnostics);
bool core_diagnostics_has_fatal(const core_diagnostics_t *diagnostics);
core_string_t core_diagnostics_render(const core_diagnostics_t *diagnostics,
                                      core_diag_render_t options);
core_string_t core_diagnostics_render_json(
    const core_diagnostics_t *diagnostics);
