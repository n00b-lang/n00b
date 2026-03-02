/*
 * Unit tests for the selection list widget.
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
#include "display/widgets/checkbox.h"
#include "display/widgets/selectionlist.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static int g_change_count = 0;

static void
test_change_handler(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    g_change_count++;
}

static void
make_labels(n00b_string_t **out, int count)
{
    const char *names[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon"};
    for (int i = 0; i < count && i < 5; i++) {
        out[i] = n00b_string_from_cstr(names[i]);
    }
}

// -------------------------------------------------------------------
// Test 1: Create
// -------------------------------------------------------------------

static void
test_sellist_create(void)
{
    n00b_string_t *labels[3];
    make_labels(labels, 3);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 3);

    assert(sl != nullptr);
    assert(sl->widget_vtable == &n00b_widget_selectionlist);
    assert(n00b_widget_can_focus(sl));
    assert(n00b_selectionlist_selected_count(sl) == 0);

    printf("  [PASS] selectionlist create\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 2: Toggle via space
// -------------------------------------------------------------------

static void
test_sellist_toggle_space(void)
{
    n00b_string_t *labels[3];
    make_labels(labels, 3);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 3);

    n00b_event_t space = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    // Toggle first item (cursor starts at 0).
    n00b_widget_handle_event(sl, &space);
    assert(n00b_selectionlist_is_selected(sl, 0));
    assert(n00b_selectionlist_selected_count(sl) == 1);

    // Toggle off.
    n00b_widget_handle_event(sl, &space);
    assert(!n00b_selectionlist_is_selected(sl, 0));
    assert(n00b_selectionlist_selected_count(sl) == 0);

    printf("  [PASS] selectionlist toggle space\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 3: Ctrl+A / Ctrl+D
// -------------------------------------------------------------------

static void
test_sellist_select_all_none(void)
{
    n00b_string_t *labels[3];
    make_labels(labels, 3);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 3);

    // Ctrl+A: select all.
    n00b_event_t ctrl_a = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = 'a', .mods = N00B_MOD_CTRL },
    };
    n00b_widget_handle_event(sl, &ctrl_a);
    assert(n00b_selectionlist_selected_count(sl) == 3);

    // Ctrl+D: deselect all.
    n00b_event_t ctrl_d = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = 'd', .mods = N00B_MOD_CTRL },
    };
    n00b_widget_handle_event(sl, &ctrl_d);
    assert(n00b_selectionlist_selected_count(sl) == 0);

    printf("  [PASS] selectionlist Ctrl+A/D\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 4: Cursor navigation
// -------------------------------------------------------------------

static void
test_sellist_cursor(void)
{
    n00b_string_t *labels[3];
    make_labels(labels, 3);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 3);
    n00b_selectionlist_t *data = (n00b_selectionlist_t *)sl->widget_data;
    assert(data->cursor == 0);

    n00b_event_t down = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_DOWN, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(sl, &down);
    assert(data->cursor == 1);

    n00b_event_t up = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_UP, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(sl, &up);
    assert(data->cursor == 0);

    // Wrap.
    n00b_widget_handle_event(sl, &up);
    assert(data->cursor == 2);

    printf("  [PASS] selectionlist cursor nav\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 5: Mouse toggle
// -------------------------------------------------------------------

static void
test_sellist_mouse(void)
{
    n00b_string_t *labels[3];
    make_labels(labels, 3);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 3);

    n00b_event_t click = {
        .type  = N00B_EVENT_MOUSE,
        .mouse = { .x = 5, .y = 1,
                   .button = N00B_MOUSE_LEFT,
                   .action = N00B_MOUSE_PRESS,
                   .mods   = N00B_MOD_NONE },
    };

    n00b_widget_handle_event(sl, &click);
    assert(n00b_selectionlist_is_selected(sl, 1));

    // Click again to untoggle.
    n00b_widget_handle_event(sl, &click);
    assert(!n00b_selectionlist_is_selected(sl, 1));

    printf("  [PASS] selectionlist mouse toggle\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 6: Selected count
// -------------------------------------------------------------------

static void
test_sellist_count(void)
{
    n00b_string_t *labels[4];
    make_labels(labels, 4);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 4);

    n00b_selectionlist_toggle(sl, 0);
    n00b_selectionlist_toggle(sl, 2);
    assert(n00b_selectionlist_selected_count(sl) == 2);

    n00b_selectionlist_select_all(sl);
    assert(n00b_selectionlist_selected_count(sl) == 4);

    n00b_selectionlist_select_none(sl);
    assert(n00b_selectionlist_selected_count(sl) == 0);

    printf("  [PASS] selectionlist selected count\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Test 7: Add/clear
// -------------------------------------------------------------------

static void
test_sellist_add_clear(void)
{
    n00b_string_t *labels[2];
    make_labels(labels, 2);

    n00b_plane_t *sl = n00b_selectionlist_new(labels, 2);
    n00b_selectionlist_t *data = (n00b_selectionlist_t *)sl->widget_data;
    assert(data->count == 2);

    n00b_selectionlist_add_item(sl, n00b_string_from_cstr("New"), nullptr);
    assert(data->count == 3);

    n00b_selectionlist_clear(sl);
    assert(data->count == 0);

    printf("  [PASS] selectionlist add/clear\n");
    n00b_plane_destroy(sl);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running selection list widget tests...\n");

    test_sellist_create();
    test_sellist_toggle_space();
    test_sellist_select_all_none();
    test_sellist_cursor();
    test_sellist_mouse();
    test_sellist_count();
    test_sellist_add_clear();

    printf("All selection list tests passed.\n");

    n00b_shutdown();
    return 0;
}
