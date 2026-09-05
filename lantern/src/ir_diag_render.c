#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ir_internal.h"
#include "ir/diag.h"

typedef struct {
    char  *p;
    size_t len;
    size_t cap;
} sb;

static bool sb_need(sb *b, size_t n)
{
    if (b->len + n + 1 <= b->cap) {
        return true;
    }

    size_t cap = b->cap ? b->cap : 256;

    while (cap < b->len + n + 1) {
        cap *= 2;
    }

    char *q = realloc(b->p, cap);

    if (!q) {
        return false;
    }
    b->p   = q;
    b->cap = cap;

    return true;
}

static void sb_put(sb *b, const char *s, size_t n)
{
    if (!sb_need(b, n)) {
        return;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(sb *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_fmt(sb *b, const char *fmt, ...)
{
    va_list ap;
    char    tmp[512];

    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        sb_put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

static void sb_json(sb *b, const char *s)
{
    sb_str(b, "\"");

    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        switch (*p) {
        case '"':
            sb_str(b, "\\\"");
            break;
        case '\\':
            sb_str(b, "\\\\");
            break;
        case '\n':
            sb_str(b, "\\n");
            break;
        case '\t':
            sb_str(b, "\\t");
            break;
        case '\r':
            sb_str(b, "\\r");
            break;
        default:
            if (*p < 0x20) {
                sb_fmt(b, "\\u%04x", *p);
            }
            else {
                sb_put(b, (const char *)p, 1);
            }
        }
    }
    sb_str(b, "\"");
}

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

static void render_caret(sb *b, const ir_render_t *r, ir_span_t s,
                         const char *label)
{
    const char *line;
    size_t      len;

    if (!line_of(r, s, &line, &len)) {
        return;
    }

    sb_fmt(b, "     |\n%4u | ", s.start_line);
    sb_put(b, line, len);
    sb_str(b, "\n     | ");

    uint32_t pad = s.start_col ? s.start_col - 1 : 0;

    for (uint32_t i = 0; i < pad; i++) {
        sb_str(b, " ");
    }

    uint32_t width = 1;

    if (s.end_byte > s.start_byte && s.end_line == s.start_line) {
        width = s.end_col > s.start_col ? s.end_col - s.start_col : 1;
    }

    for (uint32_t i = 0; i < width; i++) {
        sb_str(b, "^");
    }

    if (label) {
        sb_fmt(b, " %s", label);
    }
    sb_str(b, "\n");
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

    sb b = {0};

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);
        const char      *ind = dg->severity == IR_SEV_NOTE
                                    || dg->severity == IR_SEV_HELP
                                 ? "  "
                                 : "";

        sb_fmt(&b, "%s%s[%s]: %s\n", ind, ir_severity_name(dg->severity),
               ir_diag_code(dg->code), dg->message);

        if (ir_span_known(dg->primary.span)) {
            sb_fmt(&b, "%s  --> %s:%u:%u\n", ind,
                   file_name(r, dg->primary.span.file),
                   dg->primary.span.start_line, dg->primary.span.start_col);
            render_caret(&b, r, dg->primary.span, dg->primary.label);
        }
        else if (dg->primary.label) {
            sb_fmt(&b, "%s  %s\n", ind, dg->primary.label);
        }

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            const ir_label_t *l = &dg->secondary[k];

            sb_fmt(&b, "%s  --> %s:%u:%u\n", ind, file_name(r, l->span.file),
                   l->span.start_line, l->span.start_col);
            render_caret(&b, r, l->span, l->label);
        }

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (o->kind == IR_ORIGIN_SOURCE) {
                continue;
            }

            sb_fmt(&b, "%s  note: %s", ind, origin_word(o->kind));

            if (o->agent) {
                sb_fmt(&b, " by %s", o->agent);
            }

            if (ir_span_known(o->span)) {
                sb_fmt(&b, " (%s:%u:%u)", file_name(r, o->span.file),
                       o->span.start_line, o->span.start_col);
            }
            sb_str(&b, "\n");
        }

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            const ir_suggestion_t *s = &dg->suggestions[k];

            sb_fmt(&b, "%s  help: %s\n", ind,
                   s->description ? s->description : "try");
            sb_fmt(&b, "%s      + %s\n", ind, s->replacement);
        }
        sb_str(&b, "\n");
    }

    if (!b.p) {
        b.p = calloc(1, 1);
    }

    return b.p;
}

static void json_span(sb *b, const ir_render_t *r, ir_span_t s)
{
    if (!ir_span_known(s)) {
        sb_str(b, "null");

        return;
    }

    sb_str(b, "{\"file\":");
    sb_json(b, file_name(r, s.file));
    sb_fmt(b,
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

    sb b = {0};

    sb_str(&b, "[");

    for (uint32_t i = 0; i < ir_diags_count(d); i++) {
        const ir_diag_t *dg = ir_diags_at(d, i);

        if (i) {
            sb_str(&b, ",");
        }
        sb_str(&b, "\n  {\"severity\":");
        sb_json(&b, ir_severity_name(dg->severity));
        sb_str(&b, ",\"code\":");
        sb_json(&b, ir_diag_code(dg->code));
        sb_str(&b, ",\"message\":");
        sb_json(&b, dg->message ? dg->message : "");
        sb_str(&b, ",\"primary\":{\"span\":");
        json_span(&b, r, dg->primary.span);
        sb_str(&b, ",\"label\":");

        if (dg->primary.label) {
            sb_json(&b, dg->primary.label);
        }
        else {
            sb_str(&b, "null");
        }
        sb_str(&b, "},\"secondary\":[");

        for (uint32_t k = 0; k < dg->n_secondary; k++) {
            if (k) {
                sb_str(&b, ",");
            }
            sb_str(&b, "{\"span\":");
            json_span(&b, r, dg->secondary[k].span);
            sb_str(&b, ",\"label\":");

            if (dg->secondary[k].label) {
                sb_json(&b, dg->secondary[k].label);
            }
            else {
                sb_str(&b, "null");
            }
            sb_str(&b, "}");
        }
        sb_str(&b, "],\"suggestions\":[");

        for (uint32_t k = 0; k < dg->n_suggestions; k++) {
            if (k) {
                sb_str(&b, ",");
            }
            sb_str(&b, "{\"span\":");
            json_span(&b, r, dg->suggestions[k].span);
            sb_str(&b, ",\"replacement\":");
            sb_json(&b, dg->suggestions[k].replacement);
            sb_str(&b, ",\"description\":");

            if (dg->suggestions[k].description) {
                sb_json(&b, dg->suggestions[k].description);
            }
            else {
                sb_str(&b, "null");
            }
            sb_str(&b, "}");
        }
        sb_str(&b, "],\"origin\":[");

        uint32_t on = 0;

        for (ir_origin_t *o = dg->origin; o; o = o->parent) {
            if (on++) {
                sb_str(&b, ",");
            }
            sb_str(&b, "{\"kind\":");
            sb_json(&b, origin_word(o->kind));
            sb_str(&b, ",\"agent\":");

            if (o->agent) {
                sb_json(&b, o->agent);
            }
            else {
                sb_str(&b, "null");
            }
            sb_str(&b, ",\"span\":");
            json_span(&b, r, o->span);
            sb_str(&b, "}");
        }
        sb_str(&b, "]}");
    }
    sb_str(&b, "\n]\n");

    return b.p;
}
