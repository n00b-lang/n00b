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

    n00b_isize_t content_rows;
    n00b_isize_t content_cols;
    n00b_plane_content_size(plane, &content_rows, &content_cols);

    if (content_cols == 0 || content_rows == 0) {
        return;
    }

    double val = prog->value;
    if (val < 0.0) val = 0.0;
    if (val > 1.0) val = 1.0;

    n00b_codepoint_t fill_cp  = prog->fill_char;
    n00b_codepoint_t empty_cp = prog->empty_char;

    if (prog->vertical) {
        // Vertical: fill from bottom up.
        double fill_exact = val * (double)content_rows;
        n00b_isize_t fill_full = (n00b_isize_t)fill_exact;

        // Fill empty cells from top.
        if (content_rows - fill_full > 0) {
            n00b_plane_fill_rect(plane, 0, 0,
                                  content_rows - fill_full, content_cols,
                                  .cp    = empty_cp,
                                  .style = prog->empty_style);
        }

        // Fill full cells from bottom.
        if (fill_full > 0) {
            n00b_plane_fill_rect(plane,
                                  content_rows - fill_full, 0,
                                  fill_full, content_cols,
                                  .cp    = fill_cp,
                                  .style = prog->fill_style);
        }
    }
    else {
        // Horizontal: fill from left to right.
        double fill_exact = val * (double)content_cols;
        n00b_isize_t fill_full = (n00b_isize_t)fill_exact;
        double fraction = fill_exact - (double)fill_full;

        // Full filled cells.
        if (fill_full > 0) {
            n00b_plane_fill_rect(plane, 0, 0, 1, fill_full,
                                  .cp    = fill_cp,
                                  .style = prog->fill_style);
        }

        // Partial block for sub-cell precision.
        if (fill_full < content_cols) {
            int partial_idx = (int)(fraction * 8.0);
            if (partial_idx < 0) partial_idx = 0;
            if (partial_idx > 8) partial_idx = 8;

            if (partial_idx > 0) {
                n00b_plane_cursor_move(plane, 0, fill_full);
                n00b_plane_put_cp(plane, partial_blocks[partial_idx],
                                   .style = prog->fill_style);
                fill_full++;
            }
        }

        // Empty cells.
        if (fill_full < content_cols) {
            n00b_plane_fill_rect(plane, 0, fill_full, 1,
                                  content_cols - fill_full,
                                  .cp    = empty_cp,
                                  .style = prog->empty_style);
        }
    }
}

static void
progress_measure(n00b_plane_t *plane, void *data,
                 n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
                 n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_progress_t *prog = (n00b_progress_t *)data;

    if (prog && prog->vertical) {
        *pref_cols = 1;
        *pref_rows = 10;
        *min_cols  = 1;
        *min_rows  = 1;
    }
    else {
        *pref_cols = 20;
        *pref_rows = 1;
        *min_cols  = 3;
        *min_rows  = 1;
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
    n00b_isize_t        cols        = 20;
    n00b_isize_t        rows        = 1;
    n00b_allocator_t   *allocator   = nullptr;
}
{
    if (vertical && rows == 1) {
        rows = 10;
    }
    if (!vertical && cols == 20 && rows == 1) {
        // Defaults are fine.
    }

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .cols      = cols,
                                           .rows      = rows,
                                           .allocator = allocator);

    n00b_progress_t *prog = n00b_alloc(n00b_progress_t);
    prog->value       = value;
    prog->vertical    = vertical;
    prog->fill_style  = fill_style;
    prog->empty_style = empty_style;
    prog->fill_char   = 0x2588;  // Full block.
    prog->empty_char  = 0x2591;  // Light shade.

    n00b_widget_attach(plane, &n00b_widget_progress, prog);
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
