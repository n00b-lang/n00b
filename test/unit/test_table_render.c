#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "render/plane.h"
#include "render/cell.h"
#include "render/types.h"
#include "render/box.h"
#include "table/table.h"

// ====================================================================
// Helpers
// ====================================================================

static n00b_string_t
make_str(const char *s)
{
    return n00b_string_from_raw(nullptr, s, (int64_t)strlen(s),
                                 (int64_t)strlen(s));
}

/**
 * Check that a cell at grid (row, col) contains the expected ASCII char.
 */
static bool
grid_char_eq(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col, char ch)
{
    if (row >= p->total_rows || col >= p->total_cols) {
        return false;
    }

    n00b_rcell_t *cell = &p->grid[row * p->total_cols + col];

    return (cell->grapheme_len == 1 && cell->grapheme[0] == ch);
}

/**
 * Check that a cell has the border flag set.
 */
static bool
grid_is_border(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    if (row >= p->total_rows || col >= p->total_cols) {
        return false;
    }

    n00b_rcell_t *cell = &p->grid[row * p->total_cols + col];

    return (cell->flags & N00B_CELL_BORDER) != 0;
}

// ====================================================================
// Tests
// ====================================================================

static void
test_basic_render(void)
{
    n00b_table_t *t = n00b_table_new(.num_cols = 2);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);
    assert(p->total_rows > 0);
    assert(p->total_cols > 0);

    n00b_table_destroy(t);
    printf("  [PASS] basic render returns plane\n");
}

static void
test_render_with_border(void)
{
    n00b_table_style_t style = n00b_table_style_ascii();
    n00b_table_t      *t     = n00b_table_new(
        .num_cols    = 2,
        .table_props = style.table_props,
        .cell_props  = style.cell_props);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);

    // Top-left corner should be a border cell (ASCII: '/').
    assert(grid_is_border(p, 0, 0));

    // Top-right corner should be a border cell.
    assert(grid_is_border(p, 0, p->total_cols - 1));

    // Bottom-left corner.
    assert(grid_is_border(p, p->total_rows - 1, 0));

    n00b_table_destroy(t);
    printf("  [PASS] render with ASCII border\n");
}

static void
test_render_interior_borders(void)
{
    n00b_box_props_t *tp = n00b_box_props_new(
        .theme   = &n00b_border_ascii,
        .borders = N00B_BORDER_ALL);

    n00b_table_t *t = n00b_table_new(
        .num_cols    = 2,
        .table_props = tp,
        .cell_props  = n00b_box_props_new(
            .borders   = N00B_BORDER_NONE,
            .pad_left  = 0,
            .pad_right = 0));

    n00b_table_add_cell(t, make_str("X"));
    n00b_table_add_cell(t, make_str("Y"));
    n00b_table_end_row(t);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);

    // Should have at least 4 rows: top border, row0, interior-H, row1, bottom border.
    assert(p->total_rows >= 4);

    n00b_table_destroy(t);
    printf("  [PASS] render with interior borders\n");
}

static void
test_render_multiline(void)
{
    n00b_table_t *t = n00b_table_new(.num_cols = 1);

    n00b_table_add_cell(t, make_str("Line one\nLine two\nLine three"));
    n00b_table_end_row(t);

    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);
    // Row should be at least 3 lines tall.
    assert(p->total_rows >= 3);

    n00b_table_destroy(t);
    printf("  [PASS] render multiline content\n");
}

static void
test_render_empty_table(void)
{
    n00b_table_t *t = n00b_table_new();

    n00b_plane_t *p = n00b_table_render(t, 40);

    // Empty table should return nullptr.
    assert(p == nullptr);

    n00b_table_destroy(t);
    printf("  [PASS] render empty table returns nullptr\n");
}

static void
test_render_col_span(void)
{
    n00b_box_props_t *tp = n00b_box_props_new(
        .theme   = &n00b_border_ascii,
        .borders = N00B_BORDER_ALL);

    n00b_table_t *t = n00b_table_new(
        .num_cols    = 3,
        .table_props = tp,
        .cell_props  = n00b_box_props_new(
            .borders   = N00B_BORDER_NONE,
            .pad_left  = 0,
            .pad_right = 0));

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_add_cell(t, make_str("C"));
    n00b_table_end_row(t);

    // Second row: first cell spans 2 columns.
    n00b_table_add_cell(t, make_str("Wide"), .col_span = 2);
    n00b_table_add_cell(t, make_str("Z"));
    n00b_table_end_row(t);

    n00b_plane_t *p = n00b_table_render(t, 40);

    assert(p != nullptr);
    assert(p->total_rows > 0);

    n00b_table_destroy(t);
    printf("  [PASS] render with col_span\n");
}

static void
test_rerender_reuses_plane(void)
{
    n00b_table_t *t = n00b_table_new(.num_cols = 2);

    n00b_table_add_cell(t, make_str("A"));
    n00b_table_add_cell(t, make_str("B"));
    n00b_table_end_row(t);

    n00b_plane_t *p1 = n00b_table_render(t, 40);
    n00b_plane_t *p2 = n00b_table_render(t, 40);

    // Same width, cached: should reuse the same plane.
    assert(p1 == p2);

    n00b_table_destroy(t);
    printf("  [PASS] re-render reuses plane\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running table render tests...\n");

    test_basic_render();
    test_render_with_border();
    test_render_interior_borders();
    test_render_multiline();
    test_render_empty_table();
    test_render_col_span();
    test_rerender_reuses_plane();

    printf("All table render tests passed.\n");
    return 0;
}
