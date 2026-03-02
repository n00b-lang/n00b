/*
 * Label widget: renders styled text into a plane with alignment and wrap.
 */

#include "n00b.h"
#include <stdio.h>
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/label.h"
#include "text/unicode/properties.h"
#include "text/unicode/linebreak.h"
#include "text/strings/string_ops.h"
#include "text/strings/text_style.h"

// Temp debug log — writes to same file as widget_demo.
static FILE *g_label_log = nullptr;

__attribute__((constructor))
static void label_open_log(void) {
    g_label_log = fopen("/tmp/widget_demo.log", "a");
    if (!g_label_log) g_label_log = stderr;
}

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

/*
 * Extract the base text style from a string's styling metadata.
 * Returns nullptr if the string has no styling.
 */
static n00b_text_style_t *
label_get_text_style(n00b_string_t *text)
{
    if (!text || !text->styling) {
        return nullptr;
    }

    n00b_string_style_info_t *si = (n00b_string_style_info_t *)text->styling;
    return si->base_style;
}

static int32_t
label_wrapped_rows(n00b_string_t *text, int32_t width)
{
    if (!text || text->u8_bytes == 0 || width == 0) {
        return 1;
    }

    int32_t text_width = n00b_unicode_display_width(text);

    if (text_width <= width) {
        return 1;
    }

    n00b_array_t(uint32_t) breaks =
        n00b_unicode_linebreak_wrap(text, .width = (int)width);

    // Number of lines = number of breaks + 1.
    return (int32_t)(breaks.len + 1);
}

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
label_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    // n00b_label_t is GC-managed; nothing to manually free.
    (void)data;
}

/*
 * Render a single line of text at (0, y) with horizontal alignment.
 */
static void
label_render_line(n00b_plane_t      *plane,
                  n00b_string_t     *line,
                  int32_t            y,
                  int32_t            content_w,
                  n00b_alignment_t   halign,
                  n00b_text_style_t *style)
{
    int32_t line_width = n00b_plane_text_width(plane, line, style);
    int32_t x = 0;

    if (halign == N00B_ALIGN_CENTER && line_width < content_w) {
        x = (content_w - line_width) / 2;
    }
    else if (halign == N00B_ALIGN_RIGHT && line_width < content_w) {
        x = content_w - line_width;
    }

    fprintf(g_label_log, "[label_render_line] line_width=%d content_w=%d halign=%d x=%d y=%d\n",
            line_width, content_w, (int)halign, x, y);
    fflush(g_label_log);

    n00b_plane_draw_text(plane, x, y, line, .style = style);
}

static void
label_render(n00b_plane_t *plane, void *data)
{
    n00b_label_t *label = (n00b_label_t *)data;

    if (!label || !label->text) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    n00b_text_style_t *style = label_get_text_style(label->text);
    int32_t line_h = n00b_plane_line_height(plane, style);
    int32_t text_w = n00b_plane_text_width(plane, label->text, style);
    n00b_alignment_t halign = label->alignment & N00B_HORIZONTAL_MASK;

    fprintf(g_label_log, "[label_render] content_w=%d content_h=%d text_w=%d "
            "line_h=%d halign=%d wrap=%d canvas=%p text='%.*s'\n",
            content_w, content_h, text_w, line_h, (int)halign,
            (int)label->wrap, (void *)plane->canvas,
            (int)label->text->u8_bytes, label->text->data);
    fflush(g_label_log);

    if (!label->wrap) {
        label_render_line(plane, label->text, 0, content_w, halign, style);
        return;
    }

    // Convert pixel width to character columns for the linebreak algorithm.
    int32_t wrap_cols = n00b_plane_text_columns(plane, content_w, style);
    fprintf(g_label_log, "[label_render] wrap_cols=%d\n", wrap_cols);
    fflush(g_label_log);

    // Unicode-aware word wrap (expects column count, not pixels).
    n00b_array_t(uint32_t) breaks =
        n00b_unicode_linebreak_wrap(label->text, .width = (int)wrap_cols);

    if (breaks.len == 0) {
        // Fits on one line.
        label_render_line(plane, label->text, 0, content_w, halign, style);
        return;
    }

    // Render each wrapped line with alignment.
    uint32_t line_start = 0;
    int32_t  y = 0;

    for (uint32_t i = 0; i < breaks.len && y < content_h; i++) {
        uint32_t line_end = n00b_array_get(breaks, i);

        // Strip trailing whitespace at the break point.
        uint32_t trim_end = line_end;
        while (trim_end > line_start
               && (label->text->data[trim_end - 1] == ' '
                   || label->text->data[trim_end - 1] == '\n')) {
            trim_end--;
        }

        if (trim_end > line_start) {
            n00b_string_t *line =
                n00b_unicode_str_slice_bytes(label->text, line_start, trim_end);
            label_render_line(plane, line, y, content_w, halign, style);
        }

        line_start = line_end;
        y += line_h;
    }

    // Render the last line (after the final break).
    if (y < content_h && line_start < (uint32_t)label->text->u8_bytes) {
        uint32_t trim_end = (uint32_t)label->text->u8_bytes;
        while (trim_end > line_start
               && (label->text->data[trim_end - 1] == ' '
                   || label->text->data[trim_end - 1] == '\n')) {
            trim_end--;
        }

        if (trim_end > line_start) {
            n00b_string_t *line =
                n00b_unicode_str_slice_bytes(label->text, line_start, trim_end);
            label_render_line(plane, line, y, content_w, halign, style);
        }
    }
}

static void
label_measure(n00b_plane_t *plane, void *data,
              int32_t *pref_w, int32_t *pref_h,
              int32_t *min_w,  int32_t *min_h)
{
    n00b_label_t *label = (n00b_label_t *)data;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    if (!label || !label->text || label->text->u8_bytes == 0) {
        *pref_w = 0;
        *pref_h = lh;
        *min_w  = 0;
        *min_h  = lh;
        return;
    }

    int32_t w = n00b_plane_text_width(plane, label->text, nullptr);

    *pref_w = (w > 0 ? w : 0);
    *pref_h = lh;
    *min_w  = 1;
    *min_h  = lh;

    if (label->wrap && *pref_w > 0) {
        // When wrapping, preferred height depends on available width.
        // We report lh as minimum; actual height computed at layout time.
        *pref_h = lh;
    }
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_label = {
    .kind    = "label",
    .destroy = label_destroy,
    .render  = label_render,
    .measure = label_measure,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_label_new(n00b_string_t *text) _kargs {
    n00b_alignment_t   alignment = N00B_ALIGN_LEFT;
    bool               wrap      = false;
    n00b_box_props_t  *box       = nullptr;
    n00b_text_style_t *style     = nullptr;
    int32_t            width     = 0;
    int32_t            height    = 0;
    n00b_allocator_t  *allocator = nullptr;
    n00b_canvas_t     *canvas    = nullptr;
}
{
    // Allocate the plane (with canvas for font metrics).
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                          .box       = box,
                                          .style     = style,
                                          .allocator = allocator,
                                          .canvas    = canvas);

    // Auto-size from text if dimensions not specified.
    if (width == 0 && text) {
        width = n00b_plane_text_width(plane, text, nullptr);
        if (width <= 0) width = 1;
    }
    if (width == 0) {
        width = 1;
    }

    if (height == 0) {
        int32_t lh = n00b_plane_line_height(plane, nullptr);
        if (lh <= 0) lh = 1;
        if (wrap && text) {
            int32_t wrap_cols = n00b_plane_text_columns(plane, width, nullptr);
            if (wrap_cols <= 0) wrap_cols = 1;
            height = label_wrapped_rows(text, wrap_cols) * lh;
        }
        else {
            height = lh;
        }
    }

    plane->width = width;
    plane->height = height;

    // Allocate and fill the label data.
    n00b_label_t *label = n00b_alloc(n00b_label_t);
    label->text      = text;
    label->alignment = alignment;
    label->wrap      = wrap;

    // Attach and render.
    n00b_widget_attach(plane, &n00b_widget_label, label);
    n00b_plane_mark_dirty(plane);
    n00b_widget_render(plane);

    return plane;
}

void
n00b_label_set_text(n00b_plane_t *plane, n00b_string_t *text)
{
    if (!plane || !plane->widget_vtable || plane->widget_vtable != &n00b_widget_label) {
        return;
    }

    n00b_label_t *label = (n00b_label_t *)plane->widget_data;
    label->text = text;
    n00b_plane_mark_dirty(plane);
    n00b_widget_render(plane);
}

n00b_string_t *
n00b_label_get_text(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_label) {
        return nullptr;
    }

    n00b_label_t *label = (n00b_label_t *)plane->widget_data;
    return label->text;
}
