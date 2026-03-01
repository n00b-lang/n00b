/*
 * Label widget: renders styled text into a plane with alignment and wrap.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/label.h"
#include "text/unicode/properties.h"
#include "text/unicode/linebreak.h"
#include "text/strings/string_ops.h"

// -------------------------------------------------------------------
// Internal: compute wrapped line count
// -------------------------------------------------------------------

static n00b_isize_t
label_wrapped_rows(n00b_string_t *text, n00b_isize_t cols)
{
    if (!text || text->u8_bytes == 0 || cols == 0) {
        return 1;
    }

    int32_t text_width = n00b_unicode_display_width(text);

    if (text_width <= (int32_t)cols) {
        return 1;
    }

    n00b_array_t(uint32_t) breaks =
        n00b_unicode_linebreak_wrap(text, .width = (int)cols);

    // Number of lines = number of breaks + 1.
    return (n00b_isize_t)(breaks.len + 1);
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
 * Render a single line of text at (row, 0) with horizontal alignment.
 */
static void
label_render_line(n00b_plane_t     *plane,
                  n00b_string_t    *line,
                  n00b_isize_t      row,
                  n00b_isize_t      content_cols,
                  n00b_alignment_t  halign)
{
    int32_t line_width = n00b_unicode_display_width(line);
    n00b_isize_t offset = 0;

    if (halign == N00B_ALIGN_CENTER && line_width < (int32_t)content_cols) {
        offset = (n00b_isize_t)(((int32_t)content_cols - line_width) / 2);
    }
    else if (halign == N00B_ALIGN_RIGHT && line_width < (int32_t)content_cols) {
        offset = (n00b_isize_t)((int32_t)content_cols - line_width);
    }

    n00b_plane_put_str_at(plane, row, offset, line);
}

static void
label_render(n00b_plane_t *plane, void *data)
{
    n00b_label_t *label = (n00b_label_t *)data;

    if (!label || !label->text) {
        return;
    }

    n00b_plane_clear(plane);

    n00b_isize_t content_rows;
    n00b_isize_t content_cols;
    n00b_plane_content_size(plane, &content_rows, &content_cols);

    if (content_cols == 0 || content_rows == 0) {
        return;
    }

    n00b_alignment_t halign = label->alignment & N00B_HORIZONTAL_MASK;

    if (!label->wrap) {
        label_render_line(plane, label->text, 0, content_cols, halign);
        return;
    }

    // Unicode-aware word wrap.
    n00b_array_t(uint32_t) breaks =
        n00b_unicode_linebreak_wrap(label->text, .width = (int)content_cols);

    if (breaks.len == 0) {
        // Fits on one line.
        label_render_line(plane, label->text, 0, content_cols, halign);
        return;
    }

    // Render each wrapped line with alignment.
    uint32_t line_start = 0;
    n00b_isize_t row = 0;

    for (uint32_t i = 0; i < breaks.len && row < content_rows; i++) {
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
            label_render_line(plane, line, row, content_cols, halign);
        }

        line_start = line_end;
        row++;
    }

    // Render the last line (after the final break).
    if (row < content_rows && line_start < (uint32_t)label->text->u8_bytes) {
        uint32_t trim_end = (uint32_t)label->text->u8_bytes;
        while (trim_end > line_start
               && (label->text->data[trim_end - 1] == ' '
                   || label->text->data[trim_end - 1] == '\n')) {
            trim_end--;
        }

        if (trim_end > line_start) {
            n00b_string_t *line =
                n00b_unicode_str_slice_bytes(label->text, line_start, trim_end);
            label_render_line(plane, line, row, content_cols, halign);
        }
    }
}

static void
label_measure(n00b_plane_t *plane, void *data,
              n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
              n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_label_t *label = (n00b_label_t *)data;

    if (!label || !label->text || label->text->u8_bytes == 0) {
        *pref_cols = 0;
        *pref_rows = 1;
        *min_cols  = 0;
        *min_rows  = 1;
        return;
    }

    int32_t w = n00b_unicode_display_width(label->text);

    *pref_cols = (n00b_isize_t)(w > 0 ? w : 0);
    *pref_rows = 1;
    *min_cols  = 1;
    *min_rows  = 1;

    if (label->wrap && *pref_cols > 0) {
        // When wrapping, preferred rows depends on available width.
        // We report 1 row as minimum; actual rows computed at layout time.
        *pref_rows = 1;
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
    n00b_isize_t       cols      = 0;
    n00b_isize_t       rows      = 0;
    n00b_allocator_t  *allocator = nullptr;
}
{
    // Auto-size from text if dimensions not specified.
    if (cols == 0 && text) {
        int32_t w = n00b_unicode_display_width(text);
        cols = (n00b_isize_t)(w > 0 ? w : 1);
    }
    if (cols == 0) {
        cols = 1;
    }

    if (rows == 0) {
        if (wrap && text) {
            rows = label_wrapped_rows(text, cols);
        }
        else {
            rows = 1;
        }
    }

    // Allocate the plane.
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                          .cols      = cols,
                                          .rows      = rows,
                                          .box       = box,
                                          .style     = style,
                                          .allocator = allocator);

    // Allocate and fill the label data.
    n00b_label_t *label = n00b_alloc(n00b_label_t);
    label->text      = text;
    label->alignment = alignment;
    label->wrap      = wrap;

    // Attach and render.
    n00b_widget_attach(plane, &n00b_widget_label, label);
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
