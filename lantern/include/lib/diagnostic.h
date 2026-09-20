#pragma once

#include "lib/string.h"

typedef struct core_translation_unit_t core_translation_unit_t;
typedef struct core_diagnostics_t core_diagnostics_t;

typedef enum {
    CORE_DIAG_FATAL,
    CORE_DIAG_ERROR,
    CORE_DIAG_WARNING,
    CORE_DIAG_NOTE,
    CORE_DIAG_HELP,
} core_diag_severity_t;

typedef struct {
    core_diag_severity_t severity;
    core_string_t code;
    core_string_t message;
    core_string_t module;
    core_string_t path;
    void *user_data;
} core_diagnostic_t;

typedef bool (*core_diag_policy_fn)(void *context,
                                    core_diag_severity_t *severity,
                                    core_string_t code);

core_diagnostics_t *core_diagnostics_new(void);
void core_diagnostics_free(core_diagnostics_t *diagnostics);
void core_diagnostics_set_policy(core_diagnostics_t *diagnostics,
                                 core_diag_policy_fn policy, void *context);
bool core_diagnostics_report(core_diagnostics_t *diagnostics,
                             const core_translation_unit_t *unit,
                             core_diag_severity_t severity, core_string_t code,
                             core_string_t message);
bool core_diagnostic_set_message(core_diagnostics_t *diagnostics,
                                 size_t index,
                                 core_string_t message);
bool core_diagnostics_merge(core_diagnostics_t *destination,
                            const core_diagnostics_t *source);
size_t core_diagnostics_count(const core_diagnostics_t *diagnostics);
const core_diagnostic_t *core_diagnostics_at(
    const core_diagnostics_t *diagnostics, size_t index);
size_t core_diagnostics_count_severity(const core_diagnostics_t *diagnostics,
                                       core_diag_severity_t severity);
bool core_diagnostics_failed(const core_diagnostics_t *diagnostics);
char *core_diagnostics_render_text(const core_diagnostics_t *diagnostics);
