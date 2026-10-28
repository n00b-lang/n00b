#pragma once

#include "diag/domain.h"
#include "ir/diag.h"
#include "scanner/scanner.h"
#include "lib/module.h"

typedef enum {
#define N00B_DIAG(id, code, severity, title, explanation) N00B_DIAG_##id,
#include "diagnostics/registry.def"
#undef N00B_DIAG
    N00B_DIAG_COUNT,
} n00bb_diag_t;

typedef enum {
#define N00B_TEXT(id, text) N00B_TEXT_##id,
#define N00B_PLURAL(id, one, other) N00B_TEXT_##id,
#include "diagnostics/messages.def"
#undef N00B_TEXT
#undef N00B_PLURAL
    N00B_TEXT_COUNT,
} n00bb_text_t;

typedef core_text_arg_t n00bb_arg_t;
typedef core_text_args_t n00bb_args_t;

#define ARG_TEXT(value) CORE_TEXT_STRING(value)
#define ARG_NUMBER(value) CORE_TEXT_NUMBER(value)
#define ARG_ERRNO(value) CORE_TEXT_ERRNO(value)
#define ARG_PHRASE(id) CORE_TEXT_PHRASE(N00B_TEXT_##id)
#define ARG_PHRASE_ID(value) CORE_TEXT_PHRASE(value)
#define ARG_NATIVE(value) CORE_TEXT_NATIVE(value)
#define ARG_CHARACTER(value) CORE_TEXT_CHARACTER(value)
#define ARG_CODEPOINT(value) CORE_TEXT_CODEPOINT(value)
#define ARG_NAMES(items, total) \
    CORE_TEXT_ITEMS(CORE_TEXT_ARG_NAMES, items, total)
#define ARG_CHOICES(items, total) \
    CORE_TEXT_ITEMS(CORE_TEXT_ARG_CHOICES, items, total)
#define ARG_LIST(items, total) \
    CORE_TEXT_ITEMS(CORE_TEXT_ARG_LIST, items, total)
#define ARG_ALTERNATIVES(items, total) \
    CORE_TEXT_ITEMS(CORE_TEXT_ARG_ALTERNATIVES, items, total)
#define ARG_CLAUSES(items, total) \
    CORE_TEXT_ITEMS(CORE_TEXT_ARG_CLAUSES, items, total)
#define N00BB_ARGS(...) CORE_TEXT_ARGS(__VA_ARGS__)
#define N00BB_NO_ARGS CORE_TEXT_NO_ARGS

extern const core_text_catalog_t n00bb_catalog;
extern const core_diag_domain_t n00bb_domain;

core_string_t n00bb_text(n00bb_text_t id, n00bb_args_t args);
core_string_t n00bb_text_form(n00bb_text_t id);
const core_diag_words_t *n00bb_diag_words(void);
n00bb_text_t n00bb_scan_text(scan_diag_kind_t kind);
core_string_t n00bb_ir_text(void *state, ir_message_t message);
bool n00bb_report(core_diagnostics_t *diagnostics,
                  const core_translation_unit_t *unit, n00bb_diag_t id,
                  core_diag_location_t at, n00bb_text_t message,
                  n00bb_args_t args);
void n00bb_report_label(core_diagnostics_t *diagnostics, n00bb_text_t text,
                        n00bb_args_t args);
void n00bb_report_secondary(core_diagnostics_t *diagnostics,
                            const core_translation_unit_t *unit,
                            core_diag_location_t at, n00bb_text_t text,
                            n00bb_args_t args);
void n00bb_report_note(core_diagnostics_t *diagnostics, n00bb_text_t text,
                       n00bb_args_t args);
void n00bb_report_help(core_diagnostics_t *diagnostics, n00bb_text_t text,
                       n00bb_args_t args);

typedef struct {
    size_t start;
    size_t end;
    core_string_t text;
} n00bb_edit_t;

void n00bb_report_fix(core_diagnostics_t *diagnostics,
                      const core_translation_unit_t *unit,
                      const n00bb_edit_t *edits, size_t count);

core_string_t n00bb_diag_code(n00bb_diag_t id);
core_diag_severity_t n00bb_diag_severity(n00bb_diag_t id);
core_string_t n00bb_diag_title(n00bb_diag_t id);
core_string_t n00bb_diag_explanation(n00bb_diag_t id);
bool n00bb_diag_lookup(core_string_t code, n00bb_diag_t *out);

bool n00bb_diag_did_you_mean(core_diagnostics_t *diagnostics,
                             core_string_t name,
                             const core_string_t *candidates, size_t count);
size_t n00bb_closest_names(core_string_t name, const core_string_t *candidates,
                           size_t count, size_t *best, size_t best_capacity);
[[gnu::format(printf, 5, 6)]] void
n00bb_internal_error(core_diagnostics_t *diagnostics,
                     const core_translation_unit_t *unit,
                     core_diag_location_t at, const char *stage,
                     const char *format, ...);
int n00bb_explain_command(core_string_t code);
void n00bb_install_crash_handler(void);
