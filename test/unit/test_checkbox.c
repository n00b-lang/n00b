/*
 * Unit tests for the checkbox widget.
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
#include "display/widget.h"
#include "display/widgets/checkbox.h"
#include "display/event.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static bool g_last_checked = false;
static int  g_change_count = 0;

static void
test_change_handler(n00b_plane_t *plane, bool checked, void *data)
{
    (void)plane;
    (void)data;
    g_last_checked = checked;
    g_change_count++;
}

// -------------------------------------------------------------------
// Test 1: Checkbox creation
// -------------------------------------------------------------------

static void
test_checkbox_create(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Enable");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    assert(cb != nullptr);
    assert(cb->widget_vtable == &n00b_widget_checkbox);
    assert(n00b_widget_can_focus(cb));
    assert(!n00b_checkbox_is_checked(cb));

    printf("  [PASS] checkbox create\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 2: Space toggles checkbox
// -------------------------------------------------------------------

static void
test_checkbox_toggle(void)
{
    g_change_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Toggle");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .on_change = test_change_handler);

    assert(!n00b_checkbox_is_checked(cb));

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    // Toggle on.
    bool consumed = n00b_widget_handle_event(cb, &event);
    assert(consumed);
    assert(n00b_checkbox_is_checked(cb));
    assert(g_last_checked == true);
    assert(g_change_count == 1);

    // Toggle off.
    consumed = n00b_widget_handle_event(cb, &event);
    assert(consumed);
    assert(!n00b_checkbox_is_checked(cb));
    assert(g_last_checked == false);
    assert(g_change_count == 2);

    printf("  [PASS] checkbox toggle\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 3: Programmatic set_checked
// -------------------------------------------------------------------

static void
test_checkbox_set_checked(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Option");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    assert(!n00b_checkbox_is_checked(cb));

    n00b_checkbox_set_checked(cb, true);
    assert(n00b_checkbox_is_checked(cb));

    n00b_checkbox_set_checked(cb, false);
    assert(!n00b_checkbox_is_checked(cb));

    printf("  [PASS] checkbox set_checked\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 4: Other keys not consumed
// -------------------------------------------------------------------

static void
test_checkbox_other_key(void)
{
    n00b_string_t *label = n00b_string_from_cstr("X");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = 'a', .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(cb, &event);
    assert(!consumed);

    printf("  [PASS] checkbox other key\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running checkbox widget tests...\n");

    test_checkbox_create();
    test_checkbox_toggle();
    test_checkbox_set_checked();
    test_checkbox_other_key();

    printf("All checkbox tests passed.\n");

    n00b_shutdown();
    return 0;
}
