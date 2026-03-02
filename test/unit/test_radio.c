/*
 * Unit tests for the radio button widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/widget.h"
#include "display/widgets/radio.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static int  g_last_selected  = -1;
static int  g_change_count   = 0;

static void
test_change_handler(n00b_plane_t *plane, int selected, void *data)
{
    (void)plane;
    (void)data;
    g_last_selected = selected;
    g_change_count++;
}

// -------------------------------------------------------------------
// Test 1: Create group + 3 radios
// -------------------------------------------------------------------

static void
test_radio_create(void)
{
    n00b_radio_group_t *group = n00b_radio_group_new();
    assert(group != nullptr);
    assert(n00b_radio_group_get_selected(group) == -1);

    n00b_plane_t *r1 = n00b_radio_new(n00b_string_from_cstr("Option A"),
                                        .group = group);
    n00b_plane_t *r2 = n00b_radio_new(n00b_string_from_cstr("Option B"),
                                        .group = group);
    n00b_plane_t *r3 = n00b_radio_new(n00b_string_from_cstr("Option C"),
                                        .group = group);

    assert(r1->widget_vtable == &n00b_widget_radio);
    assert(r2->widget_vtable == &n00b_widget_radio);
    assert(r3->widget_vtable == &n00b_widget_radio);
    assert(n00b_widget_can_focus(r1));

    n00b_radio_t *data = (n00b_radio_t *)r1->widget_data;
    assert(data->index == 0);
    data = (n00b_radio_t *)r3->widget_data;
    assert(data->index == 2);

    printf("  [PASS] radio create\n");
    n00b_plane_destroy(r1);
    n00b_plane_destroy(r2);
    n00b_plane_destroy(r3);
}

// -------------------------------------------------------------------
// Test 2: Select propagates mutual exclusion
// -------------------------------------------------------------------

static void
test_radio_mutual_exclusion(void)
{
    n00b_radio_group_t *group = n00b_radio_group_new();
    n00b_plane_t *r1 = n00b_radio_new(n00b_string_from_cstr("A"), .group = group);
    n00b_plane_t *r2 = n00b_radio_new(n00b_string_from_cstr("B"), .group = group);
    n00b_plane_t *r3 = n00b_radio_new(n00b_string_from_cstr("C"), .group = group);

    n00b_radio_group_set_selected(group, 0);
    assert(n00b_radio_is_selected(r1));
    assert(!n00b_radio_is_selected(r2));
    assert(!n00b_radio_is_selected(r3));

    n00b_radio_group_set_selected(group, 2);
    assert(!n00b_radio_is_selected(r1));
    assert(!n00b_radio_is_selected(r2));
    assert(n00b_radio_is_selected(r3));

    printf("  [PASS] radio mutual exclusion\n");
    n00b_plane_destroy(r1);
    n00b_plane_destroy(r2);
    n00b_plane_destroy(r3);
}

// -------------------------------------------------------------------
// Test 3: Space selects
// -------------------------------------------------------------------

static void
test_radio_space_selects(void)
{
    n00b_radio_group_t *group = n00b_radio_group_new();
    n00b_plane_t *r1 = n00b_radio_new(n00b_string_from_cstr("A"), .group = group);
    n00b_plane_t *r2 = n00b_radio_new(n00b_string_from_cstr("B"), .group = group);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(r2, &event);
    assert(consumed);
    assert(n00b_radio_group_get_selected(group) == 1);
    assert(!n00b_radio_is_selected(r1));
    assert(n00b_radio_is_selected(r2));

    printf("  [PASS] radio space selects\n");
    n00b_plane_destroy(r1);
    n00b_plane_destroy(r2);
}

// -------------------------------------------------------------------
// Test 4: Initial selection via set_selected
// -------------------------------------------------------------------

static void
test_radio_initial_selection(void)
{
    n00b_radio_group_t *group = n00b_radio_group_new();
    n00b_plane_t *r1 = n00b_radio_new(n00b_string_from_cstr("A"), .group = group);
    n00b_plane_t *r2 = n00b_radio_new(n00b_string_from_cstr("B"), .group = group);

    // Initially nothing selected.
    assert(n00b_radio_group_get_selected(group) == -1);

    // Set initial selection.
    n00b_radio_group_set_selected(group, 1);
    assert(n00b_radio_group_get_selected(group) == 1);
    assert(n00b_radio_is_selected(r2));

    printf("  [PASS] radio initial selection\n");
    n00b_plane_destroy(r1);
    n00b_plane_destroy(r2);
}

// -------------------------------------------------------------------
// Test 5: Group callback fires
// -------------------------------------------------------------------

static void
test_radio_group_callback(void)
{
    g_change_count  = 0;
    g_last_selected = -1;

    n00b_radio_group_t *group = n00b_radio_group_new();
    n00b_radio_group_on_change(group, test_change_handler, nullptr);

    n00b_plane_t *r1 = n00b_radio_new(n00b_string_from_cstr("A"), .group = group);
    n00b_plane_t *r2 = n00b_radio_new(n00b_string_from_cstr("B"), .group = group);

    n00b_radio_group_set_selected(group, 0);
    assert(g_change_count == 1);
    assert(g_last_selected == 0);

    n00b_radio_group_set_selected(group, 1);
    assert(g_change_count == 2);
    assert(g_last_selected == 1);

    printf("  [PASS] radio group callback\n");
    n00b_plane_destroy(r1);
    n00b_plane_destroy(r2);
}

// -------------------------------------------------------------------
// Test 6: Measure width
// -------------------------------------------------------------------

static void
test_radio_measure(void)
{
    n00b_radio_group_t *group = n00b_radio_group_new();

    // Unicode: focus(1) + indicator(1) + space(1) + label("Hi"=2) = 5
    n00b_plane_t *r = n00b_radio_new(n00b_string_from_cstr("Hi"),
                                       .group = group);

    n00b_isize_t pc, pr, mc, mr;
    n00b_widget_measure(r, &pc, &pr, &mc, &mr);
    assert(pc == 5);  // 1 + 1 + 1 + 2
    assert(pr == 1);

    printf("  [PASS] radio measure\n");
    n00b_plane_destroy(r);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running radio widget tests...\n");

    test_radio_create();
    test_radio_mutual_exclusion();
    test_radio_space_selects();
    test_radio_initial_selection();
    test_radio_group_callback();
    test_radio_measure();

    printf("All radio tests passed.\n");

    n00b_shutdown();
    return 0;
}
