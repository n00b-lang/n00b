#include <stdio.h>
#include <stdlib.h>

#include "private/ir/internal.h"
#include "ir/diag.h"

static core_string_t file_name(const ir_render_t *r, int32_t file)
{
    if (!r || !r->module || file < 0) {
        return CORE_STRING_STATIC("<unknown>");
    }

    core_string_t p = ir_file_path(r->module, file);

    return p.data ? p : CORE_STRING_STATIC("<unknown>");
}

static bool line_of(const ir_render_t *r, ir_span_t s, core_string_t *out)
{
    if (!r || !r->source || !ir_span_known(s)) {
        return false;
    }

    core_string_t src = r->source(r->state, s.file);
    size_t        n   = src.u8_bytes;

    if (!src.data || s.start_byte > n) {
        return false;
    }

    size_t b = s.start_byte;

    while (b > 0 && src.data[b - 1] != '\n') {
        b--;
    }

    size_t e = s.start_byte;

    while (e < n && src.data[e] != '\n') {
        e++;
    }

    *out = core_string_slice(src, b, e - b);

    return true;
}

static void render_caret(core_buffer_t *b, const ir_render_t *r, ir_span_t s,
                         core_string_t label)
{
    core_string_t line;

    if (!line_of(r, s, &line)) {
        return;
    }

    core_buffer_printf(b, "     |\n%4u | ", s.start_line);
    core_buffer_append_string(b, line);
    core_buffer_puts(b, "\n     | ");

    uint32_t pad = s.start_col ? s.start_col - 1 : 0;

    for (uint32_t i = 0; i < pad; i++) {
        core_buffer_puts(b, " ");
    }

    uint32_t width = 1;

    if (s.end_byte > s.start_byte && s.end_line == s.start_line) {
        width = s.end_col > s.start_col ? s.end_col - s.start_col : 1;
    }

    for (uint32_t i = 0; i < width; i++) {
        core_buffer_puts(b, "^");
    }

    if (label.data) {
        core_buffer_puts(b, " ");
        core_buffer_append_string(b, label);
    }
    core_buffer_puts(b, "\n");
}

static core_string_t origin_word(ir_origin_kind_t k)
{
    switch (k) {
    case IR_ORIGIN_INLINED_FROM:
        return CORE_STRING_STATIC("inlined from here");
    case IR_ORIGIN_EXPANDED_FROM:
        return CORE_STRING_STATIC("expanded from here");
    case IR_ORIGIN_LOWERED_FROM:
        return CORE_STRING_STATIC("lowered from here");
    case IR_ORIGIN_GENERATED_BY:
        return CORE_STRING_STATIC("generated here");
    default:
        return CORE_STRING_STATIC("from here");
    }
}

static void put_location(core_buffer_t *b, core_string_t ind,
                         const ir_render_t *r, ir_span_t s)
{
    core_string_t file = file_name(r, s.file);

    core_buffer_append_string(b, ind);
    core_buffer_printf(b, "  --> %.*s:%u:%u\n", (int)file.u8_bytes, file.data,
                       s.start_line, s.start_col);
}

core_string_t ir_diags_render_text(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return (core_string_t){};
    }
    ir_diags_sort(d);

    core_buffer_t *b = core_buffer_empty();

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg  = ir_diags_at(d, i);
        core_string_t    ind = dg->severity == IR_SEV_NOTE
                                    || dg->severity == IR_SEV_HELP
                                 ? CORE_STRING_STATIC("  ")
                                 : CORE_STRING_STATIC("");

        core_buffer_append_string(b, ind);
        core_buffer_append_string(b, ir_severity_name(dg->severity));
        core_buffer_puts(b, "[");
        core_buffer_append_string(b, ir_diag_code(dg->code));
        core_buffer_puts(b, "]: ");
        core_buffer_append_string(b, dg->message);
        core_buffer_puts(b, "\n");

        if (ir_span_known(dg->primary.span)) {
            put_location(b, ind, r, dg->primary.span);
            render_caret(b, r, dg->primary.span, dg->primary.label);
        }
        else if (dg->primary.label.data) {
            core_buffer_append_string(b, ind);
            core_buffer_puts(b, "  ");
            core_buffer_append_string(b, dg->primary.label);
            core_buffer_puts(b, "\n");
        }

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            const ir_label_t *l = &dg->secondary[k];

            put_location(b, ind, r, l->span);
            render_caret(b, r, l->span, l->label);
        }

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (o->kind == IR_ORIGIN_SOURCE) {
                continue;
            }

            core_buffer_append_string(b, ind);
            core_buffer_puts(b, "  note: ");
            core_buffer_append_string(b, origin_word(o->kind));

            if (o->agent.data) {
                core_buffer_puts(b, " by ");
                core_buffer_append_string(b, o->agent);
            }

            if (ir_span_known(o->span)) {
                core_string_t file = file_name(r, o->span.file);

                core_buffer_printf(b, " (%.*s:%u:%u)", (int)file.u8_bytes,
                                   file.data, o->span.start_line,
                                   o->span.start_col);
            }
            core_buffer_puts(b, "\n");
        }

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            const ir_suggestion_t *s = &dg->suggestions[k];

            core_buffer_append_string(b, ind);
            core_buffer_puts(b, "  help: ");
            core_buffer_append_string(b, s->description.data
                                             ? s->description
                                             : CORE_STRING_STATIC("try"));
            core_buffer_puts(b, "\n");
            core_buffer_append_string(b, ind);
            core_buffer_puts(b, "      + ");
            core_buffer_append_string(b, s->replacement);
            core_buffer_puts(b, "\n");
        }
        core_buffer_puts(b, "\n");
    }

    return core_buffer_take_string(b);
}

static void json_span(core_buffer_t *b, const ir_render_t *r, ir_span_t s)
{
    if (!ir_span_known(s)) {
        core_buffer_puts(b, "null");

        return;
    }

    core_buffer_puts(b, "{\"file\":");
    ir_quote(b, file_name(r, s.file));
    core_buffer_printf(b,
           ",\"file_id\":%d,\"start_byte\":%u,\"end_byte\":%u,"
           "\"start_line\":%u,\"start_col\":%u,\"end_line\":%u,"
           "\"end_col\":%u}",
           s.file, s.start_byte, s.end_byte, s.start_line, s.start_col,
           s.end_line, s.end_col);
}

static void json_optional(core_buffer_t *b, core_string_t s)
{
    if (s.data) {
        ir_quote(b, s);
    }
    else {
        core_buffer_puts(b, "null");
    }
}

core_string_t ir_diags_render_json(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return (core_string_t){};
    }
    ir_diags_sort(d);

    core_buffer_t *b = core_buffer_empty();

    core_buffer_puts(b, "[");

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (i) {
            core_buffer_puts(b, ",");
        }
        core_buffer_puts(b, "\n  {\"severity\":");
        ir_quote(b, ir_severity_name(dg->severity));
        core_buffer_puts(b, ",\"code\":");
        ir_quote(b, ir_diag_code(dg->code));
        core_buffer_puts(b, ",\"message\":");
        ir_quote(b, dg->message);
        core_buffer_puts(b, ",\"primary\":{\"span\":");
        json_span(b, r, dg->primary.span);
        core_buffer_puts(b, ",\"label\":");
        json_optional(b, dg->primary.label);
        core_buffer_puts(b, "},\"secondary\":[");

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            if (k) {
                core_buffer_puts(b, ",");
            }
            core_buffer_puts(b, "{\"span\":");
            json_span(b, r, dg->secondary[k].span);
            core_buffer_puts(b, ",\"label\":");
            json_optional(b, dg->secondary[k].label);
            core_buffer_puts(b, "}");
        }
        core_buffer_puts(b, "],\"suggestions\":[");

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            if (k) {
                core_buffer_puts(b, ",");
            }
            core_buffer_puts(b, "{\"span\":");
            json_span(b, r, dg->suggestions[k].span);
            core_buffer_puts(b, ",\"replacement\":");
            ir_quote(b, dg->suggestions[k].replacement);
            core_buffer_puts(b, ",\"description\":");
            json_optional(b, dg->suggestions[k].description);
            core_buffer_puts(b, "}");
        }
        core_buffer_puts(b, "],\"origin\":[");

        uint32_t on = 0;

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (on++) {
                core_buffer_puts(b, ",");
            }
            core_buffer_puts(b, "{\"kind\":");
            ir_quote(b, origin_word(o->kind));
            core_buffer_puts(b, ",\"agent\":");
            json_optional(b, o->agent);
            core_buffer_puts(b, ",\"span\":");
            json_span(b, r, o->span);
            core_buffer_puts(b, "}");
        }
        core_buffer_puts(b, "]}");
    }
    core_buffer_puts(b, "\n]\n");

    return core_buffer_take_string(b);
}
