#include "../model/model.h"
#include "model_names.h"

enum {
    MAX_DEPTH = 32,
};

typedef struct {
    core_buffer_t *out;
    int            depth;
    bool           empty[MAX_DEPTH];
    bool           keyed;
} json_t;

static void
json_newline(json_t *json)
{
    core_buffer_putc(json->out, '\n');
    for (int i = 0; i < json->depth; i++) {
        core_buffer_puts(json->out, "  ");
    }
}

static void
json_value(json_t *json)
{
    if (json->keyed) {
        json->keyed = false;
        return;
    }
    if (json->depth) {
        core_buffer_puts(json->out, json->empty[json->depth] ? "" : ",");
        json->empty[json->depth] = false;
        json_newline(json);
    }
}

static void
json_open(json_t *json, char bracket)
{
    json_value(json);
    core_buffer_putc(json->out, bracket);
    json->depth++;
    json->empty[json->depth] = true;
}

static void
json_close(json_t *json, char bracket)
{
    bool empty = json->empty[json->depth];
    json->depth--;
    if (!empty) {
        json_newline(json);
    }
    core_buffer_putc(json->out, bracket);
}

static void
json_quoted(json_t *json, writ_str_t text)
{
    core_buffer_putc(json->out, '"');
    for (size_t i = 0; i < text.length; i++) {
        unsigned char c = (unsigned char)text.data[i];
        if (c == '"' || c == '\\') {
            core_buffer_printf(json->out, "\\%c", c);
        } else if (c < 0x20) {
            core_buffer_printf(json->out, "\\u%04x", c);
        } else {
            core_buffer_putc(json->out, (char)c);
        }
    }
    core_buffer_putc(json->out, '"');
}

static void
json_key(json_t *json, const char *key)
{
    json_value(json);
    json_quoted(json, writ_cstr(key));
    core_buffer_puts(json->out, ": ");
    json->keyed = true;
}

static void
json_str(json_t *json, writ_str_t text)
{
    json_value(json);
    json_quoted(json, text);
}

static void
json_null(json_t *json)
{
    json_value(json);
    core_buffer_puts(json->out, "null");
}

static void
json_bool(json_t *json, bool value)
{
    json_value(json);
    core_buffer_puts(json->out, value ? "true" : "false");
}

static void
json_int(json_t *json, int64_t value)
{
    json_value(json);
    core_buffer_printf(json->out, "%lld", (long long)value);
}

static void
json_uint(json_t *json, uint32_t value)
{
    if (value == WRIT_NONE) {
        json_null(json);
        return;
    }
    json_value(json);
    core_buffer_printf(json->out, "%u", value);
}

static void
json_enum(json_t *json, const char *const *names, size_t count, uint32_t value)
{
    json_str(json, writ_cstr(value < count ? names[value] : "?"));
}

static void
json_span(json_t *json, writ_span_t span)
{
    json_value(json);
    core_buffer_printf(json->out, "\"%u:%u\"", span.line, span.column);
}

static void
json_type(json_t *json, const writ_model_t *model, const writ_type_view_t *type)
{
    if (!type) {
        json_null(json);
        return;
    }
    json_uint(json, (uint32_t)(type - model->type_views) - 1);
}

static void
json_expr(json_t *json, const writ_model_t *model, uint32_t expr)
{
    if (!expr || expr == WRIT_NONE) {
        json_null(json);
        return;
    }
    json_str(json, writ_loc_text(model->session, writ_node(model->unit, expr)->loc));
}

#include "model_json.h"

writ_status_t
writ_lower_json(const writ_model_t *model, const writ_output_t *output)
{
    if (!model || !output || !output->write_file) {
        return WRIT_INVALID_ARGUMENT;
    }
    json_t json = {.out = core_buffer_empty()};
#define JSON_LIST(key, list, writer, views, first)                                                                     \
    json_key(&json, key);                                                                                              \
    json_open(&json, '[');                                                                                             \
    for (size_t i = first; i < model->list.len; i++) {                                                                 \
        writer(&json, model, &model->views[i]);                                                                        \
    }                                                                                                                  \
    json_close(&json, ']');
    json_open(&json, '{');
    JSON_LIST("channels", channels, json_view_channel, channel_views, 0)
    JSON_LIST("consts", consts, json_view_const, const_views, 0)
    JSON_LIST("enums", enums, json_view_enum, enum_views, 0)
    JSON_LIST("fns", fns, json_view_fn, fn_views, 0)
    JSON_LIST("handles", handles, json_view_handle, handle_views, 0)
    json_key(&json, "library");
    json_view_library(&json, model, model->library_view);
    JSON_LIST("records", records, json_view_record, record_views, 0)
    JSON_LIST("types", types, json_view_type, type_views, 1)
#undef JSON_LIST
    json_close(&json, '}');
    core_buffer_putc(json.out, '\n');
    writ_str_t text = writ_str_from_buffer(model->session, json.out);
    return output->write_file(output->context, writ_str_format(model->session, "%.*s.json", S(model->library_view->name)),
                              text);
}
