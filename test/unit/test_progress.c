/*
 * Unit tests for the progress bar widget.
 */

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/widget.h"
#include "display/widgets/progress.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

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
// Test 1: Default progress bar at 0%
// -------------------------------------------------------------------

static void
test_progress_zero(void)
{
    n00b_plane_t *bar = n00b_progress_new(.cols = 20);

    assert(bar != nullptr);
    assert(bar->widget_vtable == &n00b_widget_progress);
    assert(bar->total_cols == 20);
    assert(bar->total_rows == 1);

    double val = n00b_progress_get_value(bar);
    assert(fabs(val - 0.0) < 0.001);

    // At 0%, all cells should show the empty character.
    for (n00b_isize_t c = 0; c < 20; c++) {
        assert(cell_occupied(bar, 0, c));
    }

    printf("  [PASS] progress at 0%%\n");
    n00b_plane_destroy(bar);
}

// -------------------------------------------------------------------
// Test 2: Progress at 100%
// -------------------------------------------------------------------

static void
test_progress_full(void)
{
    n00b_plane_t *bar = n00b_progress_new(.cols = 10, .value = 1.0);

    double val = n00b_progress_get_value(bar);
    assert(fabs(val - 1.0) < 0.001);

    // At 100%, all cells should be occupied with fill char.
    for (n00b_isize_t c = 0; c < 10; c++) {
        assert(cell_occupied(bar, 0, c));
    }

    printf("  [PASS] progress at 100%%\n");
    n00b_plane_destroy(bar);
}

// -------------------------------------------------------------------
// Test 3: Set value
// -------------------------------------------------------------------

static void
test_progress_set_value(void)
{
    n00b_plane_t *bar = n00b_progress_new(.cols = 10);

    assert(fabs(n00b_progress_get_value(bar)) < 0.001);

    n00b_progress_set_value(bar, 0.5);
    assert(fabs(n00b_progress_get_value(bar) - 0.5) < 0.001);

    // Clamp above 1.0.
    n00b_progress_set_value(bar, 1.5);
    assert(fabs(n00b_progress_get_value(bar) - 1.0) < 0.001);

    // Clamp below 0.0.
    n00b_progress_set_value(bar, -0.5);
    assert(fabs(n00b_progress_get_value(bar)) < 0.001);

    printf("  [PASS] progress set_value\n");
    n00b_plane_destroy(bar);
}

// -------------------------------------------------------------------
// Test 4: Measure
// -------------------------------------------------------------------

static void
test_progress_measure(void)
{
    n00b_plane_t *bar = n00b_progress_new(.cols = 20);

    n00b_isize_t pref_cols, pref_rows, min_cols, min_rows;
    n00b_widget_measure(bar, &pref_cols, &pref_rows, &min_cols, &min_rows);

    assert(pref_cols == 20);
    assert(pref_rows == 1);
    assert(min_cols >= 3);
    assert(min_rows == 1);

    printf("  [PASS] progress measure\n");
    n00b_plane_destroy(bar);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running progress widget tests...\n");

    test_progress_zero();
    test_progress_full();
    test_progress_set_value();
    test_progress_measure();

    printf("All progress tests passed.\n");

    n00b_shutdown();
    return 0;
}
