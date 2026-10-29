#include "diag/diagnostics.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lib/alloc.h"
#include "lib/buffer.h"
#include "lib/list.h"
#include "lib/module.h"
#include "lib/os.h"
#include "private/unicode/ucd.h"
#include "unicode/encoding.h"

core_list_decl(core_diagnostic_t);

struct core_diagnostics_t {
    core_list_t(core_diagnostic_t) items;
    core_diag_policy_fn policy;
    void *policy_context;
    core_diag_filter_fn filter;
    void *filter_context;
    const core_translation_unit_t *unit;
    size_t open;
    bool has_open;
};

static void
free_item(core_diagnostic_t *item)
{
    core_free(item->code.data);
    core_free(item->message.data);
    core_free(item->label.data);
    core_free(item->module.data);
    core_free(item->path.data);
    for (size_t i = 0; i < item->parts.len; i++) {
        core_free(item->parts.data[i].text.data);
        core_free(item->parts.data[i].path.data);
    }
    core_list_free(item->parts);
}

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
    for (size_t i = 0; i < d->items.len; i++) free_item(&d->items.data[i]);
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

static void
push_item(core_diagnostics_t *d, core_diagnostic_t item)
{
    core_list_push(d->items, item);
    d->open = d->items.len - 1;
    d->has_open = true;
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
    d->has_open = false;
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
    push_item(d, item);
    return true;
}

bool
core_diagnostic_add_part(core_diagnostics_t *d, size_t index,
                         core_diag_part_kind_t kind, core_string_t path,
                         core_diag_location_t location, core_string_t text)
{
    if (!d || index >= d->items.len) return false;
    core_list_push(d->items.data[index].parts, ((core_diag_part_t){
        .kind = kind,
        .text = text.data ? core_string_dup(text) : (core_string_t){},
        .path = path.data ? core_string_dup(path) : (core_string_t){},
        .location = location,
    }));
    return true;
}

bool
core_diagnostic_set_label(core_diagnostics_t *d, size_t index,
                          core_string_t text)
{
    if (!d || index >= d->items.len || !text.data) return false;
    core_diagnostic_t *item = &d->items.data[index];
    core_string_t copy = core_string_dup(text);
    core_free(item->label.data);
    item->label = copy;
    return true;
}

static void
add_part(core_diagnostics_t *d, core_diag_part_kind_t kind,
         core_string_t path, core_diag_location_t location,
         core_string_t text)
{
    if (d && d->has_open)
        core_diagnostic_add_part(d, d->open, kind, path, location, text);
}

void
core_diag_label(core_diagnostics_t *d, core_string_t text)
{
    if (d && d->has_open) core_diagnostic_set_label(d, d->open, text);
}

void
core_diag_secondary(core_diagnostics_t *d,
                    const core_translation_unit_t *unit,
                    core_diag_location_t location, core_string_t text)
{
    add_part(d, CORE_DIAG_PART_SPAN,
             unit ? unit->source.path : (core_string_t){}, location, text);
}

void
core_diag_secondary_path(core_diagnostics_t *d, core_string_t path,
                         core_diag_location_t location, core_string_t text)
{
    add_part(d, CORE_DIAG_PART_SPAN, path, location, text);
}

void
core_diag_note(core_diagnostics_t *d, core_string_t text)
{
    if (text.data)
        add_part(d, CORE_DIAG_PART_NOTE, (core_string_t){},
                 (core_diag_location_t){}, text);
}

void
core_diag_help(core_diagnostics_t *d, core_string_t text)
{
    if (text.data)
        add_part(d, CORE_DIAG_PART_HELP, (core_string_t){},
                 (core_diag_location_t){}, text);
}

void
core_diag_mark(core_diagnostics_t *d, core_diag_location_t location)
{
    add_part(d, CORE_DIAG_PART_MARK, (core_string_t){}, location,
             (core_string_t){});
}

void
core_diag_fix(core_diagnostics_t *d, core_diag_location_t location,
              core_string_t lines)
{
    if (lines.data)
        add_part(d, CORE_DIAG_PART_FIX, (core_string_t){}, location, lines);
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
core_diagnostic_set_part_location(core_diagnostics_t *d, size_t index,
                                  size_t part, core_string_t path,
                                  core_diag_location_t location)
{
    if (!d || index >= d->items.len || !path.data
        || part >= d->items.data[index].parts.len) return false;
    core_diag_part_t *item = &d->items.data[index].parts.data[part];
    core_string_t copy = core_string_dup(path);
    core_free(item->path.data);
    item->path = copy;
    item->location = location;
    return true;
}

static core_string_t
dup_or_empty(core_string_t s)
{
    return s.data ? core_string_dup(s) : (core_string_t){};
}

bool
core_diagnostics_append(core_diagnostics_t *dst, const core_diagnostic_t *from)
{
    if (!dst || !from) return false;
    dst->has_open = false;
    core_diag_severity_t severity = from->severity;
    if (dst->policy
        && !dst->policy(dst->policy_context, &severity, from->code))
        return true;
    core_diagnostic_t copy = {
        .severity = severity,
        .code = dup_or_empty(from->code),
        .message = dup_or_empty(from->message),
        .label = dup_or_empty(from->label),
        .module = dup_or_empty(from->module),
        .path = dup_or_empty(from->path),
        .location = from->location,
    };
    for (size_t i = 0; i < from->parts.len; i++) {
        const core_diag_part_t *part = &from->parts.data[i];
        core_list_push(copy.parts, ((core_diag_part_t){
            .kind = part->kind,
            .text = dup_or_empty(part->text),
            .path = dup_or_empty(part->path),
            .location = part->location,
        }));
    }
    push_item(dst, copy);
    return true;
}

bool
core_diagnostics_merge(core_diagnostics_t *dst, const core_diagnostics_t *src)
{
    if (!dst || !src) return false;
    for (size_t i = 0; i < src->items.len; i++)
        core_diagnostics_append(dst, &src->items.data[i]);
    dst->has_open = false;
    return true;
}

static void
encode_u32(core_buffer_t *out, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)value, (uint8_t)(value >> 8),
        (uint8_t)(value >> 16), (uint8_t)(value >> 24),
    };
    core_buffer_append(out, (const char *)bytes, sizeof(bytes));
}

static void
encode_text(core_buffer_t *out, core_string_t text)
{
    encode_u32(out, (uint32_t)text.u8_bytes);
    if (text.u8_bytes) core_buffer_append(out, text.data, text.u8_bytes);
}

static void
encode_location(core_buffer_t *out, core_diag_location_t location)
{
    encode_u32(out, location.line);
    encode_u32(out, location.column);
    encode_u32(out, location.end_line);
    encode_u32(out, location.end_column);
    encode_u32(out, location.has_location);
}

void
core_diagnostic_encode(core_buffer_t *out, const core_diagnostic_t *item)
{
    encode_u32(out, (uint32_t)item->severity);
    encode_text(out, item->code);
    encode_text(out, item->message);
    encode_text(out, item->label);
    encode_text(out, item->module);
    encode_text(out, item->path);
    encode_location(out, item->location);
    encode_u32(out, (uint32_t)item->parts.len);
    for (size_t p = 0; p < item->parts.len; p++) {
        const core_diag_part_t *part = &item->parts.data[p];
        encode_u32(out, (uint32_t)part->kind);
        encode_text(out, part->text);
        encode_text(out, part->path);
        encode_location(out, part->location);
    }
}

typedef struct {
    const uint8_t *data;
    size_t length;
    size_t *at;
    bool failed;
} decoder_t;

static uint32_t
decode_u32(decoder_t *d)
{
    if (d->failed || d->length - *d->at < 4) {
        d->failed = true;
        return 0;
    }
    const uint8_t *bytes = d->data + *d->at;
    *d->at += 4;
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static core_string_t
decode_text(decoder_t *d)
{
    uint32_t length = decode_u32(d);
    if (d->failed || d->length - *d->at < length) {
        d->failed = true;
        return (core_string_t){};
    }
    core_string_t text = length
        ? core_string_dup(core_string_borrow((const char *)d->data + *d->at,
                                             length))
        : (core_string_t){};
    *d->at += length;
    return text;
}

static core_diag_location_t
decode_location(decoder_t *d)
{
    core_diag_location_t location = {0};
    location.line = decode_u32(d);
    location.column = decode_u32(d);
    location.end_line = decode_u32(d);
    location.end_column = decode_u32(d);
    location.has_location = decode_u32(d) != 0;
    return location;
}

bool
core_diagnostic_decode(const uint8_t *data, size_t length, size_t *at,
                       core_diagnostic_t *out)
{
    if (!data || !at || !out || *at > length) return false;
    decoder_t d = {.data = data, .length = length, .at = at};
    *out = (core_diagnostic_t){
        .severity = (core_diag_severity_t)decode_u32(&d),
    };
    out->code = decode_text(&d);
    out->message = decode_text(&d);
    out->label = decode_text(&d);
    out->module = decode_text(&d);
    out->path = decode_text(&d);
    out->location = decode_location(&d);
    uint32_t parts = decode_u32(&d);
    for (uint32_t p = 0; !d.failed && p < parts; p++) {
        core_diag_part_t part = {
            .kind = (core_diag_part_kind_t)decode_u32(&d),
        };
        part.text = decode_text(&d);
        part.path = decode_text(&d);
        part.location = decode_location(&d);
        core_list_push(out->parts, part);
    }
    if (d.failed) core_diagnostic_release(out);
    return !d.failed;
}

void
core_diagnostic_release(core_diagnostic_t *item)
{
    if (!item) return;
    core_free(item->code.data);
    core_free(item->message.data);
    core_free(item->label.data);
    core_free(item->module.data);
    core_free(item->path.data);
    for (size_t p = 0; p < item->parts.len; p++) {
        core_free(item->parts.data[p].text.data);
        core_free(item->parts.data[p].path.data);
    }
    core_list_free(item->parts);
    *item = (core_diagnostic_t){};
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

size_t
core_diagnostics_failures(const core_diagnostics_t *d)
{
    return core_diagnostics_count_severity(d, CORE_DIAG_FATAL)
        + core_diagnostics_count_severity(d, CORE_DIAG_ERROR)
        + core_diagnostics_count_severity(d, CORE_DIAG_INTERNAL);
}

bool
core_diagnostics_failed(const core_diagnostics_t *d)
{
    return core_diagnostics_failures(d) > 0;
}

bool
core_diagnostics_has_fatal(const core_diagnostics_t *d)
{
    return core_diagnostics_count_severity(d, CORE_DIAG_FATAL)
        || core_diagnostics_count_severity(d, CORE_DIAG_INTERNAL);
}

typedef struct {
    core_string_t path;
    core_string_t text;
} rendered_source_t;

core_list_decl(rendered_source_t);

typedef struct {
    core_diag_render_t options;
    core_string_t cwd;
    core_list_t(rendered_source_t) sources;
} render_state_t;

typedef struct {
    uint32_t line;
    uint32_t start;
    uint32_t end;
    bool primary;
    core_string_t label;
} mark_t;

core_list_decl(mark_t);

#define TAB_WIDTH 4
#define STYLE_RESET "\x1b[0m"
#define STYLE_BOLD "\x1b[1m"
#define STYLE_GUTTER "\x1b[1;34m"

static core_string_t
read_source(core_string_t path)
{
    char *bytes = nullptr;
    size_t size = 0;
    if (!core_os_read_file(path, &bytes, &size)) return (core_string_t){};
    return (core_string_t){.data = bytes, .u8_bytes = size};
}

static core_string_t
cached_source(render_state_t *state, core_string_t path)
{
    for (size_t i = 0; i < state->sources.len; i++)
        if (core_string_eq(state->sources.data[i].path, path))
            return state->sources.data[i].text;
    core_string_t supplied = state->options.source
        ? state->options.source(state->options.source_context, path)
        : (core_string_t){};
    rendered_source_t source = {
        .path = core_string_dup(path),
        .text = supplied.data ? core_string_dup(supplied) : read_source(path),
    };
    core_list_push(state->sources, source);
    return source.text;
}

static size_t
next_codepoint(core_string_t text, size_t at, int32_t *cp_out)
{
    uint32_t next = (uint32_t)at;
    int32_t cp = unicode_utf8_decode(text.data, (uint32_t)text.u8_bytes,
                                     &next);
    *cp_out = cp;
    return cp < 0 ? at + 1 : next;
}

static core_string_t
source_line(core_string_t text, uint32_t line)
{
    if (!text.data || !line) return (core_string_t){};
    uint32_t current = 1;
    size_t start = 0;
    size_t at = 0;
    while (at < text.u8_bytes) {
        int32_t cp;
        size_t next = next_codepoint(text, at, &cp);
        if (cp >= 0 && core_codepoint_is_line_terminator((core_codepoint_t)cp)) {
            if (current == line) {
                core_string_t found = core_string_slice(text, start, at - start);
                while (found.u8_bytes && found.data[found.u8_bytes - 1] == '\r')
                    found.u8_bytes--;
                return found;
            }
            current++;
            start = next;
        }
        at = next;
    }
    return current == line && start < text.u8_bytes
        ? core_string_slice(text, start, text.u8_bytes - start)
        : (core_string_t){};
}

static uint32_t
codepoint_width(int32_t cp)
{
    if (cp < 0) return 1;
    if (cp == '\t') return TAB_WIDTH;
    if (cp < 0x20 || cp == 0x7f) return 1;
    return ucd_display_width((core_codepoint_t)cp);
}

static uint32_t
display_column(core_string_t line, uint32_t column)
{
    uint32_t width = 0;
    size_t at = 0;
    for (uint32_t col = 1; col < column; col++) {
        if (at >= line.u8_bytes) {
            width++;
            continue;
        }
        int32_t cp;
        at = next_codepoint(line, at, &cp);
        width += codepoint_width(cp);
    }
    return width;
}

static uint32_t
display_width(core_string_t line)
{
    uint32_t width = 0;
    size_t at = 0;
    while (at < line.u8_bytes) {
        int32_t cp;
        at = next_codepoint(line, at, &cp);
        width += codepoint_width(cp);
    }
    return width;
}

static uint32_t
first_text_column(core_string_t line)
{
    uint32_t width = 0;
    size_t at = 0;
    while (at < line.u8_bytes && (line.data[at] == ' ' || line.data[at] == '\t')) {
        width += line.data[at] == '\t' ? TAB_WIDTH : 1;
        at++;
    }
    return width;
}

static void
put_display_line(core_buffer_t *out, core_string_t line)
{
    size_t at = 0;
    while (at < line.u8_bytes) {
        int32_t cp;
        size_t next = next_codepoint(line, at, &cp);
        if (cp == '\t') {
            core_buffer_puts(out, "    ");
        } else if (cp >= 0 && (cp < 0x20 || cp == 0x7f)) {
            core_buffer_puts(out, " ");
        } else {
            core_buffer_append(out, line.data + at, next - at);
        }
        at = next;
    }
}

static const char *
severity_style(core_diag_severity_t severity)
{
    switch (severity) {
    case CORE_DIAG_FATAL:
    case CORE_DIAG_ERROR:
    case CORE_DIAG_INTERNAL: return "\x1b[1;31m";
    case CORE_DIAG_WARNING: return "\x1b[1;33m";
    case CORE_DIAG_NOTE:
    case CORE_DIAG_INFO: return "\x1b[1;36m";
    case CORE_DIAG_HELP: return "\x1b[1;32m";
    }
    return "\x1b[1;34m";
}

static const core_diag_words_t default_words = {
    .error = CORE_STRING_STATIC("error"),
    .warning = CORE_STRING_STATIC("warning"),
    .note = CORE_STRING_STATIC("note"),
    .help = CORE_STRING_STATIC("help"),
    .internal = CORE_STRING_STATIC("internal compiler error"),
    .related = CORE_STRING_STATIC("related location"),
};

static const core_diag_words_t *
words(const render_state_t *state)
{
    return state->options.words ? state->options.words : &default_words;
}

static core_string_t
severity_name(const render_state_t *state, core_diag_severity_t severity)
{
    switch (severity) {
    case CORE_DIAG_FATAL:
    case CORE_DIAG_ERROR: return words(state)->error;
    case CORE_DIAG_WARNING: return words(state)->warning;
    case CORE_DIAG_NOTE:
    case CORE_DIAG_INFO: return words(state)->note;
    case CORE_DIAG_HELP: return words(state)->help;
    case CORE_DIAG_INTERNAL: return words(state)->internal;
    }
    return words(state)->error;
}

static void
style(core_buffer_t *out, const render_state_t *state, const char *code)
{
    if (state->options.color) core_buffer_printf(out, "%s", code);
}

static core_string_t
display_path(const render_state_t *state, core_string_t path)
{
    if (state->cwd.u8_bytes && path.u8_bytes > state->cwd.u8_bytes + 1
        && core_string_starts_with(path, state->cwd)
        && path.data[state->cwd.u8_bytes] == '/')
        return core_string_slice(path, state->cwd.u8_bytes + 1,
                                 path.u8_bytes - state->cwd.u8_bytes - 1);
    return path;
}

static void
put_location(core_buffer_t *out, const render_state_t *state,
             core_string_t path, core_diag_location_t location)
{
    core_buffer_append_string(out, display_path(state, path));
    if (location.has_location && location.line)
        core_buffer_printf(out, ":%u:%u", location.line,
                           location.column ? location.column : 1);
}

static void
add_marks(core_list_t(mark_t) *marks, core_string_t text,
          core_diag_location_t location, bool primary, core_string_t label)
{
    if (!location.line) return;
    core_string_t line = source_line(text, location.line);
    uint32_t start = display_column(line, location.column ? location.column : 1);
    if (location.end_line > location.line) {
        core_string_t last = source_line(text, location.end_line);
        uint32_t line_end = display_width(line);
        core_list_push(*marks, ((mark_t){
            .line = location.line,
            .start = start,
            .end = line_end > start ? line_end : start + 1,
            .primary = primary,
        }));
        if (last.data) {
            uint32_t last_start = first_text_column(last);
            uint32_t last_end = display_column(last, location.end_column);
            core_list_push(*marks, ((mark_t){
                .line = location.end_line,
                .start = last_start,
                .end = last_end > last_start ? last_end : last_start + 1,
                .primary = primary,
                .label = label,
            }));
        }
        return;
    }
    uint32_t end = location.end_line == location.line
            && location.end_column > location.column
        ? display_column(line, location.end_column) : start + 1;
    core_list_push(*marks, ((mark_t){
        .line = location.line,
        .start = start,
        .end = end > start ? end : start + 1,
        .primary = primary,
        .label = label,
    }));
}

static uint32_t
digits(uint32_t value)
{
    uint32_t count = 1;
    while (value >= 10) {
        value /= 10;
        count++;
    }
    return count;
}

static void
put_gutter(core_buffer_t *out, const render_state_t *state, uint32_t width,
           uint32_t line)
{
    style(out, state, STYLE_GUTTER);
    if (line) core_buffer_printf(out, "    %*u |", (int)width, line);
    else core_buffer_printf(out, "    %*s |", (int)width, "");
    style(out, state, STYLE_RESET);
}

static void
put_spaces(core_buffer_t *out, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) core_buffer_puts(out, " ");
}

static const char *
mark_style(const mark_t *mark, core_diag_severity_t severity)
{
    return mark->primary ? severity_style(severity) : STYLE_GUTTER;
}

static void
render_snippet(core_buffer_t *out, render_state_t *state, core_string_t path,
               const core_list_t(mark_t) *marks, uint32_t width,
               core_diag_severity_t severity)
{
    core_string_t text = cached_source(state, path);
    core_buffer_puts(out, "\n");
    uint32_t previous = 0;
    for (size_t i = 0; i < marks->len;) {
        uint32_t line = marks->data[i].line;
        if (previous && line > previous + 1) {
            style(out, state, STYLE_GUTTER);
            core_buffer_puts(out, "    ");
            put_spaces(out, width);
            core_buffer_puts(out, " ...\n");
            style(out, state, STYLE_RESET);
        }
        put_gutter(out, state, width, line);
        core_string_t source = source_line(text, line);
        if (source.u8_bytes) {
            core_buffer_puts(out, " ");
            put_display_line(out, source);
        }
        core_buffer_puts(out, "\n");
        put_gutter(out, state, width, 0);
        core_buffer_puts(out, " ");
        uint32_t column = 0;
        const mark_t *labelled = nullptr;
        for (; i < marks->len && marks->data[i].line == line; i++) {
            const mark_t *mark = &marks->data[i];
            if (mark->start < column) continue;
            put_spaces(out, mark->start - column);
            style(out, state, mark_style(mark, severity));
            for (uint32_t c = mark->start; c < mark->end; c++)
                core_buffer_puts(out, "^");
            style(out, state, STYLE_RESET);
            column = mark->end;
            if (mark->label.u8_bytes) labelled = mark;
        }
        if (labelled) {
            style(out, state, mark_style(labelled, severity));
            core_buffer_puts(out, " ");
            core_buffer_append_string(out, labelled->label);
            style(out, state, STYLE_RESET);
        }
        core_buffer_puts(out, "\n");
        previous = line;
    }
}

static void
put_fix(core_buffer_t *out, const render_state_t *state, uint32_t width,
        const core_diag_part_t *part)
{
    uint32_t line = part->location.line;
    size_t start = 0;
    for (size_t at = 0; at <= part->text.u8_bytes; at++) {
        if (at < part->text.u8_bytes && part->text.data[at] != '\n') continue;
        put_gutter(out, state, width, line++);
        core_buffer_puts(out, " ");
        put_display_line(out, core_string_slice(part->text, start,
                                                at - start));
        core_buffer_puts(out, "\n");
        start = at + 1;
    }
}

static void
add_part_marks(core_list_t(mark_t) *marks, render_state_t *state,
               const core_diagnostic_t *item)
{
    core_string_t text = cached_source(state, item->path);
    for (size_t i = 0; i < item->parts.len; i++)
        if (item->parts.data[i].kind == CORE_DIAG_PART_MARK)
            add_marks(marks, text, item->parts.data[i].location, true,
                      (core_string_t){});
    for (size_t i = 1; i < marks->len; i++) {
        mark_t mark = marks->data[i];
        size_t j = i;
        for (; j > 0 && (marks->data[j - 1].line > mark.line
                         || (marks->data[j - 1].line == mark.line
                             && marks->data[j - 1].start > mark.start)); j--)
            marks->data[j] = marks->data[j - 1];
        marks->data[j] = mark;
    }
}

static uint32_t
span_width(core_diag_location_t location)
{
    uint32_t last = location.end_line > location.line ? location.end_line
                                                      : location.line;
    return digits(last);
}

static void
put_attached(core_buffer_t *out, const render_state_t *state,
             const core_diag_part_t *part)
{
    bool help = part->kind == CORE_DIAG_PART_HELP;
    core_buffer_puts(out, "  ");
    style(out, state, help ? severity_style(CORE_DIAG_HELP)
                           : severity_style(CORE_DIAG_NOTE));
    core_buffer_append_string(out, help ? words(state)->help
                                        : words(state)->note);
    style(out, state, STYLE_RESET);
    core_buffer_puts(out, ": ");
    core_string_t text = part->text;
    size_t start = 0;
    for (size_t at = 0; at <= text.u8_bytes; at++) {
        if (at < text.u8_bytes && text.data[at] != '\n') continue;
        if (start) {
            core_buffer_puts(out, "\n");
            put_spaces(out, 8);
        }
        core_buffer_append(out, text.data + start, at - start);
        start = at + 1;
    }
    core_buffer_puts(out, "\n");
}

static void
put_header(core_buffer_t *out, const render_state_t *state,
           const core_diagnostic_t *item)
{
    if (item->path.u8_bytes || item->module.u8_bytes) {
        style(out, state, STYLE_BOLD);
        if (item->path.u8_bytes)
            put_location(out, state, item->path, item->location);
        else
            core_buffer_append_string(out, item->module);
        core_buffer_puts(out, ": ");
        style(out, state, STYLE_RESET);
    }
    style(out, state, severity_style(item->severity));
    core_buffer_append_string(out, severity_name(state, item->severity));
    style(out, state, STYLE_RESET);
    style(out, state, STYLE_BOLD);
    core_buffer_puts(out, ": ");
    core_buffer_append_string(out, item->message);
    style(out, state, STYLE_RESET);
    core_buffer_puts(out, "\n");
}

static bool
collect_marks(core_list_t(mark_t) *marks, render_state_t *state,
              core_string_t path, core_diag_location_t location, bool primary,
              core_string_t label)
{
    marks->len = 0;
    if (!path.u8_bytes || !location.has_location) return false;
    core_string_t text = cached_source(state, path);
    if (!text.data) return false;
    add_marks(marks, text, location, primary, label);
    return marks->len != 0;
}

static void
render_default(core_buffer_t *out, render_state_t *state,
               const core_diagnostic_t *item)
{
    put_header(out, state, item);
    uint32_t width = item->location.has_location
        ? span_width(item->location) : 1;
    for (size_t i = 0; i < item->parts.len; i++) {
        const core_diag_part_t *part = &item->parts.data[i];
        if (part->kind == CORE_DIAG_PART_NOTE
            || part->kind == CORE_DIAG_PART_HELP
            || !part->location.has_location)
            continue;
        uint32_t w = span_width(part->location);
        if (w > width) width = w;
    }
    bool detailed = false;
    core_list_t(mark_t) marks = {};
    if (collect_marks(&marks, state, item->path, item->location, true,
                      item->label)) {
        add_part_marks(&marks, state, item);
        render_snippet(out, state, item->path, &marks, width, item->severity);
        detailed = true;
    }
    for (size_t i = 0; i < item->parts.len; i++) {
        const core_diag_part_t *part = &item->parts.data[i];
        if (part->kind != CORE_DIAG_PART_SPAN) continue;
        core_string_t path = part->path.u8_bytes ? part->path : item->path;
        bool shown = collect_marks(&marks, state, path, part->location, false,
                                   (core_string_t){});
        core_buffer_puts(out, "\n  ");
        core_buffer_append_string(out, part->text.u8_bytes
                                           ? part->text
                                           : words(state)->related);
        if (path.u8_bytes && (!shown || !core_string_eq(path, item->path))) {
            core_buffer_puts(out, " (");
            put_location(out, state, path, part->location);
            core_buffer_puts(out, ")");
        }
        if (shown) {
            core_buffer_puts(out, ":\n");
            render_snippet(out, state, path, &marks, width, CORE_DIAG_NOTE);
        } else {
            core_buffer_puts(out, "\n");
        }
        detailed = true;
    }
    core_list_free(marks);
    bool attached = false;
    for (size_t i = 0; i < item->parts.len; i++) {
        const core_diag_part_t *part = &item->parts.data[i];
        if (part->kind == CORE_DIAG_PART_SPAN
            || part->kind == CORE_DIAG_PART_MARK) continue;
        if (!attached && detailed) core_buffer_puts(out, "\n");
        attached = true;
        if (part->kind == CORE_DIAG_PART_FIX) put_fix(out, state, width, part);
        else put_attached(out, state, part);
    }
}

static void
render_terse(core_buffer_t *out, render_state_t *state,
             const core_diagnostic_t *item)
{
    if (item->path.u8_bytes) {
        put_location(out, state, item->path, item->location);
        core_buffer_puts(out, ": ");
    }
    core_buffer_append_string(out, severity_name(state, item->severity));
    if (item->code.u8_bytes) {
        core_buffer_puts(out, " ");
        core_buffer_append_string(out, item->code);
    }
    core_buffer_puts(out, ": ");
    core_buffer_append_string(out, item->message);
    core_buffer_puts(out, "\n");
    for (size_t i = 0; i < item->parts.len; i++) {
        const core_diag_part_t *part = &item->parts.data[i];
        if (part->kind == CORE_DIAG_PART_MARK) continue;
        if (part->kind == CORE_DIAG_PART_FIX) {
            put_fix(out, state, span_width(part->location), part);
            continue;
        }
        core_buffer_puts(out, "  ");
        if (part->kind == CORE_DIAG_PART_SPAN) {
            core_string_t path = part->path.u8_bytes ? part->path : item->path;
            if (path.u8_bytes) {
                put_location(out, state, path, part->location);
                core_buffer_puts(out, ": ");
            }
            core_buffer_append_string(out, words(state)->note);
            core_buffer_puts(out, ": ");
            core_buffer_append_string(out, part->text.u8_bytes
                                               ? part->text
                                               : words(state)->related);
        } else {
            core_buffer_append_string(out, part->kind == CORE_DIAG_PART_HELP
                                               ? words(state)->help
                                               : words(state)->note);
            core_buffer_puts(out, ": ");
            core_buffer_append_string(out, part->text);
        }
        core_buffer_puts(out, "\n");
    }
}

static void
state_init(render_state_t *state, core_diag_render_t options)
{
    *state = (render_state_t){
        .options = options,
        .cwd = core_os_current_directory(),
        .sources = core_list_new(rendered_source_t),
    };
}

static void
state_release(render_state_t *state)
{
    for (size_t i = 0; i < state->sources.len; i++) {
        core_free(state->sources.data[i].path.data);
        core_free(state->sources.data[i].text.data);
    }
    core_list_free(state->sources);
    core_free(state->cwd.data);
}

static void
render_item(core_buffer_t *out, render_state_t *state,
            const core_diagnostic_t *item)
{
    if (state->options.terse) render_terse(out, state, item);
    else render_default(out, state, item);
}

static bool
renders_before(const core_diagnostic_t *a, const core_diagnostic_t *b)
{
    bool a_located = a->path.u8_bytes && a->location.has_location;
    bool b_located = b->path.u8_bytes && b->location.has_location;
    if (a_located != b_located) return a_located;
    if (!a_located) return false;
    int path = core_string_cmp(a->path, b->path);
    if (path) return path < 0;
    if (a->location.line != b->location.line)
        return a->location.line < b->location.line;
    if (a->location.column != b->location.column)
        return a->location.column < b->location.column;
    if (a->location.end_line != b->location.end_line)
        return a->location.end_line < b->location.end_line;
    if (a->location.end_column != b->location.end_column)
        return a->location.end_column < b->location.end_column;
    if (a->severity != b->severity) return a->severity < b->severity;
    int order = core_string_cmp(a->code, b->code);
    if (!order) order = core_string_cmp(a->message, b->message);
    if (!order) order = core_string_cmp(a->label, b->label);
    return order < 0;
}

static size_t *
render_order(const core_diagnostics_t *d, bool sorted)
{
    size_t *order = core_alloc_array(size_t, d->items.len + 1);
    size_t *anchor = core_alloc_array(size_t, d->items.len + 1);
    if (!order || !anchor) {
        core_free(order);
        core_free(anchor);
        return nullptr;
    }
    for (size_t i = 0; i < d->items.len; i++) {
        core_diag_severity_t severity = d->items.data[i].severity;
        anchor[i] = i && (severity == CORE_DIAG_NOTE
                          || severity == CORE_DIAG_HELP)
            ? anchor[i - 1] : i;
        size_t at = i;
        while (sorted && at
               && renders_before(&d->items.data[anchor[i]],
                                 &d->items.data[anchor[order[at - 1]]])) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    core_free(anchor);
    return order;
}

core_string_t
core_diagnostics_render(const core_diagnostics_t *d, core_diag_render_t options)
{
    if (!d) return (core_string_t){};
    size_t *order = render_order(d, options.sorted);
    if (!order) return (core_string_t){};
    core_buffer_t *out = core_buffer_empty();
    render_state_t state;
    state_init(&state, options);
    bool first = true;
    for (size_t i = 0; i < d->items.len; i++) {
        const core_diagnostic_t *item = &d->items.data[order[i]];
        if (d->filter && !d->filter(d->filter_context, item)) continue;
        if (item->severity == CORE_DIAG_INFO && !options.hints) continue;
        if (!first && !options.terse) core_buffer_puts(out, "\n");
        first = false;
        render_item(out, &state, item);
    }
    state_release(&state);
    core_free(order);
    return core_buffer_take_string(out);
}

static void
json_text(core_buffer_t *out, core_string_t text)
{
    if (text.data) core_buffer_append_quoted(out, text);
    else core_buffer_puts(out, "null");
}

static void
json_location(core_buffer_t *out, core_diag_location_t location)
{
    if (!location.has_location) {
        core_buffer_puts(out, "null");
        return;
    }
    core_buffer_printf(out, "{\"line\":%u,\"column\":%u,\"end_line\":%u,"
                       "\"end_column\":%u}", location.line, location.column,
                       location.end_line, location.end_column);
}

core_string_t
core_diagnostics_render_json(const core_diagnostics_t *d)
{
    static const core_string_t severities[] = {
        [CORE_DIAG_FATAL] = CORE_STRING_STATIC("fatal"),
        [CORE_DIAG_ERROR] = CORE_STRING_STATIC("error"),
        [CORE_DIAG_WARNING] = CORE_STRING_STATIC("warning"),
        [CORE_DIAG_NOTE] = CORE_STRING_STATIC("note"),
        [CORE_DIAG_HELP] = CORE_STRING_STATIC("help"),
        [CORE_DIAG_INFO] = CORE_STRING_STATIC("info"),
        [CORE_DIAG_INTERNAL] = CORE_STRING_STATIC("internal"),
    };
    static const core_string_t kinds[] = {
        [CORE_DIAG_PART_SPAN] = CORE_STRING_STATIC("span"),
        [CORE_DIAG_PART_NOTE] = CORE_STRING_STATIC("note"),
        [CORE_DIAG_PART_HELP] = CORE_STRING_STATIC("help"),
        [CORE_DIAG_PART_MARK] = CORE_STRING_STATIC("mark"),
        [CORE_DIAG_PART_FIX] = CORE_STRING_STATIC("fix"),
    };
    size_t *order = d ? render_order(d, true) : nullptr;
    core_buffer_t *out = core_buffer_empty();
    core_buffer_puts(out, "[");
    for (size_t i = 0; order && i < d->items.len; i++) {
        const core_diagnostic_t *item = &d->items.data[order[i]];
        if (i) core_buffer_puts(out, ",");
        core_buffer_puts(out, "\n  {\"severity\":");
        core_buffer_append_quoted(out, severities[item->severity]);
        core_buffer_puts(out, ",\"code\":");
        json_text(out, item->code);
        core_buffer_puts(out, ",\"message\":");
        json_text(out, item->message);
        core_buffer_puts(out, ",\"path\":");
        json_text(out, item->path.u8_bytes ? item->path : (core_string_t){});
        core_buffer_puts(out, ",\"location\":");
        json_location(out, item->location);
        core_buffer_puts(out, ",\"label\":");
        json_text(out, item->label.u8_bytes ? item->label : (core_string_t){});
        core_buffer_puts(out, ",\"parts\":[");
        for (size_t p = 0; p < item->parts.len; p++) {
            const core_diag_part_t *part = &item->parts.data[p];
            if (p) core_buffer_puts(out, ",");
            core_buffer_puts(out, "{\"kind\":");
            core_buffer_append_quoted(out, kinds[part->kind]);
            core_buffer_puts(out, ",\"text\":");
            json_text(out, part->text);
            core_buffer_puts(out, ",\"path\":");
            json_text(out, part->path.u8_bytes ? part->path : (core_string_t){});
            core_buffer_puts(out, ",\"location\":");
            json_location(out, part->location);
            core_buffer_puts(out, "}");
        }
        core_buffer_puts(out, "]}");
    }
    core_buffer_puts(out, "\n]\n");
    core_free(order);
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
