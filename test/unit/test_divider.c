/*
 * Unit tests for the divider widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/divider.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static char
cell_char(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    n00b_option_t(n00b_const_rcell_ptr_t) opt = n00b_plane_get_cell(p, row, col);
    if (!n00b_option_is_set(opt)) {
        return '\0';
    }
    const n00b_rcell_t *cell = n00b_option_get(opt);
    if (cell->grapheme_len == 0) {
        return '\0';
    }
    return cell->grapheme[0];
}

static bool
cell_occupied(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    n00b_option_t(n00b_const_rcell_ptr_t) opt = n00b_plane_get_cell(p, row, col);
    if (!n00b_option_is_set(opt)) {
        return false;
    }
    const n00b_rcell_t *cell = n00b_option_get(opt);
    return (cell->flags & N00B_CELL_OCCUPIED) != 0;
}

// -------------------------------------------------------------------
// Test 1: Horizontal divider fills with line character
// -------------------------------------------------------------------

static void
test_divider_horizontal(void)
{
    n00b_plane_t *div = n00b_divider_new(.cols = 20);

    assert(div != nullptr);
    assert(div->widget_vtable == &n00b_widget_divider);
    assert(div->total_cols == 20);
    assert(div->total_rows == 1);

    // All cells should be occupied.
    for (n00b_isize_t c = 0; c < 20; c++) {
        assert(cell_occupied(div, 0, c));
    }

    printf("  [PASS] horizontal divider\n");
    n00b_plane_destroy(div);
}

// -------------------------------------------------------------------
// Test 2: Vertical divider
// -------------------------------------------------------------------

static void
test_divider_vertical(void)
{
    n00b_plane_t *div = n00b_divider_new(.vertical = true,
                                           .rows = 5, .cols = 1);

    assert(div != nullptr);
    assert(div->total_cols == 1);
    assert(div->total_rows == 5);

    // All cells in column 0 should be occupied.
    for (n00b_isize_t r = 0; r < 5; r++) {
        assert(cell_occupied(div, r, 0));
    }

    printf("  [PASS] vertical divider\n");
    n00b_plane_destroy(div);
}

// -------------------------------------------------------------------
// Test 3: Divider with label
// -------------------------------------------------------------------

static void
test_divider_label(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Title");
    n00b_plane_t  *div   = n00b_divider_new(.cols = 30, .label = label);

    assert(div != nullptr);
    assert(div->total_cols == 30);

    // The label "Title" should appear somewhere in the middle.
    bool found_T = false;
    for (n00b_isize_t c = 0; c < 30; c++) {
        if (cell_char(div, 0, c) == 'T') {
            found_T = true;
            break;
        }
    }
    assert(found_T);

    printf("  [PASS] divider with label\n");
    n00b_plane_destroy(div);
}

// -------------------------------------------------------------------
// Test 4: Measure
// -------------------------------------------------------------------

static void
test_divider_measure(void)
{
    n00b_plane_t *div = n00b_divider_new(.cols = 20);

    n00b_isize_t pref_cols, pref_rows, min_cols, min_rows;
    n00b_widget_measure(div, &pref_cols, &pref_rows, &min_cols, &min_rows);

    assert(pref_rows == 1);
    assert(min_cols >= 1);
    assert(min_rows == 1);

    printf("  [PASS] divider measure\n");
    n00b_plane_destroy(div);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running divider widget tests...\n");

    test_divider_horizontal();
    test_divider_vertical();
    test_divider_label();
    test_divider_measure();

    printf("All divider tests passed.\n");

    n00b_shutdown();
    return 0;
}
