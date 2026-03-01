/*
 * Unit tests for the spacer widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/widget.h"
#include "display/widgets/spacer.h"

// -------------------------------------------------------------------
// Test 1: Default spacer is 1x1
// -------------------------------------------------------------------

static void
test_spacer_default(void)
{
    n00b_plane_t *sp = n00b_spacer_new();

    assert(sp != nullptr);
    assert(sp->widget_vtable == &n00b_widget_spacer);
    assert(sp->total_cols == 1);
    assert(sp->total_rows == 1);

    // Cell should be empty (not occupied).
    n00b_option_t(n00b_const_rcell_ptr_t) opt = n00b_plane_get_cell(sp, 0, 0);
    assert(n00b_option_is_set(opt));
    const n00b_rcell_t *cell = n00b_option_get(opt);
    assert(!(cell->flags & N00B_CELL_OCCUPIED));

    printf("  [PASS] default spacer\n");
    n00b_plane_destroy(sp);
}

// -------------------------------------------------------------------
// Test 2: Custom size spacer
// -------------------------------------------------------------------

static void
test_spacer_custom_size(void)
{
    n00b_plane_t *sp = n00b_spacer_new(.cols = 5, .rows = 3);

    assert(sp != nullptr);
    assert(sp->total_cols == 5);
    assert(sp->total_rows == 3);

    printf("  [PASS] custom size spacer\n");
    n00b_plane_destroy(sp);
}

// -------------------------------------------------------------------
// Test 3: Measure
// -------------------------------------------------------------------

static void
test_spacer_measure(void)
{
    n00b_plane_t *sp = n00b_spacer_new(.cols = 4, .rows = 2);

    n00b_isize_t pref_cols, pref_rows, min_cols, min_rows;
    n00b_widget_measure(sp, &pref_cols, &pref_rows, &min_cols, &min_rows);

    assert(pref_cols == 4);
    assert(pref_rows == 2);
    assert(min_cols == 4);
    assert(min_rows == 2);

    printf("  [PASS] spacer measure\n");
    n00b_plane_destroy(sp);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running spacer widget tests...\n");

    test_spacer_default();
    test_spacer_custom_size();
    test_spacer_measure();

    printf("All spacer tests passed.\n");

    n00b_shutdown();
    return 0;
}
