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
#include "display/render/draw_cmd.h"
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

    // Spacer render should produce an empty draw list (no content).
    n00b_widget_render(sp);
    assert(sp->draw_list.count == 0);

    printf("  [PASS] default spacer\n");
    n00b_plane_destroy(sp);
}

// -------------------------------------------------------------------
// Test 2: Custom size spacer
// -------------------------------------------------------------------

static void
test_spacer_custom_size(void)
{
    n00b_plane_t *sp = n00b_spacer_new(.width = 5, .height = 3);

    assert(sp != nullptr);

    // Verify size via measure.
    int32_t pref_w, pref_h, min_w, min_h;
    n00b_widget_measure(sp, &pref_w, &pref_h, &min_w, &min_h);

    assert(pref_w == 5);
    assert(pref_h == 3);

    printf("  [PASS] custom size spacer\n");
    n00b_plane_destroy(sp);
}

// -------------------------------------------------------------------
// Test 3: Measure
// -------------------------------------------------------------------

static void
test_spacer_measure(void)
{
    n00b_plane_t *sp = n00b_spacer_new(.width = 4, .height = 2);

    int32_t pref_w, pref_h, min_w, min_h;
    n00b_widget_measure(sp, &pref_w, &pref_h, &min_w, &min_h);

    assert(pref_w == 4);
    assert(pref_h == 2);
    assert(min_w == 4);
    assert(min_h == 2);

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
