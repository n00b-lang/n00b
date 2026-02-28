/*
 * Box decoration: creation and stamping.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/box.h"

// -------------------------------------------------------------------
// Creation
// -------------------------------------------------------------------

n00b_box_props_t *
n00b_box_props_new() _kargs
{
    const n00b_border_theme_t *theme        = nullptr;
    n00b_border_set_t          borders      = N00B_BORDER_ALL;
    n00b_text_style_t         *border_style = nullptr;
    n00b_text_style_t         *fill_style   = nullptr;
    n00b_text_style_t         *text_style   = nullptr;
    int8_t                     pad_top      = 0;
    int8_t                     pad_bottom   = 0;
    int8_t                     pad_left     = 0;
    int8_t                     pad_right    = 0;
    int8_t                     margin_top   = 0;
    int8_t                     margin_bottom = 0;
    int8_t                     margin_left  = 0;
    int8_t                     margin_right = 0;
    n00b_alignment_t           alignment    = N00B_ALIGN_IGNORE;
    n00b_overflow_t            overflow     = N00B_OVERFLOW_CLIP;
}
{
    n00b_box_props_t *box = n00b_alloc(n00b_box_props_t);

    box->border_theme  = theme;
    box->borders       = borders;
    box->border_style  = border_style;
    box->fill_style    = fill_style;
    box->text_style    = text_style;
    box->pad_top       = pad_top;
    box->pad_bottom    = pad_bottom;
    box->pad_left      = pad_left;
    box->pad_right     = pad_right;
    box->margin_top    = margin_top;
    box->margin_bottom = margin_bottom;
    box->margin_left   = margin_left;
    box->margin_right  = margin_right;
    box->alignment     = alignment;
    box->overflow      = overflow;

    return box;
}

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

static inline void
stamp_border_cell(n00b_rcell_t      *cell,
                   n00b_codepoint_t   cp,
                   n00b_text_style_t *style)
{
    n00b_rcell_set_codepoint(cell, cp, 1, style);
    cell->flags = (n00b_cell_flags_t)(cell->flags | N00B_CELL_BORDER);
}

static inline void
stamp_padding_cell(n00b_rcell_t *cell, n00b_text_style_t *style)
{
    n00b_rcell_set_ascii(cell, ' ', style);
    cell->flags = (n00b_cell_flags_t)(cell->flags | N00B_CELL_PADDING);
}

static inline n00b_rcell_t *
grid_at(n00b_rcell_t *grid, n00b_isize_t grid_cols,
        n00b_isize_t row, n00b_isize_t col)
{
    return &grid[row * grid_cols + col];
}

// -------------------------------------------------------------------
// Box stamping
// -------------------------------------------------------------------

void
n00b_box_stamp(const n00b_box_props_t *box,
                n00b_rcell_t           *grid,
                n00b_isize_t            grid_cols,
                n00b_isize_t            origin_row,
                n00b_isize_t            origin_col,
                n00b_isize_t            outer_rows,
                n00b_isize_t            outer_cols,
                n00b_text_style_t      *border_style,
                n00b_text_style_t      *fill_style)
{
    if (!box || !grid || outer_rows == 0 || outer_cols == 0) {
        return;
    }

    const n00b_border_theme_t *theme = box->border_theme;
    n00b_border_set_t          borders = box->borders;

    bool has_top    = (borders & N00B_BORDER_TOP) && theme;
    bool has_bottom = (borders & N00B_BORDER_BOTTOM) && theme;
    bool has_left   = (borders & N00B_BORDER_LEFT) && theme;
    bool has_right  = (borders & N00B_BORDER_RIGHT) && theme;

    n00b_isize_t top_row    = origin_row;
    n00b_isize_t bottom_row = origin_row + outer_rows - 1;
    n00b_isize_t left_col   = origin_col;
    n00b_isize_t right_col  = origin_col + outer_cols - 1;

    // --- Top border row ---
    if (has_top) {
        // Corners.
        if (has_left) {
            stamp_border_cell(grid_at(grid, grid_cols, top_row, left_col),
                              theme->upper_left, border_style);
        }
        if (has_right) {
            stamp_border_cell(grid_at(grid, grid_cols, top_row, right_col),
                              theme->upper_right, border_style);
        }
        // Horizontal bar.
        n00b_isize_t start = left_col + (has_left ? 1 : 0);
        n00b_isize_t end   = right_col - (has_right ? 1 : 0);
        for (n00b_isize_t c = start; c <= end; c++) {
            stamp_border_cell(grid_at(grid, grid_cols, top_row, c),
                              theme->horizontal, border_style);
        }
    }

    // --- Bottom border row ---
    if (has_bottom) {
        if (has_left) {
            stamp_border_cell(grid_at(grid, grid_cols, bottom_row, left_col),
                              theme->lower_left, border_style);
        }
        if (has_right) {
            stamp_border_cell(grid_at(grid, grid_cols, bottom_row, right_col),
                              theme->lower_right, border_style);
        }
        n00b_isize_t start = left_col + (has_left ? 1 : 0);
        n00b_isize_t end   = right_col - (has_right ? 1 : 0);
        for (n00b_isize_t c = start; c <= end; c++) {
            stamp_border_cell(grid_at(grid, grid_cols, bottom_row, c),
                              theme->horizontal, border_style);
        }
    }

    // --- Left and right border columns ---
    n00b_isize_t row_start = top_row + (has_top ? 1 : 0);
    n00b_isize_t row_end   = bottom_row - (has_bottom ? 1 : 0);

    for (n00b_isize_t r = row_start; r <= row_end; r++) {
        if (has_left) {
            stamp_border_cell(grid_at(grid, grid_cols, r, left_col),
                              theme->vertical, border_style);
        }
        if (has_right) {
            stamp_border_cell(grid_at(grid, grid_cols, r, right_col),
                              theme->vertical, border_style);
        }
    }

    // --- Padding fill ---
    n00b_isize_t content_top  = top_row + (has_top ? 1 : 0);
    n00b_isize_t content_left = left_col + (has_left ? 1 : 0);
    n00b_isize_t content_bot  = bottom_row - (has_bottom ? 1 : 0);
    n00b_isize_t content_right = right_col - (has_right ? 1 : 0);

    // Top padding rows.
    for (int8_t pr = 0; pr < box->pad_top; pr++) {
        n00b_isize_t r = content_top + pr;
        if (r > content_bot) {
            break;
        }
        for (n00b_isize_t c = content_left; c <= content_right; c++) {
            stamp_padding_cell(grid_at(grid, grid_cols, r, c), fill_style);
        }
    }

    // Bottom padding rows.
    for (int8_t pr = 0; pr < box->pad_bottom; pr++) {
        n00b_isize_t r = content_bot - pr;
        if (r < content_top) {
            break;
        }
        for (n00b_isize_t c = content_left; c <= content_right; c++) {
            stamp_padding_cell(grid_at(grid, grid_cols, r, c), fill_style);
        }
    }

    // Left padding columns (in rows between top+bottom padding).
    n00b_isize_t inner_top = content_top + box->pad_top;
    n00b_isize_t inner_bot = content_bot - box->pad_bottom;

    for (n00b_isize_t r = inner_top; r <= inner_bot; r++) {
        for (int8_t pc = 0; pc < box->pad_left; pc++) {
            n00b_isize_t c = content_left + pc;
            if (c > content_right) {
                break;
            }
            stamp_padding_cell(grid_at(grid, grid_cols, r, c), fill_style);
        }
        for (int8_t pc = 0; pc < box->pad_right; pc++) {
            n00b_isize_t c = content_right - pc;
            if (c < content_left) {
                break;
            }
            stamp_padding_cell(grid_at(grid, grid_cols, r, c), fill_style);
        }
    }
}
