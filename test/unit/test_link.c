/*
 * Unit tests for the link widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/link.h"
#include "display/event.h"
#include "text/unicode/properties.h"

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
// Test 1: Create with text
// -------------------------------------------------------------------

static void
test_link_create(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Click here");
    n00b_plane_t  *lk   = n00b_link_new(text);

    assert(lk != nullptr);
    assert(lk->widget_vtable == &n00b_widget_link);
    assert(n00b_widget_can_focus(lk));

    n00b_link_t *data = (n00b_link_t *)lk->widget_data;
    assert(data->visited == false);
    assert(data->underline == true);

    printf("  [PASS] link create\n");
    n00b_plane_destroy(lk);
}

// -------------------------------------------------------------------
// Test 2: Activate fires callback
// -------------------------------------------------------------------

static void
test_link_activate(void)
{
    g_click_count = 0;

    n00b_string_t *text = n00b_string_from_cstr("Link");
    n00b_plane_t  *lk   = n00b_link_new(text,
                                           .on_click = test_click_handler);

    // Activate via Enter key.
    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(lk, &event);
    assert(consumed);
    assert(g_click_count == 1);

    printf("  [PASS] link activate\n");
    n00b_plane_destroy(lk);
}

// -------------------------------------------------------------------
// Test 3: Visited state changes on activation
// -------------------------------------------------------------------

static void
test_link_visited(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Link");
    n00b_plane_t  *lk   = n00b_link_new(text);

    n00b_link_t *data = (n00b_link_t *)lk->widget_data;
    assert(data->visited == false);

    // Activate via space.
    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(lk, &event);
    assert(data->visited == true);

    // Reset visited.
    n00b_link_reset_visited(lk);
    assert(data->visited == false);

    printf("  [PASS] link visited state\n");
    n00b_plane_destroy(lk);
}

// -------------------------------------------------------------------
// Test 4: Underline on/off
// -------------------------------------------------------------------

static void
test_link_underline(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Link");

    // Default: underline on.
    n00b_plane_t *lk1 = n00b_link_new(text);
    n00b_link_t *d1 = (n00b_link_t *)lk1->widget_data;
    assert(d1->underline == true);

    // Explicitly off.
    n00b_plane_t *lk2 = n00b_link_new(text, .underline = false);
    n00b_link_t *d2 = (n00b_link_t *)lk2->widget_data;
    assert(d2->underline == false);

    printf("  [PASS] link underline\n");
    n00b_plane_destroy(lk1);
    n00b_plane_destroy(lk2);
}

// -------------------------------------------------------------------
// Test 5: Measure width
// -------------------------------------------------------------------

static void
test_link_measure(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Hello");
    n00b_plane_t  *lk   = n00b_link_new(text);

    int32_t pc, pr, mc, mr;
    n00b_widget_measure(lk, &pc, &pr, &mc, &mr);
    assert(pc == 5);  // "Hello" = 5 cols
    assert(pr == 1);

    printf("  [PASS] link measure\n");
    n00b_plane_destroy(lk);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running link widget tests...\n");

    test_link_create();
    test_link_activate();
    test_link_visited();
    test_link_underline();
    test_link_measure();

    printf("All link tests passed.\n");

    n00b_shutdown();
    return 0;
}
