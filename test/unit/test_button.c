/*
 * Unit tests for the button widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/draw_cmd.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/event.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static int g_click_count = 0;

static void
test_click_handler(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    g_click_count++;
}

// -------------------------------------------------------------------
// Test 1: Button creation and vtable
// -------------------------------------------------------------------

static void
test_button_create(void)
{
    n00b_string_t *label = n00b_string_from_cstr("OK");
    n00b_plane_t  *btn   = n00b_button_new(label);

    assert(btn != nullptr);
    assert(btn->widget_vtable == &n00b_widget_button);
    assert(n00b_widget_can_focus(btn));

    printf("  [PASS] button create\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Test 2: Enter key activates button
// -------------------------------------------------------------------

static void
test_button_enter(void)
{
    g_click_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Click");
    n00b_plane_t  *btn   = n00b_button_new(label,
                                             .on_click = test_click_handler);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(btn, &event);
    assert(consumed);
    assert(g_click_count == 1);

    printf("  [PASS] button enter\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Test 3: Space key activates button
// -------------------------------------------------------------------

static void
test_button_space(void)
{
    g_click_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Go");
    n00b_plane_t  *btn   = n00b_button_new(label,
                                             .on_click = test_click_handler);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(btn, &event);
    assert(consumed);
    assert(g_click_count == 1);

    printf("  [PASS] button space\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Test 4: Other keys are not consumed
// -------------------------------------------------------------------

static void
test_button_other_key(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Btn");
    n00b_plane_t  *btn   = n00b_button_new(label);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = 'a', .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(btn, &event);
    assert(!consumed);

    printf("  [PASS] button other key\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Test 5: Measure
// -------------------------------------------------------------------

static void
test_button_measure(void)
{
    n00b_string_t *label = n00b_string_from_cstr("OK");
    n00b_plane_t  *btn   = n00b_button_new(label);

    int32_t pref_w, pref_h, min_w, min_h;
    n00b_widget_measure(btn, &pref_w, &pref_h, &min_w, &min_h);

    // Measure reports content area in pixels.
    assert(pref_w > 2);
    assert(pref_h == 1);

    printf("  [PASS] button measure\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Test 6: Button render produces draw commands
// -------------------------------------------------------------------

static void
test_button_render(void)
{
    n00b_string_t *label = n00b_string_from_cstr("OK");
    n00b_plane_t  *btn   = n00b_button_new(label);

    n00b_widget_render(btn);

    // Rendering should produce at least one draw command.
    assert(btn->draw_list.count > 0);

    printf("  [PASS] button render\n");
    n00b_plane_destroy(btn);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running button widget tests...\n");

    test_button_create();
    test_button_enter();
    test_button_space();
    test_button_other_key();
    test_button_measure();
    test_button_render();

    printf("All button tests passed.\n");

    n00b_shutdown();
    return 0;
}
