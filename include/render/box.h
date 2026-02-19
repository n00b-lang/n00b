/**
 * @file box.h
 * @brief Box decoration stamping for the render layer.
 *
 * Provides functions to create box properties, compute content insets
 * (border + padding), and stamp border/padding decorations onto a
 * cell grid during compositing.
 *
 * ### Design
 *
 * Borders and padding are **not** stored in the plane's content grid.
 * Instead, during compositing, `n00b_box_stamp()` writes border and
 * padding cells into the composite frame buffer around the plane's
 * content area.
 *
 * ### Related modules
 *
 * - `render/types.h` — `n00b_box_props_t`, `n00b_border_theme_t`
 * - `render/cell.h` — cell type written by stamping
 * - `render/composite.h` — calls `n00b_box_stamp()` during compositing
 */
#pragma once

#include "n00b.h"
#include "render/cell.h"
#include "render/types.h"

// ====================================================================
// Box creation
// ====================================================================

/**
 * @brief Create box properties with the given border theme and settings.
 *
 * @kw theme       Border theme (nullptr = no borders drawn).
 * @kw borders     Which borders to draw (default: N00B_BORDER_ALL).
 * @kw border_style Style for border characters.
 * @kw fill_style  Style for padding/fill characters.
 * @kw pad_top     Top padding in cells.
 * @kw pad_bottom  Bottom padding in cells.
 * @kw pad_left    Left padding in cells.
 * @kw pad_right   Right padding in cells.
 * @kw margin_top  Top margin in cells.
 * @kw margin_bottom Bottom margin in cells.
 * @kw margin_left Left margin in cells.
 * @kw margin_right Right margin in cells.
 * @kw alignment   Content alignment within the box.
 * @kw overflow    Overflow handling mode.
 *
 * @post Returned pointer is allocated via `n00b_alloc`.
 */
extern n00b_box_props_t *
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
};

// ====================================================================
// Inset calculation
// ====================================================================

/**
 * @brief Compute the total inset (border + padding) on each side.
 * @param box        Box properties.
 * @param out_top    Output: top inset in cells.
 * @param out_bottom Output: bottom inset in cells.
 * @param out_left   Output: left inset in cells.
 * @param out_right  Output: right inset in cells.
 */
static inline void
n00b_box_insets(const n00b_box_props_t *box,
                n00b_isize_t           *out_top,
                n00b_isize_t           *out_bottom,
                n00b_isize_t           *out_left,
                n00b_isize_t           *out_right)
{
    n00b_isize_t top = 0, bottom = 0, left = 0, right = 0;

    if (box) {
        if (box->borders & N00B_BORDER_TOP) {
            top++;
        }
        if (box->borders & N00B_BORDER_BOTTOM) {
            bottom++;
        }
        if (box->borders & N00B_BORDER_LEFT) {
            left++;
        }
        if (box->borders & N00B_BORDER_RIGHT) {
            right++;
        }

        top    += box->pad_top;
        bottom += box->pad_bottom;
        left   += box->pad_left;
        right  += box->pad_right;
    }

    if (out_top)    *out_top    = top;
    if (out_bottom) *out_bottom = bottom;
    if (out_left)   *out_left   = left;
    if (out_right)  *out_right  = right;
}

/**
 * @brief Compute total outer size including margins, borders, and padding.
 * @param box      Box properties.
 * @param content_rows Content area rows.
 * @param content_cols Content area columns.
 * @param out_rows     Output: total rows including all decoration.
 * @param out_cols     Output: total columns including all decoration.
 */
static inline void
n00b_box_outer_size(const n00b_box_props_t *box,
                     n00b_isize_t            content_rows,
                     n00b_isize_t            content_cols,
                     n00b_isize_t           *out_rows,
                     n00b_isize_t           *out_cols)
{
    n00b_isize_t top, bottom, left, right;
    n00b_box_insets(box, &top, &bottom, &left, &right);

    n00b_isize_t margin_h = 0, margin_v = 0;
    if (box) {
        margin_h = box->margin_left + box->margin_right;
        margin_v = box->margin_top + box->margin_bottom;
    }

    *out_rows = content_rows + top + bottom + margin_v;
    *out_cols = content_cols + left + right + margin_h;
}

// ====================================================================
// Box stamping
// ====================================================================

/**
 * @brief Stamp border and padding decoration onto a cell grid.
 *
 * Writes border characters and padding fill into the provided cell
 * grid at the specified origin.  The grid must be large enough to
 * contain the full outer size (see `n00b_box_outer_size()`).
 *
 * @param box          Box properties (borders, padding, theme).
 * @param grid         Target cell grid (row-major).
 * @param grid_cols    Stride (total columns in the grid).
 * @param origin_row   Row offset in grid where the box starts.
 * @param origin_col   Column offset in grid where the box starts.
 * @param outer_rows   Total rows of the box (including borders/padding).
 * @param outer_cols   Total columns of the box (including borders/padding).
 * @param border_style Style for border characters (nullable).
 * @param fill_style   Style for padding characters (nullable).
 *
 * @pre  `origin_row + outer_rows <= grid_rows` (caller responsibility).
 * @post Border and padding cells are written with `N00B_CELL_BORDER`
 *       and `N00B_CELL_PADDING` flags respectively.
 */
extern void n00b_box_stamp(const n00b_box_props_t *box,
                            n00b_rcell_t           *grid,
                            n00b_isize_t            grid_cols,
                            n00b_isize_t            origin_row,
                            n00b_isize_t            origin_col,
                            n00b_isize_t            outer_rows,
                            n00b_isize_t            outer_cols,
                            n00b_text_style_t      *border_style,
                            n00b_text_style_t      *fill_style);
