/*
 * Progress bar widget: horizontal or vertical, 0.0–1.0.
 *
 * Uses partial block characters (U+258F..U+2588) for sub-cell
 * precision on the boundary cell.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/progress.h"
#include "text/strings/text_style.h"

// Partial horizontal block characters: 1/8 to 8/8.
// U+2588 = full block, U+2589..U+258F = 7/8..1/8.
static const n00b_codepoint_t partial_blocks[9] = {
    ' ',     // 0/8
    0x258F,  // 1/8
    0x258E,  // 2/8
    0x258D,  // 3/8
    0x258C,  // 4/8
    0x258B,  // 5/8
    0x258A,  // 6/8
    0x2589,  // 7/8
    0x2588,  // 8/8
};

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
progress_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
progress_render(n00b_plane_t *plane, void *data)
{
    n00b_progress_t *prog = (n00b_progress_t *)data;
    if (!prog) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    double val = prog->value;
    if (val < 0.0) val = 0.0;
    if (val > 1.0) val = 1.0;

    n00b_codepoint_t fill_cp  = prog->fill_char;
    n00b_codepoint_t empty_cp = prog->empty_char;

    if (prog->vertical) {
        // Vertical: fill from bottom up. Work in whole line-height rows.
        int32_t n_rows     = content_h / lh;
        double  fill_exact = val * (double)n_rows;
        int32_t fill_rows  = (int32_t)fill_exact;
        int32_t fill_px    = fill_rows * lh;

        // Empty rows from the top.
        if (content_h - fill_px > 0) {
            n00b_plane_fill_rect(plane, 0, 0,
                                  content_w, content_h - fill_px,
                                  .cp    = empty_cp,
                                  .style = prog->empty_style);
        }

        // Filled rows from the bottom.
        if (fill_px > 0) {
            n00b_plane_fill_rect(plane, 0, content_h - fill_px,
                                  content_w, fill_px,
                                  .cp    = fill_cp,
                                  .style = prog->fill_style);
        }
    }
    else {
        // Horizontal: fill from left to right. Work in whole cell-width columns.
        int32_t n_cols     = content_w / cpw;
        double  fill_exact = val * (double)n_cols;
        int32_t fill_cols  = (int32_t)fill_exact;
        double  fraction   = fill_exact - (double)fill_cols;
        int32_t fill_px    = fill_cols * cpw;

        // Full filled columns.
        if (fill_px > 0) {
            n00b_plane_fill_rect(plane, 0, 0, fill_px, lh,
                                  .cp    = fill_cp,
                                  .style = prog->fill_style);
        }

        // Partial block for sub-cell precision.
        if (fill_cols < n_cols) {
            int partial_idx = (int)(fraction * 8.0);
            if (partial_idx < 0) partial_idx = 0;
            if (partial_idx > 8) partial_idx = 8;

            if (partial_idx > 0) {
                n00b_plane_draw_glyph(plane, fill_px, 0,
                                       partial_blocks[partial_idx],
                                       .style = prog->fill_style);
                fill_px += cpw;
            }
        }

        // Empty columns.
        if (fill_px < content_w) {
            n00b_plane_fill_rect(plane, fill_px, 0,
                                  content_w - fill_px, lh,
                                  .cp    = empty_cp,
                                  .style = prog->empty_style);
        }
    }
}

static void
progress_measure(n00b_plane_t *plane, void *data,
                 int32_t *pref_w, int32_t *pref_h,
                 int32_t *min_w,  int32_t *min_h)
{
    n00b_progress_t *prog = (n00b_progress_t *)data;

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    if (prog && prog->vertical) {
        *pref_w = cpw;
        *pref_h = 10 * lh;
        *min_w  = cpw;
        *min_h  = lh;
    }
    else {
        *pref_w = 20 * cpw;
        *pref_h = lh;
        *min_w  = 3 * cpw;
        *min_h  = lh;
    }
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_progress = {
    .kind    = "progress",
    .destroy = progress_destroy,
    .render  = progress_render,
    .measure = progress_measure,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_progress_new() _kargs {
    double              value       = 0.0;
    bool                vertical    = false;
    n00b_text_style_t  *fill_style  = nullptr;
    n00b_text_style_t  *empty_style = nullptr;
    int32_t             width       = 20;
    int32_t             height      = 1;
    n00b_canvas_t      *canvas      = nullptr;
    n00b_allocator_t   *allocator   = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .canvas    = canvas,
                                           .allocator = allocator);

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    // Convert caller-supplied character-count widths to pixels.
    // The default karg value of 20 means "20 characters wide".
    if (width == 20) {
        width = 20 * cpw;
    }

    if (vertical && height == 1) {
        height = 10 * lh;
    }
    else if (!vertical && height <= 1) {
        height = lh;
    }

    plane->width = width;
    plane->height = height;

    n00b_progress_t *prog = n00b_alloc(n00b_progress_t);
    prog->value       = value;
    prog->vertical    = vertical;
    prog->fill_style  = fill_style;
    prog->empty_style = empty_style;
    prog->fill_char   = 0x2588;  // Full block.
    prog->empty_char  = 0x2591;  // Light shade.

    n00b_widget_attach(plane, &n00b_widget_progress, prog);
    n00b_plane_mark_dirty(plane);
    n00b_widget_render(plane);

    return plane;
}

void
n00b_progress_set_value(n00b_plane_t *plane, double value)
{
    if (!plane || plane->widget_vtable != &n00b_widget_progress) {
        return;
    }

    n00b_progress_t *prog = (n00b_progress_t *)plane->widget_data;
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;
    prog->value = value;
    n00b_plane_mark_dirty(plane);
    n00b_widget_render(plane);
}

double
n00b_progress_get_value(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_progress) {
        return 0.0;
    }

    n00b_progress_t *prog = (n00b_progress_t *)plane->widget_data;
    return prog->value;
}
