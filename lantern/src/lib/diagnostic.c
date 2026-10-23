#include "lib/diagnostic.h"

#include <string.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/module.h"

core_list_decl(core_diagnostic_t);

struct core_diagnostics_t {
    core_list_t(core_diagnostic_t) items;
    core_diag_policy_fn policy;
    void *policy_context;
    core_diag_filter_fn filter;
    void *filter_context;
    const core_translation_unit_t *unit;
};

core_diagnostics_t *
core_diagnostics_new(void)
{
    core_diagnostics_t *d = core_alloc(core_diagnostics_t);
    d->items = core_list_new(core_diagnostic_t);
    return d;
}

void
core_diagnostics_free(core_diagnostics_t *d)
{
    if (!d) return;
    for (size_t i = 0; i < d->items.len; i++) {
        core_diagnostic_t *item = &d->items.data[i];
        core_free(item->code.data);
        core_free(item->message.data);
        core_free(item->module.data);
        core_free(item->path.data);
    }
    core_list_free(d->items);
    core_free(d);
}

void
core_diagnostics_set_unit(core_diagnostics_t *d,
                          const core_translation_unit_t *unit)
{
    if (d) d->unit = unit;
}

void
core_diagnostics_set_policy(core_diagnostics_t *d, core_diag_policy_fn policy,
                            void *context)
{
    if (!d) return;
    d->policy = policy;
    d->policy_context = context;
}

void
core_diagnostics_set_filter(core_diagnostics_t *d, core_diag_filter_fn filter,
                            void *context)
{
    if (!d) return;
    d->filter = filter;
    d->filter_context = context;
}

bool
core_diagnostics_report(core_diagnostics_t *d,
                        const core_translation_unit_t *unit,
                        core_diag_severity_t severity, core_string_t code,
                        core_string_t message)
{
    return core_diagnostics_report_at(d, unit, severity, code, message,
                                     (core_diag_location_t){0});
}

bool
core_diagnostics_report_at(core_diagnostics_t *d,
                           const core_translation_unit_t *unit,
                           core_diag_severity_t severity, core_string_t code,
                           core_string_t message,
                           core_diag_location_t location)
{
    if (!d || !code.data || !message.data) return false;
    if (d->policy && !d->policy(d->policy_context, &severity, code))
        return true;
    core_diagnostic_t item = {
        .severity = severity,
        .code = core_string_dup(code),
        .message = core_string_dup(message),
        .location = location,
    };
    const core_translation_unit_t *owner = unit ? unit : d->unit;
    if (owner) {
        item.module = core_string_dup(owner->module.identifier);
        item.path = core_string_dup(owner->source.path);
    }
    core_list_push(d->items, item);
    return true;
}

bool
core_diagnostic_set_message(core_diagnostics_t *d, size_t index,
                            core_string_t message)
{
    if (!d || index >= d->items.len || !message.data) return false;
    core_diagnostic_t *item = &d->items.data[index];
    core_string_t copy = core_string_dup(message);
    core_free(item->message.data);
    item->message = copy;
    return true;
}

bool
core_diagnostic_set_path(core_diagnostics_t *d, size_t index,
                         core_string_t path)
{
    if (!d || index >= d->items.len || !path.data) return false;
    core_diagnostic_t *item = &d->items.data[index];
    core_string_t copy = core_string_dup(path);
    core_free(item->path.data);
    item->path = copy;
    return true;
}

bool
core_diagnostic_set_location(core_diagnostics_t *d, size_t index,
                             core_diag_location_t location)
{
    if (!d || index >= d->items.len) return false;
    d->items.data[index].location = location;
    return true;
}

bool
core_diagnostics_append(core_diagnostics_t *dst, const core_diagnostic_t *from)
{
    if (!dst || !from) return false;
    core_diag_severity_t severity = from->severity;
    if (dst->policy
        && !dst->policy(dst->policy_context, &severity, from->code))
        return true;
    core_diagnostic_t copy = *from;
    copy.severity = severity;
    copy.code = core_string_dup(from->code);
    copy.message = core_string_dup(from->message);
    copy.module = core_string_dup(from->module);
    copy.path = core_string_dup(from->path);
    copy.user_data = nullptr;
    core_list_push(dst->items, copy);
    return true;
}

bool
core_diagnostics_merge(core_diagnostics_t *dst, const core_diagnostics_t *src)
{
    if (!dst || !src) return false;
    for (size_t i = 0; i < src->items.len; i++)
        core_diagnostics_append(dst, &src->items.data[i]);
    return true;
}

size_t
core_diagnostics_count(const core_diagnostics_t *d)
{
    return d ? d->items.len : 0;
}

const core_diagnostic_t *
core_diagnostics_at(const core_diagnostics_t *d, size_t index)
{
    return d && index < d->items.len ? &d->items.data[index] : nullptr;
}

size_t
core_diagnostics_count_severity(const core_diagnostics_t *d,
                                core_diag_severity_t severity)
{
    size_t count = 0;
    if (d) for (size_t i = 0; i < d->items.len; i++)
        count += d->items.data[i].severity == severity;
    return count;
}

bool
core_diagnostics_failed(const core_diagnostics_t *d)
{
    return core_diagnostics_count_severity(d, CORE_DIAG_FATAL)
        || core_diagnostics_count_severity(d, CORE_DIAG_ERROR);
}

bool
core_diagnostics_has_fatal(const core_diagnostics_t *d)
{
    return core_diagnostics_count_severity(d, CORE_DIAG_FATAL) != 0;
}

core_string_t
core_diagnostics_render_text(const core_diagnostics_t *d)
{
    if (!d) return (core_string_t){};
    core_buffer_t *out = core_buffer_empty();
    static const core_string_t names[] = {
        CORE_STRING_STATIC("fatal"), CORE_STRING_STATIC("error"),
        CORE_STRING_STATIC("warning"), CORE_STRING_STATIC("note"),
        CORE_STRING_STATIC("help"), CORE_STRING_STATIC("info"),
    };
    for (size_t i = 0; i < d->items.len; i++) {
        const core_diagnostic_t *item = &d->items.data[i];
        if (d->filter && !d->filter(d->filter_context, item)) continue;
        core_string_t severity = item->severity <= CORE_DIAG_INFO
            ? names[item->severity] : CORE_STRING_STATIC("diagnostic");
        core_buffer_printf(out, "%.*s[%.*s]: %.*s\n",
            (int)severity.u8_bytes, severity.data,
            (int)item->code.u8_bytes, item->code.data,
            (int)item->message.u8_bytes, item->message.data);
        if (item->module.data)
            core_buffer_printf(out, "  --> module %.*s\n",
                (int)item->module.u8_bytes, item->module.data);
        if (item->path.data && item->location.has_location)
            core_buffer_printf(out, "  --> %.*s:%u:%u\n",
                (int)item->path.u8_bytes, item->path.data,
                item->location.line, item->location.column);
        else if (item->path.data)
            core_buffer_printf(out, "  --> %.*s\n",
                (int)item->path.u8_bytes, item->path.data);
    }
    return core_buffer_take_string(out);
}

bool
core_diag_is_advisory(core_diag_severity_t severity)
{
    return severity == CORE_DIAG_WARNING || severity == CORE_DIAG_INFO;
}

core_diag_location_t
core_diag_location_span(core_string_t text, size_t start, size_t end)
{
    core_text_position_t first = core_string_position_at(text, start);
    core_text_position_t last = core_string_position_at(text,
                                                        end > start ? end : start);

    return (core_diag_location_t){
        .line = first.line,
        .column = first.column,
        .end_line = last.line,
        .end_column = last.column,
        .has_location = true,
    };
}
