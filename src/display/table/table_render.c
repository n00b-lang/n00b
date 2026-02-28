/*
 * Table rendering: plane creation, border stamping, cell content writing.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/layout.h"
#include "display/render/box.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/table/table.h"
#include "text/strings/string_ops.h"
#include "text/strings/string_style.h"
#include "text/strings/style_ops.h"
#include "text/unicode/properties.h"

#include <assert.h>

// ====================================================================
// Internal: grid helpers
// ====================================================================

static inline n00b_rcell_t *
grid_at(n00b_rcell_t *grid, n00b_isize_t grid_cols,
        n00b_isize_t row, n00b_isize_t col)
{
    return &grid[row * grid_cols + col];
}

static inline void
stamp_border(n00b_rcell_t      *cell,
              n00b_codepoint_t   cp,
              n00b_text_style_t *style)
{
    n00b_rcell_set_codepoint(cell, cp, 1, style);
    cell->flags = (n00b_cell_flags_t)(cell->flags | N00B_CELL_BORDER);
}

// ====================================================================
// Internal: resolve effective wrap setting for a cell
// ====================================================================

static inline bool
cell_should_wrap(const n00b_table_t *table, const n00b_table_cell_t *cell)
{
    if (cell->wrap == N00B_TRI_YES) {
        return true;
    }
    if (cell->wrap == N00B_TRI_NO) {
        return false;
    }

    return table->wrap;
}

// ====================================================================
// Internal: visible row access (ring buffer aware)
// ====================================================================

static inline n00b_table_row_t *
render_visible_row(n00b_table_t *table, n00b_isize_t vis_idx)
{
    if (table->max_rows > 0) {
        n00b_isize_t actual = (table->ring_base + vis_idx) % table->max_rows;
        return &table->rows.data[actual];
    }

    return &table->rows.data[vis_idx];
}

// ====================================================================
// Internal: dimension computation
// ====================================================================

typedef struct {
    n00b_isize_t total_w;
    n00b_isize_t total_h;
    n00b_isize_t content_x; // Left edge of content area in grid coords.
    n00b_isize_t content_y; // Top edge of content area in grid coords.
    n00b_isize_t data_y;    // Top edge of data rows (after title).
    bool         has_top;
    bool         has_bottom;
    bool         has_left;
    bool         has_right;
    bool         has_int_h;
    bool         has_int_v;
    n00b_isize_t title_h;
    n00b_isize_t caption_h;
} render_dims_t;

static render_dims_t
compute_dimensions(n00b_table_t *table)
{
    render_dims_t d = {};

    const n00b_box_props_t *tp = table->table_props;

    if (tp && tp->border_theme) {
        d.has_top    = (tp->borders & N00B_BORDER_TOP) != 0;
        d.has_bottom = (tp->borders & N00B_BORDER_BOTTOM) != 0;
        d.has_left   = (tp->borders & N00B_BORDER_LEFT) != 0;
        d.has_right  = (tp->borders & N00B_BORDER_RIGHT) != 0;
        d.has_int_h  = (tp->borders & N00B_BORDER_INTERIOR_H) != 0;
        d.has_int_v  = (tp->borders & N00B_BORDER_INTERIOR_V) != 0;
    }

    n00b_isize_t outer_top = 0, outer_bottom = 0;
    n00b_isize_t outer_left = 0, outer_right = 0;

    if (tp) {
        n00b_box_insets(tp, &outer_top, &outer_bottom,
                         &outer_left, &outer_right);
    }

    n00b_isize_t num_cols = (n00b_isize_t)table->col_specs.len;

    // Sum column widths.
    int64_t col_sum = 0;
    for (n00b_isize_t i = 0; i < num_cols; i++) {
        col_sum += table->col_results[i].size;
    }

    // Interior vertical borders.
    n00b_isize_t int_v_total = d.has_int_v ? (num_cols - 1) : 0;

    // Sum row heights.
    n00b_isize_t n_rows = (n00b_isize_t)table->rows.len;
    int64_t row_sum = 0;

    for (n00b_isize_t i = 0; i < n_rows; i++) {
        row_sum += table->row_heights[i];
    }

    // Interior horizontal borders.
    n00b_isize_t int_h_total = d.has_int_h ? (n_rows > 0 ? n_rows - 1 : 0) : 0;

    // Title and caption.
    d.title_h   = (table->title.u8_bytes > 0) ? 1 : 0;
    d.caption_h = (table->caption.u8_bytes > 0) ? 1 : 0;

    // Total dimensions.
    d.total_w = (n00b_isize_t)(outer_left + col_sum + int_v_total + outer_right);
    d.total_h = (n00b_isize_t)(outer_top + d.title_h + row_sum
                                + int_h_total + d.caption_h + outer_bottom);

    d.content_x = outer_left;
    d.content_y = outer_top;
    d.data_y    = outer_top + d.title_h;

    return d;
}

// ====================================================================
// Internal: get column start position in content coordinates
// ====================================================================

static n00b_isize_t
col_start_x(n00b_table_t *table, n00b_isize_t col, bool has_int_v,
             n00b_isize_t content_x)
{
    n00b_isize_t x = content_x;

    for (n00b_isize_t i = 0; i < col; i++) {
        x += (n00b_isize_t)table->col_results[i].size;
        if (has_int_v) {
            x++; // Interior vertical border.
        }
    }

    return x;
}

// ====================================================================
// Internal: get row start position in grid coordinates
// ====================================================================

static n00b_isize_t
row_start_y(n00b_table_t *table, n00b_isize_t row, bool has_int_h,
             n00b_isize_t data_y)
{
    n00b_isize_t y = data_y;

    for (n00b_isize_t i = 0; i < row; i++) {
        y += (n00b_isize_t)table->row_heights[i];
        if (has_int_h) {
            y++; // Interior horizontal border.
        }
    }

    return y;
}

// ====================================================================
// Phase 4: stamp outer box
// ====================================================================

static void
render_outer_box(n00b_table_t *table, n00b_plane_t *plane, render_dims_t *d)
{
    if (!table->table_props || !table->table_props->border_theme) {
        return;
    }

    n00b_box_stamp(table->table_props,
                    plane->grid,
                    plane->total_cols,
                    0, 0,
                    d->total_h, d->total_w,
                    table->table_props->border_style,
                    table->table_props->fill_style);
}

// ====================================================================
// Phase 5: draw interior borders
// ====================================================================

static void
render_interior_borders(n00b_table_t *table, n00b_plane_t *plane,
                          render_dims_t *d)
{
    const n00b_box_props_t    *tp    = table->table_props;
    const n00b_border_theme_t *theme = tp ? tp->border_theme : nullptr;
    n00b_text_style_t         *bstyle  = tp ? tp->border_style : nullptr;
    n00b_isize_t               grid_w  = plane->total_cols;
    n00b_rcell_t              *grid    = plane->grid;
    n00b_isize_t               n_rows  = (n00b_isize_t)table->rows.len;
    n00b_isize_t               n_cols  = (n00b_isize_t)table->col_specs.len;

    // Interior vertical lines.
    if (d->has_int_v && theme) {
        for (n00b_isize_t col = 1; col < n_cols; col++) {
            n00b_isize_t x = col_start_x(table, col, true, d->content_x) - 1;

            // Draw from top of data to bottom of data.
            for (n00b_isize_t r = 0; r < n_rows; r++) {
                n00b_isize_t y  = row_start_y(table, r, d->has_int_h, d->data_y);
                n00b_isize_t rh = (n00b_isize_t)table->row_heights[r];

                for (n00b_isize_t dy = 0; dy < rh; dy++) {
                    stamp_border(grid_at(grid, grid_w, y + dy, x),
                                  theme->vertical, bstyle);
                }
            }
        }
    }

    // Interior horizontal lines.
    if (d->has_int_h && theme) {
        for (n00b_isize_t r = 0; r < n_rows - 1; r++) {
            n00b_isize_t y = row_start_y(table, r, true, d->data_y)
                             + (n00b_isize_t)table->row_heights[r];

            // Fill the horizontal line.
            n00b_isize_t x_start = d->content_x;
            n00b_isize_t x_end   = d->content_x;

            for (n00b_isize_t c = 0; c < n_cols; c++) {
                x_end += (n00b_isize_t)table->col_results[c].size;
                if (d->has_int_v && c < n_cols - 1) {
                    x_end++;
                }
            }

            for (n00b_isize_t x = x_start; x < x_end; x++) {
                stamp_border(grid_at(grid, grid_w, y, x),
                              theme->horizontal, bstyle);
            }

            // Crosses at column intersections.
            if (d->has_int_v) {
                for (n00b_isize_t col = 1; col < n_cols; col++) {
                    n00b_isize_t cx = col_start_x(table, col, true,
                                                    d->content_x) - 1;
                    stamp_border(grid_at(grid, grid_w, y, cx),
                                  theme->cross, bstyle);
                }
            }

            // T-junctions at outer edges.
            if (d->has_left) {
                n00b_isize_t lx = d->content_x - 1;
                if (lx < grid_w) {
                    stamp_border(grid_at(grid, grid_w, y, lx),
                                  theme->left_t, bstyle);
                }
            }
            if (d->has_right) {
                n00b_isize_t rx = x_end;
                if (rx < grid_w) {
                    stamp_border(grid_at(grid, grid_w, y, rx),
                                  theme->right_t, bstyle);
                }
            }
        }
    }

    // T-junctions along top and bottom borders for interior vertical lines.
    if (d->has_int_v && theme) {
        for (n00b_isize_t col = 1; col < n_cols; col++) {
            n00b_isize_t x = col_start_x(table, col, true, d->content_x) - 1;

            if (d->has_top) {
                stamp_border(grid_at(grid, grid_w, 0, x),
                              theme->top_t, bstyle);
            }
            if (d->has_bottom) {
                stamp_border(grid_at(grid, grid_w, d->total_h - 1, x),
                              theme->bottom_t, bstyle);
            }
        }
    }
}

// ====================================================================
// Phase 6: fill cell content
// ====================================================================

static void
render_cell_content(n00b_table_t *table, n00b_plane_t *plane,
                      render_dims_t *d)
{
    n00b_isize_t n_rows = (n00b_isize_t)table->rows.len;
    n00b_isize_t n_cols = (n00b_isize_t)table->col_specs.len;

    for (n00b_isize_t r = 0; r < n_rows; r++) {
        n00b_table_row_t *row = render_visible_row(table, r);
        n00b_isize_t      gy  = row_start_y(table, r, d->has_int_h, d->data_y);
        n00b_isize_t      rh  = (n00b_isize_t)table->row_heights[r];
        n00b_isize_t      col_ix = 0;

        for (size_t ci = 0; ci < row->cells.len; ci++) {
            n00b_table_cell_t *cell = &row->cells.data[ci];
            int32_t            span = cell->col_span;

            if (span <= 0) {
                span = 1;
            }

            // Compute cell geometry.
            n00b_isize_t gx = col_start_x(table, col_ix, d->has_int_v,
                                            d->content_x);

            int64_t cell_w = 0;
            for (int32_t s = 0; s < span && (col_ix + s) < n_cols; s++) {
                cell_w += table->col_results[col_ix + s].size;
            }
            if (d->has_int_v && span > 1) {
                cell_w += (span - 1);
            }

            // Resolve cell style.
            const n00b_box_props_t *cprops =
                _n00b_table_resolve_cell_props(table, r, col_ix, cell);

            int8_t  pl = cprops->pad_left;
            int8_t  pr = cprops->pad_right;
            int8_t  pt = cprops->pad_top;
            int8_t  pb = cprops->pad_bottom;
            int64_t cw = cell_w - pl - pr;
            int64_t ch = rh - pt - pb;

            if (cw < 1) {
                cw = 1;
            }
            if (ch < 1) {
                ch = 1;
            }

            // Wrap (or truncate) content.
            int64_t                  num_lines = 0;
            n00b_array_t(n00b_string_t) lines = {};

            if (cell->content.u8_bytes > 0) {
                if (cell_should_wrap(table, cell)) {
                    lines     = n00b_unicode_str_wrap(cell->content,
                                                       .width = (int32_t)cw);
                    num_lines = (int64_t)n00b_array_len(lines);
                }
                else {
                    // No wrap: split on hard newlines, truncate each.
                    n00b_array_t(n00b_string_t) hard_lines =
                        n00b_unicode_str_split_lines(cell->content);
                    n00b_isize_t n_hard = n00b_array_len(hard_lines);

                    lines = n00b_array_new(n00b_string_t, n_hard);

                    for (n00b_isize_t li = 0; li < n_hard; li++) {
                        n00b_string_t raw = n00b_array_get(hard_lines, li);
                        n00b_string_t trunc =
                            n00b_unicode_str_truncate(raw,
                                                       (int32_t)cw);
                        n00b_array_set(lines, li, trunc);
                    }

                    num_lines = (int64_t)n_hard;
                    n00b_array_free(hard_lines);
                }
            }

            // Vertical alignment.
            int64_t v_offset = 0;
            n00b_alignment_t align = cprops->alignment;

            if (align & N00B_ALIGN_MIDDLE) {
                v_offset = (ch - num_lines) / 2;
            }
            else if (align & N00B_ALIGN_BOTTOM) {
                v_offset = ch - num_lines;
            }

            if (v_offset < 0) {
                v_offset = 0;
            }

            // Fill cell area with background color.
            if (cprops->fill_style) {
                n00b_plane_fill_rect(plane, gy, gx, rh, (n00b_isize_t)cell_w,
                                      .style = cprops->fill_style);
            }

            // Write each line.
            for (int64_t li = 0; li < num_lines && li < ch; li++) {
                n00b_string_t line = n00b_array_get(lines, (size_t)li);

                // Horizontal alignment.
                n00b_string_t padded;

                if (align & N00B_ALIGN_CENTER) {
                    padded = n00b_unicode_str_center(line, (int32_t)cw);
                }
                else if (align & N00B_ALIGN_RIGHT) {
                    padded = n00b_unicode_str_pad_left(line, (int32_t)cw);
                }
                else {
                    padded = n00b_unicode_str_pad_right(line, (int32_t)cw);
                }

                // Apply text style from cell props.  When both
                // text_style and fill_style exist, merge them so text
                // cells carry the fill background + the text foreground.
                if (cprops->text_style && cprops->fill_style) {
                    n00b_text_style_t *merged =
                        n00b_str_style_merge(cprops->fill_style,
                                              cprops->text_style);
                    padded = n00b_str_set_base_style(padded, merged);
                }
                else if (cprops->text_style) {
                    padded = n00b_str_set_base_style(padded, cprops->text_style);
                }
                else if (cprops->fill_style) {
                    padded = n00b_str_set_base_style(padded, cprops->fill_style);
                }

                n00b_isize_t write_y = gy + pt + (n00b_isize_t)v_offset
                                       + (n00b_isize_t)li;
                n00b_isize_t write_x = gx + pl;

                n00b_plane_put_str_at(plane, write_y, write_x, padded);
            }

            if (num_lines > 0) {
                n00b_array_free(lines);
            }

            col_ix += (n00b_isize_t)span;
        }
    }
}

// ====================================================================
// Phase 7: title and caption
// ====================================================================

static void
render_title_caption(n00b_table_t *table, n00b_plane_t *plane,
                       render_dims_t *d)
{
    if (table->title.u8_bytes > 0 && d->title_h > 0) {
        // Title goes in the first content row.
        n00b_isize_t title_y = d->content_y;
        n00b_isize_t title_x = d->content_x;
        n00b_isize_t title_w = d->total_w - d->content_x;

        // Subtract right inset.
        if (d->has_right) {
            title_w--;
        }
        if (table->table_props) {
            title_w -= table->table_props->pad_right;
        }

        n00b_string_t centered =
            n00b_unicode_str_center(table->title, (int32_t)title_w);

        n00b_plane_put_str_at(plane, title_y, title_x, centered);
    }

    if (table->caption.u8_bytes > 0 && d->caption_h > 0) {
        // Caption goes in the last content row.
        n00b_isize_t cap_y = d->total_h - 1;

        if (d->has_bottom) {
            cap_y--;
        }
        if (table->table_props) {
            cap_y -= table->table_props->pad_bottom;
        }

        n00b_isize_t cap_x = d->content_x;
        n00b_isize_t cap_w = d->total_w - d->content_x;

        if (d->has_right) {
            cap_w--;
        }
        if (table->table_props) {
            cap_w -= table->table_props->pad_right;
        }

        n00b_string_t centered =
            n00b_unicode_str_center(table->caption, (int32_t)cap_w);

        n00b_plane_put_str_at(plane, cap_y, cap_x, centered);
    }
}

// ====================================================================
// Public: render
// ====================================================================

n00b_plane_t *
n00b_table_render(n00b_table_t *table) _kargs
{
    int64_t width = 80;
    bool    force = false;
}
{
    assert(table);
    if (table->rows.len == 0) {
        return nullptr;
    }

    // Compute layout if needed.
    if (!table->layout_valid || force || table->total_width != width) {
        _n00b_table_compute_layout(table, width);
    }

    // Compute dimensions.
    render_dims_t d = compute_dimensions(table);

    if (d.total_w == 0 || d.total_h == 0) {
        return nullptr;
    }

    // Create or resize the plane.
    if (table->plane) {
        if (table->plane->total_cols != d.total_w
            || table->plane->total_rows != d.total_h) {
            n00b_plane_resize(table->plane, d.total_h, d.total_w);
        }
        n00b_plane_clear(table->plane);
    }
    else {
        table->plane = n00b_new_kargs(n00b_plane_t, plane,
                                       .cols      = d.total_w,
                                       .rows      = d.total_h,
                                       .name      = n00b_option_set(n00b_string_t, *r"table"),
                                       .allocator = table->allocator);
    }

    // Render phases.
    render_outer_box(table, table->plane, &d);
    render_interior_borders(table, table->plane, &d);
    render_cell_content(table, table->plane, &d);
    render_title_caption(table, table->plane, &d);

    return table->plane;
}
