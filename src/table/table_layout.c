/**
 * Table layout engine: content scanning, column width computation,
 * and row height calculation.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "core/layout.h"
#include "table/table.h"
#include "render/box.h"
#include "strings/string_ops.h"
#include "unicode/properties.h"

// ====================================================================
// Internal: content metrics
// ====================================================================

/**
 * Compute the display width of the longest line in a string.
 * Lines are split on CR/LF/CRLF boundaries.
 */
static int32_t
longest_line_width(n00b_string_t s)
{
    if (s.u8_bytes == 0) {
        return 0;
    }

    n00b_array_t(n00b_string_t) lines = n00b_unicode_str_split_lines(s);
    int32_t max_w = 0;

    n00b_array_foreach(lines, lp) {
        int32_t w = n00b_unicode_display_width(*lp);

        if (w > max_w) {
            max_w = w;
        }
    }

    n00b_array_free(lines);

    return max_w;
}

/**
 * Compute the display width of the longest word in a string.
 * Words are delimited by spaces.
 */
static int32_t
longest_word_width(n00b_string_t s)
{
    if (s.u8_bytes == 0) {
        return 0;
    }

    n00b_string_t space = n00b_string_from_raw(nullptr, " ", 1, 1);

    n00b_array_t(n00b_string_t) lines = n00b_unicode_str_split_lines(s);
    int32_t max_w = 0;

    n00b_array_foreach(lines, lp) {
        n00b_array_t(n00b_string_t) words =
            n00b_unicode_str_split(*lp, space);

        n00b_array_foreach(words, wp) {
            int32_t w = n00b_unicode_display_width(*wp);

            if (w > max_w) {
                max_w = w;
            }
        }

        n00b_array_free(words);
    }

    n00b_array_free(lines);

    return max_w;
}

// ====================================================================
// Internal: cell padding for layout computation
// ====================================================================

static void
get_cell_lr_padding(const n00b_table_t *table,
                     int8_t             *out_left,
                     int8_t             *out_right)
{
    const n00b_box_props_t *props = table->default_cell_props;

    if (!props) {
        // Fallback: use 1-cell padding on each side.
        *out_left  = 1;
        *out_right = 1;
        return;
    }

    *out_left  = props->pad_left;
    *out_right = props->pad_right;
}

static void
get_cell_tb_padding(const n00b_box_props_t *props,
                     int8_t                 *out_top,
                     int8_t                 *out_bottom)
{
    if (!props) {
        *out_top    = 0;
        *out_bottom = 0;
        return;
    }

    *out_top    = props->pad_top;
    *out_bottom = props->pad_bottom;
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
visible_row(n00b_table_t *table, n00b_isize_t vis_idx)
{
    if (table->max_rows > 0) {
        n00b_isize_t actual = (table->ring_base + vis_idx) % table->max_rows;
        return &table->rows[actual];
    }

    return &table->rows[vis_idx];
}

static inline n00b_isize_t
visible_row_count(n00b_table_t *table)
{
    return table->num_rows;
}

// ====================================================================
// Content-driven column preference scanning
// ====================================================================

static void
scan_column_preferences(n00b_table_t *table, int64_t width)
{
    int8_t cell_lpad, cell_rpad;
    get_cell_lr_padding(table, &cell_lpad, &cell_rpad);

    int32_t lr_pad = cell_lpad + cell_rpad;

    n00b_isize_t n_rows = visible_row_count(table);

    for (n00b_isize_t col = 0; col < table->num_cols; col++) {
        n00b_table_col_spec_t *spec = &table->col_specs[col];

        // Only scan content for FIT columns that don't already have pref set.
        if (spec->mode != N00B_COL_FIT || spec->pref.value.i > 0) {
            continue;
        }

        int32_t col_longest_line = 0;
        int32_t col_longest_word = 0;

        for (n00b_isize_t r = 0; r < n_rows; r++) {
            n00b_table_row_t *row = visible_row(table, r);

            // Find the cell that covers this column.
            n00b_isize_t cell_col = 0;

            for (n00b_isize_t ci = 0; ci < row->num_cells; ci++) {
                n00b_table_cell_t *cell = &row->cells[ci];
                int32_t            span = cell->col_span;

                if (span <= 0) {
                    span = 1;
                }

                if (cell_col + (n00b_isize_t)span > col && cell_col <= col) {
                    // This cell covers our column.
                    if (cell->content.u8_bytes > 0) {
                        int32_t ll = longest_line_width(cell->content);
                        int32_t lw;

                        if (cell_should_wrap(table, cell)) {
                            lw = longest_word_width(cell->content);
                        }
                        else {
                            // No wrap: minimum = full line width
                            // (can't break at word boundaries).
                            lw = ll;
                        }

                        // Divide by span for multi-column cells.
                        if (span > 1) {
                            ll /= span;
                            lw /= span;
                        }

                        if (ll > col_longest_line) {
                            col_longest_line = ll;
                        }
                        if (lw > col_longest_word) {
                            col_longest_word = lw;
                        }
                    }
                    break;
                }

                cell_col += (n00b_isize_t)span;
            }
        }

        if (col_longest_line == 0) {
            // No string content: make it flex.
            spec->mode          = N00B_COL_FLEX;
            spec->flex_multiple = 1;
            continue;
        }

        // Add cell padding to metrics.
        col_longest_line += lr_pad;
        col_longest_word += lr_pad;

        // Minimum: longest word (or existing min, whichever is larger).
        int64_t cur_min = n00b_layout_resolve_dim(&spec->min, width);
        if (col_longest_word > cur_min) {
            spec->min = (n00b_layout_dim_t){ .value.i = col_longest_word };
        }

        // Preferred: longest line (capped at total width).
        if (col_longest_line < width) {
            spec->pref = (n00b_layout_dim_t){ .value.i = col_longest_line };
            if (spec->max.value.i == 0 && !spec->max.pct) {
                spec->max = (n00b_layout_dim_t){ .value.i = col_longest_line };
            }
        }
        else {
            int64_t share = width / (int64_t)table->num_cols;
            int64_t p     = col_longest_word > share ? col_longest_word : share;
            spec->pref    = (n00b_layout_dim_t){ .value.i = p };
        }
    }
}

// ====================================================================
// Build layout items from column specs
// ====================================================================

static void
build_layout_items(n00b_table_t     *table,
                    n00b_layout_t    *items,
                    int64_t           width)
{
    for (n00b_isize_t i = 0; i < table->num_cols; i++) {
        n00b_table_col_spec_t *spec = &table->col_specs[i];

        items[i] = (n00b_layout_t){
            .min           = spec->min,
            .max           = spec->max,
            .pref          = spec->pref,
            .priority      = spec->priority,
            .flex_multiple = spec->flex_multiple,
        };
    }
}

// ====================================================================
// Row height computation
// ====================================================================

static void
compute_row_heights(n00b_table_t *table)
{
    n00b_isize_t n_rows = visible_row_count(table);

    for (n00b_isize_t r = 0; r < n_rows; r++) {
        n00b_table_row_t *row    = visible_row(table, r);
        int64_t           max_h  = 1; // At least 1 line.
        n00b_isize_t      col_ix = 0;

        for (n00b_isize_t ci = 0; ci < row->num_cells; ci++) {
            n00b_table_cell_t *cell = &row->cells[ci];

            // Compute the actual cell width from column results.
            int32_t span = cell->col_span;
            if (span <= 0) {
                span = 1;
            }

            int64_t cell_width = 0;
            for (int32_t s = 0; s < span && (col_ix + s) < table->num_cols; s++) {
                cell_width += table->col_results[col_ix + s].size;
            }

            // Add interior border gaps between spanned columns.
            bool has_interior_v =
                table->table_props
                && (table->table_props->borders & N00B_BORDER_INTERIOR_V);

            if (has_interior_v && span > 1) {
                cell_width += (span - 1);
            }

            // Subtract cell padding.
            const n00b_box_props_t *cprops =
                _n00b_table_resolve_cell_props(table, r, col_ix, cell);
            int8_t tb_top, tb_bot;
            get_cell_tb_padding(cprops, &tb_top, &tb_bot);

            int64_t content_w = cell_width - cprops->pad_left - cprops->pad_right;
            if (content_w < 1) {
                content_w = 1;
            }

            // Wrap (or don't) content and count lines.
            int64_t num_lines = 1;

            if (cell->content.u8_bytes > 0) {
                if (cell_should_wrap(table, cell)) {
                    n00b_array_t(n00b_string_t) wrapped =
                        n00b_unicode_str_wrap(cell->content,
                                              .width = (int32_t)content_w);
                    num_lines = (int64_t)n00b_array_len(wrapped);
                    n00b_array_free(wrapped);
                }
                else {
                    // No wrap: count only hard newlines.
                    n00b_array_t(n00b_string_t) hard_lines =
                        n00b_unicode_str_split_lines(cell->content);
                    num_lines = (int64_t)n00b_array_len(hard_lines);
                    n00b_array_free(hard_lines);
                }

                if (num_lines < 1) {
                    num_lines = 1;
                }
            }

            int64_t total_h = num_lines + tb_top + tb_bot;
            if (total_h > max_h) {
                max_h = total_h;
            }

            col_ix += (n00b_isize_t)span;
        }

        table->row_heights[r] = max_h;
    }
}

// ====================================================================
// Public: compute layout
// ====================================================================

void
_n00b_table_compute_layout(n00b_table_t *table, int64_t width)
{
    if (table->num_rows == 0) {
        table->layout_valid = true;
        return;
    }

    // Ensure col_specs are created for all columns.
    while (table->num_cols < table->cols_cap) {
        // Already allocated; stop if we have enough.
        break;
    }

    // Compute available width for columns.
    n00b_isize_t outer_left = 0, outer_right = 0;
    n00b_isize_t outer_top = 0, outer_bottom = 0;

    if (table->table_props) {
        n00b_box_insets(table->table_props,
                         &outer_top, &outer_bottom,
                         &outer_left, &outer_right);
    }

    bool has_interior_v =
        table->table_props
        && (table->table_props->borders & N00B_BORDER_INTERIOR_V);

    int64_t interior_v_total = has_interior_v ? (table->num_cols - 1) : 0;
    int64_t avail = width - outer_left - outer_right - interior_v_total;

    if (avail < (int64_t)table->num_cols) {
        avail = (int64_t)table->num_cols;
    }

    // Phase 1: scan content for FIT columns.
    scan_column_preferences(table, avail);

    // Phase 2: build layout items.
    n00b_layout_t *items =
        n00b_alloc_array(n00b_layout_t, table->num_cols,
                          .allocator = table->allocator);

    build_layout_items(table, items, avail);

    // Phase 3: run layout solver.
    if (table->col_results) {
        n00b_free(table->col_results);
    }

    table->col_results =
        n00b_alloc_array(n00b_layout_result_t, table->num_cols,
                          .allocator = table->allocator);

    n00b_layout_calculate(items, table->col_results,
                           table->num_cols, avail);

    n00b_free(items);

    // Phase 4: compute row heights.
    n00b_isize_t n_rows = visible_row_count(table);

    if (table->row_heights) {
        n00b_free(table->row_heights);
    }

    table->row_heights =
        n00b_alloc_array(int64_t, n_rows,
                          .allocator = table->allocator);

    compute_row_heights(table);

    table->total_width  = width;
    table->layout_valid = true;
}
