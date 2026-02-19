/**
 * Table construction, destruction, cell/row insertion, and column spec API.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "strings/string_ops.h"
#include "table/table.h"

// ====================================================================
// Internal constants
// ====================================================================

#define INITIAL_ROW_CAP  16
#define INITIAL_CELL_CAP 8
#define INITIAL_COL_CAP  8

// ====================================================================
// Internal: default box props (fallback for style cascade)
// ====================================================================

static n00b_box_props_t _default_cell_props = {
    .border_theme = nullptr,
    .borders      = N00B_BORDER_NONE,
    .alignment    = N00B_ALIGN_TOP_LEFT,
    .pad_left     = 1,
    .pad_right    = 1,
};

// ====================================================================
// Internal: grow helpers
// ====================================================================

static void
grow_rows(n00b_table_t *table)
{
    n00b_isize_t new_cap = table->rows_cap * 2;

    if (new_cap < INITIAL_ROW_CAP) {
        new_cap = INITIAL_ROW_CAP;
    }

    n00b_table_row_t *new_rows =
        n00b_alloc_array(n00b_table_row_t, new_cap,
                          .allocator = table->allocator);

    if (table->rows && table->num_rows > 0) {
        memcpy(new_rows, table->rows,
               table->num_rows * sizeof(n00b_table_row_t));
        n00b_free(table->rows);
    }

    table->rows     = new_rows;
    table->rows_cap = new_cap;
}

static void
grow_current_row(n00b_table_t *table)
{
    n00b_table_row_t *row     = &table->current_row;
    n00b_isize_t      new_cap = row->cells_cap * 2;

    if (new_cap < INITIAL_CELL_CAP) {
        new_cap = INITIAL_CELL_CAP;
    }

    n00b_table_cell_t *new_cells =
        n00b_alloc_array(n00b_table_cell_t, new_cap,
                          .allocator = table->allocator);

    if (row->cells && row->num_cells > 0) {
        memcpy(new_cells, row->cells,
               row->num_cells * sizeof(n00b_table_cell_t));
        n00b_free(row->cells);
    }

    row->cells     = new_cells;
    row->cells_cap = new_cap;
}

static void
grow_col_specs(n00b_table_t *table)
{
    n00b_isize_t new_cap = table->cols_cap * 2;

    if (new_cap < INITIAL_COL_CAP) {
        new_cap = INITIAL_COL_CAP;
    }

    n00b_table_col_spec_t *new_specs =
        n00b_alloc_array(n00b_table_col_spec_t, new_cap,
                          .allocator = table->allocator);

    if (table->col_specs && table->num_cols > 0) {
        memcpy(new_specs, table->col_specs,
               table->num_cols * sizeof(n00b_table_col_spec_t));
        n00b_free(table->col_specs);
    }

    table->col_specs = new_specs;
    table->cols_cap  = new_cap;
}

static n00b_isize_t
add_col_spec(n00b_table_t *table, n00b_table_col_spec_t spec)
{
    if (table->num_cols >= table->cols_cap) {
        grow_col_specs(table);
    }

    n00b_isize_t idx           = table->num_cols;
    table->col_specs[idx]      = spec;
    table->num_cols++;

    return idx;
}

// Count how many logical columns the current row's cells consume.
static n00b_isize_t
current_row_col_count(n00b_table_t *table)
{
    n00b_isize_t count = 0;

    for (n00b_isize_t i = 0; i < table->current_row.num_cells; i++) {
        int32_t span = table->current_row.cells[i].col_span;
        count += (span > 0) ? (n00b_isize_t)span : 1;
    }

    return count;
}

// ====================================================================
// Construction / destruction
// ====================================================================

n00b_table_t *
n00b_table_new() _kargs
{
    n00b_isize_t       num_cols     = 0;
    n00b_box_props_t  *table_props  = nullptr;
    n00b_box_props_t  *cell_props   = nullptr;
    n00b_box_props_t  *header_props = nullptr;
    n00b_box_props_t  *alt_props    = nullptr;
    n00b_string_t     *title        = nullptr;
    n00b_string_t     *caption      = nullptr;
    n00b_isize_t       max_rows     = 0;
    bool               wrap         = true;
    n00b_allocator_t  *allocator    = nullptr;
}
{
    n00b_table_t *table = n00b_alloc(n00b_table_t, .allocator = allocator);

    table->allocator          = allocator;
    table->table_props        = table_props;
    table->default_cell_props = cell_props;
    table->header_props       = header_props;
    table->alt_cell_props     = alt_props;
    table->max_rows           = max_rows;
    table->wrap               = wrap;

    if (title) {
        table->title = *title;
    }
    else {
        table->title = n00b_string_empty(allocator);
    }

    if (caption) {
        table->caption = *caption;
    }
    else {
        table->caption = n00b_string_empty(allocator);
    }

    if (num_cols > 0) {
        table->cols_cap  = (n00b_isize_t)num_cols;
        table->col_specs = n00b_alloc_array(n00b_table_col_spec_t,
                                             num_cols,
                                             .allocator = allocator);
        // Default all columns to FIT mode.
        for (n00b_isize_t i = 0; i < num_cols; i++) {
            table->col_specs[i].mode          = N00B_COL_FIT;
            table->col_specs[i].flex_multiple = 1;
        }
        table->num_cols = num_cols;
    }

    if (max_rows > 0) {
        table->rows_cap = max_rows;
        table->rows     = n00b_alloc_array(n00b_table_row_t, max_rows,
                                            .allocator = allocator);
    }

    return table;
}

void
n00b_table_destroy(n00b_table_t *table)
{
    if (!table) {
        return;
    }

    // Free each row's cell array.
    for (n00b_isize_t i = 0; i < table->num_rows; i++) {
        if (table->rows[i].cells) {
            n00b_free(table->rows[i].cells);
        }
    }

    if (table->rows) {
        n00b_free(table->rows);
    }

    if (table->col_specs) {
        n00b_free(table->col_specs);
    }

    if (table->col_results) {
        n00b_free(table->col_results);
    }

    if (table->row_heights) {
        n00b_free(table->row_heights);
    }

    if (table->current_row.cells) {
        n00b_free(table->current_row.cells);
    }

    n00b_free(table);
}

// ====================================================================
// Cell / row insertion
// ====================================================================

void
n00b_table_add_cell(n00b_table_t *table, n00b_string_t content) _kargs
{
    int32_t           col_span   = 1;
    int32_t           row_span   = 1;
    n00b_box_props_t *cell_props = nullptr;
    n00b_tristate_t   wrap       = N00B_TRI_UNSPECIFIED;
}
{
    assert(row_span == 1 && "multi-row spanning not yet supported");

    if (table->current_row.num_cells >= table->current_row.cells_cap) {
        grow_current_row(table);
    }

    n00b_table_cell_t *cell = &table->current_row.cells[table->current_row.num_cells];

    cell->content    = content;
    cell->cell_props = cell_props;
    cell->col_span   = col_span;
    cell->row_span   = row_span;
    cell->wrap       = wrap;

    table->current_row.num_cells++;
}

void
n00b_table_empty_cell(n00b_table_t *table)
{
    n00b_string_t empty = n00b_string_empty(table->allocator);

    n00b_table_add_cell(table, empty);
}

void
n00b_table_end_row(n00b_table_t *table)
{
    n00b_table_row_t *row = &table->current_row;

    if (row->num_cells == 0) {
        return;
    }

    // First row: auto-detect column count if not set.
    if (!table->cols_locked) {
        n00b_isize_t row_cols = current_row_col_count(table);

        if (table->num_cols == 0) {
            // Auto-detect: create default FIT specs.
            while (table->num_cols < row_cols) {
                n00b_table_col_spec_t spec = {
                    .mode          = N00B_COL_FIT,
                    .flex_multiple = 1,
                };
                add_col_spec(table, spec);
            }
        }

        table->cols_locked = true;
    }

    // Resolve span-all (-1) cells: replace with actual span.
    n00b_isize_t used_cols = 0;

    for (n00b_isize_t i = 0; i < row->num_cells; i++) {
        if (row->cells[i].col_span == -1) {
            int32_t remaining = (int32_t)(table->num_cols - used_cols);
            row->cells[i].col_span = (remaining > 0) ? remaining : 1;
        }
        used_cols += (n00b_isize_t)row->cells[i].col_span;
    }

    // Commit the row.
    if (table->max_rows > 0) {
        // Ring buffer mode.
        n00b_isize_t slot = table->total_added % table->max_rows;

        // Free old row's cells if we're overwriting a full ring.
        if (table->num_rows >= table->max_rows && table->rows[slot].cells) {
            n00b_free(table->rows[slot].cells);
        }

        table->rows[slot] = *row;

        if (table->num_rows < table->max_rows) {
            table->num_rows++;
        }
        else {
            table->ring_base = (table->total_added + 1) % table->max_rows;
        }
    }
    else {
        // Unlimited mode: grow array as needed.
        if (table->num_rows >= table->rows_cap) {
            grow_rows(table);
        }

        table->rows[table->num_rows] = *row;
        table->num_rows++;
    }

    table->total_added++;
    table->layout_valid = false;

    // Reset current row (don't free the cells — they've been moved to rows[]).
    table->current_row = (n00b_table_row_t){};
}

void
n00b_table_add_row(n00b_table_t *table, n00b_string_t *cells, n00b_isize_t n)
{
    for (n00b_isize_t i = 0; i < n; i++) {
        n00b_table_add_cell(table, cells[i]);
    }

    n00b_table_end_row(table);
}

void
n00b_table_end(n00b_table_t *table)
{
    if (table->current_row.num_cells > 0) {
        n00b_table_end_row(table);
    }
}

// ====================================================================
// Column specification
// ====================================================================

n00b_isize_t
n00b_table_col_fit(n00b_table_t *table)
{
    n00b_table_col_spec_t spec = {
        .mode          = N00B_COL_FIT,
        .flex_multiple = 1,
    };

    return add_col_spec(table, spec);
}

n00b_isize_t
n00b_table_col_flex(n00b_table_t *table, int64_t factor)
{
    n00b_table_col_spec_t spec = {
        .mode          = N00B_COL_FLEX,
        .flex_multiple = factor > 0 ? factor : 1,
    };

    return add_col_spec(table, spec);
}

n00b_isize_t
n00b_table_col_range(n00b_table_t *table, int64_t min, int64_t max)
{
    n00b_table_col_spec_t spec = {
        .mode          = N00B_COL_FIT,
        .min           = { .value.i = min },
        .max           = { .value.i = max },
        .flex_multiple = 1,
    };

    return add_col_spec(table, spec);
}

n00b_isize_t
n00b_table_col_pct(n00b_table_t *table, double min, double max)
{
    n00b_table_col_spec_t spec = {
        .mode          = N00B_COL_FIT,
        .min           = { .value.d = min, .pct = true },
        .max           = { .value.d = max, .pct = true },
        .flex_multiple = 1,
    };

    return add_col_spec(table, spec);
}

n00b_isize_t
n00b_table_col_fixed(n00b_table_t *table, int64_t width)
{
    n00b_table_col_spec_t spec = {
        .mode = N00B_COL_FIXED,
        .min  = { .value.i = width },
        .max  = { .value.i = width },
        .pref = { .value.i = width },
    };

    return add_col_spec(table, spec);
}

void
n00b_table_set_col_priority(n00b_table_t *table, n00b_isize_t col,
                              int64_t priority)
{
    assert(col < table->num_cols);
    table->col_specs[col].priority = priority;
}

void
n00b_table_set_col_props(n00b_table_t *table, n00b_isize_t col,
                           n00b_box_props_t *col_props)
{
    assert(col < table->num_cols);
    table->col_specs[col].col_props = col_props;
}

// ====================================================================
// Layout invalidation
// ====================================================================

void
n00b_table_invalidate(n00b_table_t *table)
{
    table->layout_valid = false;
}

// ====================================================================
// Style cascade resolution
// ====================================================================

const n00b_box_props_t *
_n00b_table_resolve_cell_props(const n00b_table_t      *table,
                                n00b_isize_t             row_idx,
                                n00b_isize_t             col_idx,
                                const n00b_table_cell_t *cell)
{
    // 1. Per-cell override.
    if (cell && cell->cell_props) {
        return cell->cell_props;
    }

    // 2. Per-column style.
    if (col_idx < table->num_cols && table->col_specs[col_idx].col_props) {
        return table->col_specs[col_idx].col_props;
    }

    // 3. Row-based: header (row 0), alternating odd rows.
    if (row_idx == 0 && table->header_props) {
        return table->header_props;
    }

    if ((row_idx & 1) && table->alt_cell_props) {
        return table->alt_cell_props;
    }

    // 4. Default cell props.
    if (table->default_cell_props) {
        return table->default_cell_props;
    }

    return &_default_cell_props;
}

// ====================================================================
// Convenience: build table from delimited string
// ====================================================================

n00b_table_t *
n00b_table_from_string(n00b_string_t s) _kargs
{
    n00b_string_t    *row_sep      = nullptr;
    n00b_string_t    *col_sep      = nullptr;
    n00b_box_props_t *table_props  = nullptr;
    n00b_box_props_t *cell_props   = nullptr;
    n00b_box_props_t *header_props = nullptr;
    n00b_box_props_t *alt_props    = nullptr;
    n00b_bool32_t     no_stripe    = 0;
    n00b_allocator_t *allocator    = nullptr;
}
{
    if (no_stripe) {
        alt_props = nullptr;
    }

    n00b_string_t default_row_sep = n00b_string_from_raw(allocator,
                                                          "\n", 1, 1);
    n00b_string_t default_col_sep = n00b_string_from_raw(allocator,
                                                          ",", 1, 1);

    n00b_string_t rsep = row_sep ? *row_sep : default_row_sep;
    n00b_string_t csep = col_sep ? *col_sep : default_col_sep;

    n00b_array_t(n00b_string_t) rows = n00b_unicode_str_split(s, rsep);

    n00b_table_t *table = n00b_table_new(
        .table_props  = table_props,
        .cell_props   = cell_props,
        .header_props = header_props,
        .alt_props    = alt_props,
        .allocator    = allocator);

    n00b_isize_t num_rows = n00b_array_len(rows);

    for (n00b_isize_t r = 0; r < num_rows; r++) {
        n00b_string_t row_str = n00b_array_get(rows, r);

        // Skip trailing empty rows (common from trailing newline).
        if (row_str.u8_bytes == 0 && r == num_rows - 1) {
            break;
        }

        n00b_array_t(n00b_string_t) cols = n00b_unicode_str_split(row_str,
                                                                    csep);
        n00b_isize_t num_cols = n00b_array_len(cols);

        for (n00b_isize_t c = 0; c < num_cols; c++) {
            n00b_table_add_cell(table, n00b_array_get(cols, c));
        }

        n00b_array_free(cols);
        n00b_table_end_row(table);
    }

    n00b_array_free(rows);
    n00b_table_end(table);

    return table;
}
