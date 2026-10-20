#include "internal.h"

typedef struct {
    const char     *code;
    writ_severity_t severity;
    core_string_t   text;
} writ_diag_info_t;

static const writ_diag_info_t diag_info[] = {
#define WRIT_DIAG(id, code, severity, text) \
    {code, severity, {.data = (char *)("" text), .u8_bytes = sizeof(text) - 1}},
#include "diag_codes.def"
#undef WRIT_DIAG
};

static core_diag_severity_t
core_severity(writ_severity_t severity)
{
    return severity == WRIT_ERROR ? CORE_DIAG_ERROR : severity == WRIT_WARNING ? CORE_DIAG_WARNING : CORE_DIAG_NOTE;
}

uint32_t
writ_report(writ_session_t *session, writ_diag_code_t code, writ_loc_t loc, const char *format, ...)
{
    const writ_diag_info_t *info = &diag_info[code];
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append_string(buffer, info->text);
    if (format && *format) {
        va_list args;
        va_start(args, format);
        core_buffer_puts(buffer, ": ");
        core_buffer_vprintf(buffer, format, args);
        va_end(args);
    }
    writ_str_t message = writ_str_from_buffer(session, buffer);
    writ_span_t span = writ_loc_span(session, loc);
    size_t before = core_diagnostics_count(session->diagnostics);
    core_diagnostics_report_at(session->diagnostics, nullptr, core_severity(info->severity),
                               core_string_borrow(info->code, strlen(info->code)),
                               core_string_borrow(message.data, message.length),
                               (core_diag_location_t){span.line, span.column, span.line, span.column, true});
    if (core_diagnostics_count(session->diagnostics) == before) {
        return UINT32_MAX;
    }
    writ_diag_record_t record = {
        .code        = code,
        .message     = message,
        .primary     = loc,
        .secondary   = core_list_new(writ_diag_part_t),
        .suggestions = core_list_new(writ_diag_part_t),
    };
    core_list_push(session->diags, record);
    return (uint32_t)session->diags.len - 1;
}

void
writ_report_label(writ_session_t *session, uint32_t diag, writ_loc_t loc, writ_str_t label)
{
    if (diag < session->diags.len) {
        core_list_push(session->diags.data[diag].secondary, ((writ_diag_part_t){loc, label, {0}}));
    }
}

void
writ_report_suggest(writ_session_t *session, uint32_t diag, writ_loc_t loc, writ_str_t replacement,
                    writ_str_t description)
{
    if (diag < session->diags.len) {
        core_list_push(session->diags.data[diag].suggestions, ((writ_diag_part_t){loc, replacement, description}));
    }
}

bool
writ_session_failed(const writ_session_t *session)
{
    return core_diagnostics_failed(session->diagnostics);
}

static writ_diag_label_t *
labels_of(writ_session_t *session, const core_list_t(writ_diag_part_t) *parts)
{
    writ_diag_label_t *labels = writ_alloc(session, sizeof(writ_diag_label_t) * parts->len);
    for (size_t i = 0; i < parts->len; i++) {
        labels[i] = (writ_diag_label_t){writ_loc_span(session, parts->data[i].loc), parts->data[i].text};
    }
    return labels;
}

static const writ_diag_t *
build_view(writ_session_t *session, writ_diag_record_t *record)
{
    if (record->view) {
        return record->view;
    }
    const writ_diag_info_t *info = &diag_info[record->code];
    const writ_source_t *source = writ_source_at(session, record->primary.source);
    writ_diag_suggestion_t *suggestions =
        writ_alloc(session, sizeof(writ_diag_suggestion_t) * record->suggestions.len);
    for (size_t i = 0; i < record->suggestions.len; i++) {
        writ_diag_part_t *part = &record->suggestions.data[i];
        suggestions[i] = (writ_diag_suggestion_t){writ_loc_span(session, part->loc), part->text, part->extra};
    }
    writ_diag_t *view = writ_alloc(session, sizeof(writ_diag_t));
    *view = (writ_diag_t){
        .struct_size      = sizeof(writ_diag_t),
        .code             = writ_cstr(info->code),
        .severity         = info->severity,
        .message          = record->message,
        .path             = source ? (source->origin.host_path.length ? source->origin.host_path : source->path)
                                   : (writ_str_t){"", 0},
        .primary          = {writ_loc_span(session, record->primary), {"", 0}},
        .secondary_count  = record->secondary.len,
        .secondary        = labels_of(session, &record->secondary),
        .suggestion_count = record->suggestions.len,
        .suggestions      = suggestions,
    };
    record->view = view;
    return view;
}

void
writ_session_set_diag_sink(writ_session_t *session, writ_diag_sink_fn sink, void *context)
{
    session->sink         = sink;
    session->sink_context = context;
}

void
writ_diag_flush(writ_session_t *session, size_t from)
{
    if (!session->sink) {
        return;
    }
    for (size_t i = from; i < session->diags.len; i++) {
        session->sink(session->sink_context, build_view(session, &session->diags.data[i]));
    }
}

size_t
writ_diag_count(const writ_session_t *session)
{
    return session->diags.len;
}

const writ_diag_t *
writ_diag_at(const writ_session_t *session, size_t index)
{
    writ_session_t *mutable_session = (writ_session_t *)session;
    return index < session->diags.len ? build_view(mutable_session, &mutable_session->diags.data[index]) : nullptr;
}

static const char *
severity_name(writ_severity_t severity)
{
    return severity == WRIT_ERROR ? "error" : severity == WRIT_WARNING ? "warning" : "note";
}

writ_status_t
writ_diag_render(const writ_session_t *session, const writ_diag_t *diag, writ_str_t *rendered)
{
    if (!session || !diag || !rendered) {
        return WRIT_INVALID_ARGUMENT;
    }
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_printf(buffer, "%s[%.*s]: %.*s\n", severity_name(diag->severity), (int)diag->code.length,
                       diag->code.data, (int)diag->message.length, diag->message.data);
    if (diag->path.length) {
        core_buffer_printf(buffer, "  --> %.*s:%u:%u\n", (int)diag->path.length, diag->path.data,
                           diag->primary.span.line, diag->primary.span.column);
    }
    for (size_t i = 0; i < diag->secondary_count; i++) {
        core_buffer_printf(buffer, "  = %u:%u: %.*s\n", diag->secondary[i].span.line, diag->secondary[i].span.column,
                           (int)diag->secondary[i].label.length, diag->secondary[i].label.data);
    }
    for (size_t i = 0; i < diag->suggestion_count; i++) {
        core_buffer_printf(buffer, "  help: %.*s: `%.*s`\n", (int)diag->suggestions[i].description.length,
                           diag->suggestions[i].description.data, (int)diag->suggestions[i].replacement.length,
                           diag->suggestions[i].replacement.data);
    }
    *rendered = writ_str_from_buffer((writ_session_t *)session, buffer);
    return WRIT_OK;
}
