#include "internal.h"

#include "diag/domain.h"

#define CORE_SEVERITY_WRIT_ERROR   CORE_DIAG_ERROR
#define CORE_SEVERITY_WRIT_WARNING CORE_DIAG_WARNING
#define CORE_SEVERITY_WRIT_NOTE    CORE_DIAG_NOTE

static const core_diag_code_t codes[] = {
#define WRIT_DIAG(id, code, severity, text) \
    {CORE_STRING_STATIC(code), CORE_SEVERITY_##severity, CORE_STRING_STATIC(text), {}},
#include "writ/diag_codes.def"
#undef WRIT_DIAG
};

static const core_text_form_t forms[] = {
#define WRIT_TEXT(id, text) {CORE_STRING_STATIC(text), CORE_STRING_STATIC(text)},
#include "writ/diag_texts.def"
#undef WRIT_TEXT
};

static const core_text_catalog_t catalog = {
    .forms = forms,
    .count = WRIT_TEXT_COUNT,
};

static const core_diag_domain_t domain = {
    .codes = codes,
    .count = WRIT_D_COUNT,
    .texts = &catalog,
};

static writ_severity_t
writ_severity(core_diag_severity_t severity)
{
    return severity == CORE_DIAG_ERROR ? WRIT_ERROR : severity == CORE_DIAG_WARNING ? WRIT_WARNING : WRIT_NOTE;
}

static core_string_t
copy_string(writ_session_t *session, core_string_t text)
{
    return writ_core_string(writ_str_copy(session, text.data, text.u8_bytes));
}

static core_text_message_t
copy_message(writ_session_t *session, core_text_message_t message)
{
    core_text_arg_t *args = writ_alloc(session, sizeof(core_text_arg_t) * (message.args.count + 1));
    for (size_t i = 0; i < message.args.count; i++) {
        core_text_arg_t arg = message.args.at[i];
        arg.text = copy_string(session, arg.text);
        if (arg.items) {
            core_string_t *items = writ_alloc(session, sizeof(core_string_t) * (arg.count + 1));
            for (size_t j = 0; j < arg.count; j++) {
                items[j] = copy_string(session, arg.items[j]);
            }
            arg.items = items;
        }
        args[i] = arg;
    }
    return (core_text_message_t){message.text, {args, message.args.count}};
}

writ_str_t
writ_message_text(writ_session_t *session, core_text_message_t message)
{
    if (message.text >= WRIT_TEXT_COUNT) {
        return (writ_str_t){"", 0};
    }
    core_string_t text = core_text_render(domain.texts, message.text, message.args);
    writ_str_t copy = writ_str_copy(session, text.data, text.u8_bytes);
    core_free(text.data);
    return copy;
}

uint32_t
writ_report(writ_session_t *session, writ_diag_code_t code, writ_loc_t loc, core_text_message_t detail)
{
    const core_diag_code_t *info = core_diag_domain_code(&domain, code);
    core_text_message_t stored = copy_message(session, detail);
    writ_str_t message = stored.text < WRIT_TEXT_COUNT
                             ? writ_message_text(session, WRIT_MESSAGE(TITLE_DETAIL, CORE_TEXT_STRING(info->title),
                                                                       WRIT_ARG(writ_message_text(session, stored))))
                             : writ_str_copy(session, info->title.data, info->title.u8_bytes);
    writ_span_t span = writ_loc_span(session, loc);
    size_t before = core_diagnostics_count(session->diagnostics);
    core_diagnostics_report_at(session->diagnostics, nullptr, info->severity, info->code, writ_core_string(message),
                               (core_diag_location_t){span.line, span.column, span.line, span.column, true});
    if (core_diagnostics_count(session->diagnostics) == before) {
        return UINT32_MAX;
    }
    writ_diag_record_t record = {
        .code        = code,
        .message     = message,
        .detail      = stored,
        .primary     = loc,
        .secondary   = core_list_new(writ_diag_part_t),
        .suggestions = core_list_new(writ_diag_part_t),
    };
    core_list_push(session->diags, record);
    return (uint32_t)session->diags.len - 1;
}

void
writ_report_label(writ_session_t *session, uint32_t diag, writ_loc_t loc, core_text_message_t label)
{
    if (diag < session->diags.len) {
        core_text_message_t stored = copy_message(session, label);
        core_list_push(session->diags.data[diag].secondary,
                       ((writ_diag_part_t){loc, writ_message_text(session, stored), {0}, stored}));
    }
}

void
writ_report_suggest(writ_session_t *session, uint32_t diag, writ_loc_t loc, writ_str_t replacement,
                    core_text_message_t description)
{
    if (diag < session->diags.len) {
        core_text_message_t stored = copy_message(session, description);
        core_list_push(session->diags.data[diag].suggestions,
                       ((writ_diag_part_t){loc, replacement, writ_message_text(session, stored), stored}));
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
        labels[i] = (writ_diag_label_t){writ_loc_span(session, parts->data[i].loc), parts->data[i].text,
                                        parts->data[i].message};
    }
    return labels;
}

static const writ_diag_t *
build_view(writ_session_t *session, writ_diag_record_t *record)
{
    if (record->view) {
        return record->view;
    }
    const core_diag_code_t *info = core_diag_domain_code(&domain, record->code);
    const writ_source_t *source = writ_source_at(session, record->primary.source);
    writ_diag_suggestion_t *suggestions =
        writ_alloc(session, sizeof(writ_diag_suggestion_t) * record->suggestions.len);
    for (size_t i = 0; i < record->suggestions.len; i++) {
        writ_diag_part_t *part = &record->suggestions.data[i];
        suggestions[i] =
            (writ_diag_suggestion_t){writ_loc_span(session, part->loc), part->text, part->extra, part->message};
    }
    writ_diag_t *view = writ_alloc(session, sizeof(writ_diag_t));
    *view = (writ_diag_t){
        .struct_size      = sizeof(writ_diag_t),
        .code             = {info->code.data, info->code.u8_bytes},
        .code_id          = record->code,
        .severity         = writ_severity(info->severity),
        .message          = record->message,
        .detail           = record->detail,
        .path             = source ? (source->origin.host_path.length ? source->origin.host_path : source->path)
                                   : (writ_str_t){"", 0},
        .primary          = {writ_loc_span(session, record->primary), {"", 0}, WRIT_NO_DETAIL},
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

static core_string_t
severity_word(writ_severity_t severity)
{
    return core_text_form(&catalog, severity == WRIT_ERROR     ? WRIT_TEXT_SEVERITY_ERROR
                                    : severity == WRIT_WARNING ? WRIT_TEXT_SEVERITY_WARNING
                                                               : WRIT_TEXT_SEVERITY_NOTE);
}

writ_status_t
writ_diag_render(const writ_session_t *session, const writ_diag_t *diag, writ_str_t *rendered)
{
    if (!session || !diag || !rendered) {
        return WRIT_INVALID_ARGUMENT;
    }
    core_buffer_t *buffer = core_buffer_empty();
    core_buffer_append_string(buffer, severity_word(diag->severity));
    core_buffer_printf(buffer, "[%.*s]: %.*s\n", (int)diag->code.length, diag->code.data, (int)diag->message.length,
                       diag->message.data);
    if (diag->path.length) {
        core_buffer_printf(buffer, "  --> %.*s:%u:%u\n", (int)diag->path.length, diag->path.data,
                           diag->primary.span.line, diag->primary.span.column);
    }
    for (size_t i = 0; i < diag->secondary_count; i++) {
        core_buffer_printf(buffer, "  = %u:%u: %.*s\n", diag->secondary[i].span.line, diag->secondary[i].span.column,
                           (int)diag->secondary[i].label.length, diag->secondary[i].label.data);
    }
    core_string_t help = core_text_form(&catalog, WRIT_TEXT_HELP);
    for (size_t i = 0; i < diag->suggestion_count; i++) {
        core_buffer_printf(buffer, "  %.*s: %.*s: `%.*s`\n", (int)help.u8_bytes, help.data,
                           (int)diag->suggestions[i].description.length, diag->suggestions[i].description.data,
                           (int)diag->suggestions[i].replacement.length, diag->suggestions[i].replacement.data);
    }
    *rendered = writ_str_from_buffer((writ_session_t *)session, buffer);
    return WRIT_OK;
}
