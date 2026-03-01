/*
 * Divider widget: horizontal or vertical separator line with optional label.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/divider.h"
#include "text/unicode/properties.h"

// Default border theme if none specified.
extern const n00b_border_theme_t n00b_border_plain;

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
divider_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
divider_render(n00b_plane_t *plane, void *data)
{
    n00b_divider_t *div = (n00b_divider_t *)data;
    if (!div) {
        return;
    }

    n00b_plane_clear(plane);

    n00b_isize_t content_rows;
    n00b_isize_t content_cols;
    n00b_plane_content_size(plane, &content_rows, &content_cols);

    if (content_cols == 0 || content_rows == 0) {
        return;
    }

    const n00b_border_theme_t *theme = div->theme;
    if (!theme) {
        theme = &n00b_border_plain;
    }

    n00b_codepoint_t line_cp = div->line_char;

    if (div->vertical) {
        if (line_cp == 0) {
            line_cp = theme->vertical;
        }
        n00b_plane_fill_rect(plane, 0, 0, content_rows, 1, .cp = line_cp);
    }
    else {
        if (line_cp == 0) {
            line_cp = theme->horizontal;
        }

        if (!div->label || div->label->u8_bytes == 0) {
            // No label: fill the entire row.
            n00b_plane_fill_rect(plane, 0, 0, 1, content_cols, .cp = line_cp);
        }
        else {
            // Label centered with gaps.
            int32_t label_width = n00b_unicode_display_width(div->label);
            // Format: "──── Label ────"
            // Need at least 2 chars on each side + spaces.
            n00b_isize_t total_line = (n00b_isize_t)(content_cols - label_width - 2);
            n00b_isize_t left_len;
            n00b_isize_t right_len;

            if (total_line < 2) {
                // Not enough room — just render the label.
                n00b_plane_put_str_at(plane, 0, 0, div->label);
                return;
            }

            left_len  = total_line / 2;
            right_len = total_line - left_len;

            // Left line segment.
            n00b_plane_fill_rect(plane, 0, 0, 1, left_len, .cp = line_cp);

            // Space + label + space.
            n00b_isize_t label_col = left_len + 1;
            n00b_plane_put_str_at(plane, 0, label_col, div->label);

            // Right line segment.
            n00b_isize_t right_start = label_col + (n00b_isize_t)label_width + 1;
            if (right_start < content_cols) {
                n00b_plane_fill_rect(plane, 0, right_start, 1,
                                      content_cols - right_start, .cp = line_cp);
            }
        }
    }
}

static void
divider_measure(n00b_plane_t *plane, void *data,
                n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
                n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_divider_t *div = (n00b_divider_t *)data;

    if (div && div->vertical) {
        *pref_cols = 1;
        *pref_rows = 3;
        *min_cols  = 1;
        *min_rows  = 1;
    }
    else {
        *pref_cols = 20;
        *pref_rows = 1;
        *min_cols  = 1;
        *min_rows  = 1;
    }
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_divider = {
    .kind    = "divider",
    .destroy = divider_destroy,
    .render  = divider_render,
    .measure = divider_measure,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_divider_new() _kargs {
    bool                       vertical   = false;
    n00b_string_t             *label      = nullptr;
    const n00b_border_theme_t *theme      = nullptr;
    n00b_codepoint_t           line_char  = 0;
    n00b_isize_t               cols       = 0;
    n00b_isize_t               rows       = 0;
    n00b_text_style_t         *style      = nullptr;
    n00b_allocator_t          *allocator  = nullptr;
}
{
    if (cols == 0) {
        cols = vertical ? 1 : 20;
    }
    if (rows == 0) {
        rows = vertical ? 10 : 1;
    }

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .cols      = cols,
                                           .rows      = rows,
                                           .style     = style,
                                           .allocator = allocator);

    n00b_divider_t *div = n00b_alloc(n00b_divider_t);
    div->label     = label;
    div->theme     = theme ? theme : &n00b_border_plain;
    div->line_char = line_char;
    div->vertical  = vertical;

    n00b_widget_attach(plane, &n00b_widget_divider, div);
    n00b_widget_render(plane);

    return plane;
}
