/*
 * Unit tests for the switch widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/switch.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static bool g_last_on    = false;
static int  g_change_count = 0;

static void
test_change_handler(n00b_plane_t *plane, bool on, void *data)
{
    (void)plane;
    (void)data;
    g_last_on = on;
    g_change_count++;
}

// -------------------------------------------------------------------
// Test 1: Switch creation
// -------------------------------------------------------------------

static void
test_switch_create(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Dark mode");
    n00b_plane_t  *sw    = n00b_switch_new(label);

    assert(sw != nullptr);
    assert(sw->widget_vtable == &n00b_widget_switch);
    assert(n00b_widget_can_focus(sw));
    assert(!n00b_switch_is_on(sw));

    printf("  [PASS] switch create\n");
    n00b_plane_destroy(sw);
}

// -------------------------------------------------------------------
// Test 2: Space toggles switch
// -------------------------------------------------------------------

static void
test_switch_toggle_space(void)
{
    g_change_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Toggle");
    n00b_plane_t  *sw    = n00b_switch_new(label,
                                             .on_change = test_change_handler);

    assert(!n00b_switch_is_on(sw));

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    // Toggle on.
    bool consumed = n00b_widget_handle_event(sw, &event);
    assert(consumed);
    assert(n00b_switch_is_on(sw));
    assert(g_last_on == true);
    assert(g_change_count == 1);

    // Toggle off.
    consumed = n00b_widget_handle_event(sw, &event);
    assert(consumed);
    assert(!n00b_switch_is_on(sw));
    assert(g_last_on == false);
    assert(g_change_count == 2);

    printf("  [PASS] switch toggle via space\n");
    n00b_plane_destroy(sw);
}

// -------------------------------------------------------------------
// Test 3: Mouse click toggles
// -------------------------------------------------------------------

static void
test_switch_toggle_mouse(void)
{
    g_change_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Click");
    n00b_plane_t  *sw    = n00b_switch_new(label,
                                             .on_change = test_change_handler);

    n00b_event_t event = {
        .type  = N00B_EVENT_MOUSE,
        .mouse = { .x = 3, .y = 0,
                   .button = N00B_MOUSE_LEFT,
                   .action = N00B_MOUSE_PRESS,
                   .mods   = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(sw, &event);
    assert(consumed);
    assert(n00b_switch_is_on(sw));
    assert(g_change_count == 1);

    printf("  [PASS] switch toggle via mouse\n");
    n00b_plane_destroy(sw);
}

// -------------------------------------------------------------------
// Test 4: Programmatic set_on
// -------------------------------------------------------------------

static void
test_switch_set_on(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Option");
    n00b_plane_t  *sw    = n00b_switch_new(label);

    assert(!n00b_switch_is_on(sw));

    n00b_switch_set_on(sw, true);
    assert(n00b_switch_is_on(sw));

    n00b_switch_set_on(sw, false);
    assert(!n00b_switch_is_on(sw));

    printf("  [PASS] switch set_on\n");
    n00b_plane_destroy(sw);
}

// -------------------------------------------------------------------
// Test 5: Measure width
// -------------------------------------------------------------------

static void
test_switch_measure(void)
{
    // focus(1) + track(5) + space(1) + label("Hi"=2) = 9
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *sw    = n00b_switch_new(label);

    n00b_isize_t pc, pr, mc, mr;
    n00b_widget_measure(sw, &pc, &pr, &mc, &mr);
    assert(pc == 9);  // 1 + 5 + 1 + 2
    assert(pr == 1);
    assert(mc == 6);  // 1 + 5

    printf("  [PASS] switch measure\n");
    n00b_plane_destroy(sw);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running switch widget tests...\n");

    test_switch_create();
    test_switch_toggle_space();
    test_switch_toggle_mouse();
    test_switch_set_on();
    test_switch_measure();

    printf("All switch tests passed.\n");

    n00b_shutdown();
    return 0;
}
