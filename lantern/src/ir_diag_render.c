#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/diag.h"

static const char *file_name(const ir_render_t *r, int32_t file)
{
    if (!r || !r->module || file < 0) {
        return "<unknown>";
    }

    const char *p = ir_file_path(r->module, file);

    return p ? p : "<unknown>";
}

static bool line_of(const ir_render_t *r, ir_span_t s, const char **start,
                    size_t *len)
{
    if (!r || !r->source || !ir_span_known(s)) {
        return false;
    }

    size_t      n   = 0;
    const char *src = r->source(r->state, s.file, &n);

    if (!src || s.start_byte > n) {
        return false;
    }

    size_t b = s.start_byte;

    while (b > 0 && src[b - 1] != '\n') {
        b--;
    }

    size_t e = s.start_byte;

    while (e < n && src[e] != '\n') {
        e++;
    }

    *start = src + b;
    *len   = e - b;

    return true;
}

static void render_caret(ir_sb *b, const ir_render_t *r, ir_span_t s,
                         const char *label)
{
    const char *line;
    size_t      len;

    if (!line_of(r, s, &line, &len)) {
        return;
    }

    ir_sb_fmt(b, "     |\n%4u | ", s.start_line);
    ir_sb_put(b, line, len);
    ir_sb_str(b, "\n     | ");

    uint32_t pad = s.start_col ? s.start_col - 1 : 0;

    for (uint32_t i = 0; i < pad; i++) {
        ir_sb_str(b, " ");
    }

    uint32_t width = 1;

    if (s.end_byte > s.start_byte && s.end_line == s.start_line) {
        width = s.end_col > s.start_col ? s.end_col - s.start_col : 1;
    }

    for (uint32_t i = 0; i < width; i++) {
        ir_sb_str(b, "^");
    }

    if (label) {
        ir_sb_fmt(b, " %s", label);
    }
    ir_sb_str(b, "\n");
}

static const char *origin_word(ir_origin_kind_t k)
{
    switch (k) {
    case IR_ORIGIN_INLINED_FROM:
        return "inlined from here";
    case IR_ORIGIN_EXPANDED_FROM:
        return "expanded from here";
    case IR_ORIGIN_LOWERED_FROM:
        return "lowered from here";
    case IR_ORIGIN_GENERATED_BY:
        return "generated here";
    default:
        return "from here";
    }
}

char *ir_diags_render_text(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return nullptr;
    }
    ir_diags_sort(d);

    ir_sb b = {0};

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);
        const char      *ind = dg->severity == IR_SEV_NOTE
                                    || dg->severity == IR_SEV_HELP
                                 ? "  "
                                 : "";

        ir_sb_fmt(&b, "%s%s[%s]: %s\n", ind, ir_severity_name(dg->severity),
               ir_diag_code(dg->code), dg->message);

        if (ir_span_known(dg->primary.span)) {
            ir_sb_fmt(&b, "%s  --> %s:%u:%u\n", ind,
                   file_name(r, dg->primary.span.file),
                   dg->primary.span.start_line, dg->primary.span.start_col);
            render_caret(&b, r, dg->primary.span, dg->primary.label);
        }
        else if (dg->primary.label) {
            ir_sb_fmt(&b, "%s  %s\n", ind, dg->primary.label);
        }

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            const ir_label_t *l = &dg->secondary[k];

            ir_sb_fmt(&b, "%s  --> %s:%u:%u\n", ind, file_name(r, l->span.file),
                   l->span.start_line, l->span.start_col);
            render_caret(&b, r, l->span, l->label);
        }

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (o->kind == IR_ORIGIN_SOURCE) {
                continue;
            }

            ir_sb_fmt(&b, "%s  note: %s", ind, origin_word(o->kind));

            if (o->agent) {
                ir_sb_fmt(&b, " by %s", o->agent);
            }

            if (ir_span_known(o->span)) {
                ir_sb_fmt(&b, " (%s:%u:%u)", file_name(r, o->span.file),
                       o->span.start_line, o->span.start_col);
            }
            ir_sb_str(&b, "\n");
        }

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            const ir_suggestion_t *s = &dg->suggestions[k];

            ir_sb_fmt(&b, "%s  help: %s\n", ind,
                   s->description ? s->description : "try");
            ir_sb_fmt(&b, "%s      + %s\n", ind, s->replacement);
        }
        ir_sb_str(&b, "\n");
    }

    return ir_sb_take(&b);
}

static void json_span(ir_sb *b, const ir_render_t *r, ir_span_t s)
{
    if (!ir_span_known(s)) {
        ir_sb_str(b, "null");

        return;
    }

    ir_sb_str(b, "{\"file\":");
    ir_sb_json(b, file_name(r, s.file));
    ir_sb_fmt(b,
           ",\"file_id\":%d,\"start_byte\":%u,\"end_byte\":%u,"
           "\"start_line\":%u,\"start_col\":%u,\"end_line\":%u,"
           "\"end_col\":%u}",
           s.file, s.start_byte, s.end_byte, s.start_line, s.start_col,
           s.end_line, s.end_col);
}

char *ir_diags_render_json(ir_diags_t *d, const ir_render_t *r)
{
    if (!d) {
        return nullptr;
    }
    ir_diags_sort(d);

    ir_sb b = {0};

    ir_sb_str(&b, "[");

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (i) {
            ir_sb_str(&b, ",");
        }
        ir_sb_str(&b, "\n  {\"severity\":");
        ir_sb_json(&b, ir_severity_name(dg->severity));
        ir_sb_str(&b, ",\"code\":");
        ir_sb_json(&b, ir_diag_code(dg->code));
        ir_sb_str(&b, ",\"message\":");
        ir_sb_json(&b, dg->message ? dg->message : "");
        ir_sb_str(&b, ",\"primary\":{\"span\":");
        json_span(&b, r, dg->primary.span);
        ir_sb_str(&b, ",\"label\":");

        if (dg->primary.label) {
            ir_sb_json(&b, dg->primary.label);
        }
        else {
            ir_sb_str(&b, "null");
        }
        ir_sb_str(&b, "},\"secondary\":[");

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            if (k) {
                ir_sb_str(&b, ",");
            }
            ir_sb_str(&b, "{\"span\":");
            json_span(&b, r, dg->secondary[k].span);
            ir_sb_str(&b, ",\"label\":");

            if (dg->secondary[k].label) {
                ir_sb_json(&b, dg->secondary[k].label);
            }
            else {
                ir_sb_str(&b, "null");
            }
            ir_sb_str(&b, "}");
        }
        ir_sb_str(&b, "],\"suggestions\":[");

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            if (k) {
                ir_sb_str(&b, ",");
            }
            ir_sb_str(&b, "{\"span\":");
            json_span(&b, r, dg->suggestions[k].span);
            ir_sb_str(&b, ",\"replacement\":");
            ir_sb_json(&b, dg->suggestions[k].replacement);
            ir_sb_str(&b, ",\"description\":");

            if (dg->suggestions[k].description) {
                ir_sb_json(&b, dg->suggestions[k].description);
            }
            else {
                ir_sb_str(&b, "null");
            }
            ir_sb_str(&b, "}");
        }
        ir_sb_str(&b, "],\"origin\":[");

        uint32_t on = 0;

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (on++) {
                ir_sb_str(&b, ",");
            }
            ir_sb_str(&b, "{\"kind\":");
            ir_sb_json(&b, origin_word(o->kind));
            ir_sb_str(&b, ",\"agent\":");

            if (o->agent) {
                ir_sb_json(&b, o->agent);
            }
            else {
                ir_sb_str(&b, "null");
            }
            ir_sb_str(&b, ",\"span\":");
            json_span(&b, r, o->span);
            ir_sb_str(&b, "}");
        }
        ir_sb_str(&b, "]}");
    }
    ir_sb_str(&b, "\n]\n");

    return ir_sb_take(&b);
}
