#include "diagnostics.h"

#include <stdarg.h>

#include "n00bb_version.h"

#include "lib/alloc.h"
#include "lib/buffer.h"

static const core_diag_code_t registry[] = {
#define N00B_DIAG(id, code, severity, title, explanation)                      \
    {CORE_STRING_STATIC(code), CORE_DIAG_##severity,                           \
     CORE_STRING_STATIC(title), CORE_STRING_STATIC(explanation)},
#include "diagnostics/registry.def"
#undef N00B_DIAG
};

static const core_text_form_t texts[] = {
#define N00B_TEXT(id, text) \
    {CORE_STRING_STATIC(text), CORE_STRING_STATIC(text)},
#define N00B_PLURAL(id, one, other) \
    {CORE_STRING_STATIC(one), CORE_STRING_STATIC(other)},
#include "diagnostics/messages.def"
#undef N00B_TEXT
#undef N00B_PLURAL
};

const core_text_catalog_t n00bb_catalog = {
    .forms = texts,
    .count = N00B_TEXT_COUNT,
    .list_separator = N00B_TEXT_LIST_SEPARATOR,
    .list_and = N00B_TEXT_LIST_LAST,
    .list_or = N00B_TEXT_LIST_OR,
    .quoted = N00B_TEXT_QUOTED_NAME,
    .clause_separator = N00B_TEXT_CLAUSE_SEPARATOR,
};

const core_diag_domain_t n00bb_domain = {
    .prefix = CORE_STRING_STATIC("N00B-"),
    .codes = registry,
    .count = N00B_DIAG_COUNT,
    .texts = &n00bb_catalog,
};

static const core_diag_code_t *
entry(n00bb_diag_t id)
{
    const core_diag_code_t *code = core_diag_domain_code(&n00bb_domain, id);
    return code ? code : &registry[N00B_DIAG_INTERNAL];
}

core_string_t
n00bb_diag_code(n00bb_diag_t id)
{
    return entry(id)->code;
}

core_diag_severity_t
n00bb_diag_severity(n00bb_diag_t id)
{
    return entry(id)->severity;
}

core_string_t
n00bb_diag_title(n00bb_diag_t id)
{
    return entry(id)->title;
}

core_string_t
n00bb_diag_explanation(n00bb_diag_t id)
{
    return entry(id)->explanation;
}

bool
n00bb_diag_lookup(core_string_t code, n00bb_diag_t *out)
{
    uint32_t id = 0;
    if (!core_diag_domain_lookup(&n00bb_domain, code, &id)) return false;
    if (out) *out = (n00bb_diag_t)id;
    return true;
}


core_string_t
n00bb_text_form(n00bb_text_t id)
{
    return core_text_form(&n00bb_catalog, id);
}

core_string_t
n00bb_text(n00bb_text_t id, n00bb_args_t args)
{
    return core_text_render(&n00bb_catalog, id, args);
}

n00bb_text_t
n00bb_scan_text(scan_diag_kind_t kind)
{
    static const n00bb_text_t texts[] = {
#define SCAN_DIAG(id, text) N00B_TEXT_SCAN_##id,
#include "scanner/scan_diags.def"
#undef SCAN_DIAG
    };
    return kind < SCAN_DIAG_COUNT ? texts[kind]
        : N00B_TEXT_SCAN_UNEXPECTED_CHARACTER;
}

core_string_t
n00bb_ir_text(void *state, ir_message_t message)
{
    static const n00bb_text_t texts[] = {
#define IR_TEXT(id, text) N00B_TEXT_IR_##id,
#include "ir/ir_texts.def"
#undef IR_TEXT
    };
    (void)state;
    return message.text < IR_TEXT_COUNT
        ? n00bb_text(texts[message.text], message.args)
        : (core_string_t){0};
}

const core_diag_words_t *
n00bb_diag_words(void)
{
    static core_diag_words_t words;
    words = (core_diag_words_t){
        .error = n00bb_text_form(N00B_TEXT_SEVERITY_ERROR),
        .warning = n00bb_text_form(N00B_TEXT_SEVERITY_WARNING),
        .note = n00bb_text_form(N00B_TEXT_SEVERITY_NOTE),
        .help = n00bb_text_form(N00B_TEXT_SEVERITY_HELP),
        .internal = n00bb_text_form(N00B_TEXT_SEVERITY_INTERNAL),
        .related = n00bb_text_form(N00B_TEXT_RELATED_LOCATION),
    };
    return &words;
}

bool
n00bb_report(core_diagnostics_t *d, const core_translation_unit_t *unit,
             n00bb_diag_t id, core_diag_location_t at, n00bb_text_t message,
             n00bb_args_t args)
{
    core_string_t text = n00bb_text(message, args);
    bool reported = core_diagnostics_report_at(d, unit,
        n00bb_diag_severity(id), n00bb_diag_code(id), text, at);
    core_free(text.data);
    return reported;
}

void
n00bb_report_label(core_diagnostics_t *d, n00bb_text_t id, n00bb_args_t args)
{
    core_string_t text = n00bb_text(id, args);
    core_diag_label(d, text);
    core_free(text.data);
}

void
n00bb_report_secondary(core_diagnostics_t *d,
                       const core_translation_unit_t *unit,
                       core_diag_location_t at, n00bb_text_t id,
                       n00bb_args_t args)
{
    core_string_t text = n00bb_text(id, args);
    core_diag_secondary(d, unit, at, text);
    core_free(text.data);
}

void
n00bb_report_note(core_diagnostics_t *d, n00bb_text_t id, n00bb_args_t args)
{
    core_string_t text = n00bb_text(id, args);
    core_diag_note(d, text);
    core_free(text.data);
}

void
n00bb_report_help(core_diagnostics_t *d, n00bb_text_t id, n00bb_args_t args)
{
    core_string_t text = n00bb_text(id, args);
    core_diag_help(d, text);
    core_free(text.data);
}

void
n00bb_report_fix(core_diagnostics_t *d, const core_translation_unit_t *unit,
                 const n00bb_edit_t *edits, size_t count)
{
    if (!d || !unit || !edits || !count) return;
    core_string_t source = core_source_file_contents(&unit->source);
    size_t size = core_source_file_size(&unit->source);
    size_t start = edits[0].start;
    size_t end = edits[count - 1].end;
    for (size_t i = 0; i < count; i++)
        if (edits[i].start > edits[i].end || edits[i].end > size
            || (i && edits[i].start < edits[i - 1].end)) return;
    while (start && source.data[start - 1] != '\n') start--;
    while (end < size && source.data[end] != '\n') end++;
    if (end > start && source.data[end - 1] == '\r') end--;
    core_buffer_t *text = core_buffer_empty();
    size_t at = start;
    for (size_t i = 0; i < count; i++) {
        core_buffer_append(text, source.data + at, edits[i].start - at);
        core_buffer_append_string(text, edits[i].text);
        at = edits[i].end;
    }
    core_buffer_append(text, source.data + at, end - at);
    core_string_t lines = core_buffer_take_string(text);
    core_diag_location_t span = core_diag_location_span(source, start, end);
    core_diag_fix(d, (core_diag_location_t){
        .line = span.line,
        .end_line = span.end_line,
        .has_location = true,
    }, lines);
    core_free(lines.data);
}

void
n00bb_internal_error(core_diagnostics_t *d,
                     const core_translation_unit_t *unit,
                     core_diag_location_t at, const char *stage,
                     const char *format, ...)
{
    core_buffer_t message = {0};
    va_list args;
    va_start(args, format);
    core_buffer_vprintf(&message, format, args);
    va_end(args);
    core_diagnostics_report_at(d, unit, CORE_DIAG_INTERNAL,
                               n00bb_diag_code(N00B_DIAG_INTERNAL),
                               core_buffer_string(&message), at);
    core_buffer_release(&message);
    if (stage && stage[0])
        n00bb_report_note(d, N00B_TEXT_INTERNAL_STAGE,
                          N00BB_ARGS(ARG_NATIVE(stage)));
    n00bb_report_note(d, n00bb_dirty ? N00B_TEXT_INTERNAL_VERSION_DIRTY
                                     : N00B_TEXT_INTERNAL_VERSION,
                      N00BB_ARGS(ARG_NATIVE(N00BB_VERSION),
                                 ARG_NATIVE(n00bb_build),
                                 ARG_NATIVE(n00bb_revision)));
    n00bb_report_help(d, N00B_TEXT_INTERNAL_REPORT, N00BB_NO_ARGS);
}

