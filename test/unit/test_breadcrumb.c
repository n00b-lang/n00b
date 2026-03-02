/*
 * Unit tests for the breadcrumb widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/breadcrumb.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static n00b_isize_t g_last_clicked = -1;
static int          g_click_count  = 0;

static void
test_click_handler(n00b_plane_t *plane, n00b_isize_t index, void *data)
{
    (void)plane;
    (void)data;
    g_last_clicked = index;
    g_click_count++;
}

// -------------------------------------------------------------------
// Test 1: Create + push segments
// -------------------------------------------------------------------

static void
test_bc_create_push(void)
{
    n00b_plane_t *bc = n00b_breadcrumb_new();
    assert(bc != nullptr);
    assert(bc->widget_vtable == &n00b_widget_breadcrumb);
    assert(n00b_breadcrumb_count(bc) == 0);

    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Home"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Products"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Current"), nullptr);
    assert(n00b_breadcrumb_count(bc) == 3);

    printf("  [PASS] breadcrumb create + push\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Test 2: Pop removes last
// -------------------------------------------------------------------

static void
test_bc_pop(void)
{
    n00b_plane_t *bc = n00b_breadcrumb_new();
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("A"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("B"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("C"), nullptr);
    assert(n00b_breadcrumb_count(bc) == 3);

    n00b_breadcrumb_pop(bc);
    assert(n00b_breadcrumb_count(bc) == 2);

    n00b_breadcrumb_pop(bc);
    assert(n00b_breadcrumb_count(bc) == 1);

    printf("  [PASS] breadcrumb pop\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Test 3: Click callback fires
// -------------------------------------------------------------------

static void
test_bc_click_callback(void)
{
    g_click_count  = 0;
    g_last_clicked = -1;

    n00b_plane_t *bc = n00b_breadcrumb_new(.on_click = test_click_handler);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Home"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Products"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Current"), nullptr);

    // Activate via Enter (focused_index starts at 0 = "Home").
    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };

    n00b_widget_handle_event(bc, &enter);
    assert(g_click_count == 1);
    assert(g_last_clicked == 0);

    printf("  [PASS] breadcrumb click callback\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Test 4: Last segment not clickable
// -------------------------------------------------------------------

static void
test_bc_last_not_clickable(void)
{
    g_click_count = 0;

    n00b_plane_t *bc = n00b_breadcrumb_new(.on_click = test_click_handler);
    // Only one segment — it IS the last, so clicking does nothing.
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Home"), nullptr);

    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };

    n00b_widget_handle_event(bc, &enter);
    assert(g_click_count == 0); // No callback — only segment is current.

    printf("  [PASS] breadcrumb last not clickable\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Test 5: Keyboard navigation
// -------------------------------------------------------------------

static void
test_bc_keyboard_nav(void)
{
    g_click_count  = 0;
    g_last_clicked = -1;

    n00b_plane_t *bc = n00b_breadcrumb_new(.on_click = test_click_handler);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Home"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Products"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Details"), nullptr);
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Current"), nullptr);

    n00b_breadcrumb_t *data = (n00b_breadcrumb_t *)bc->widget_data;
    assert(data->focused_index == 0);

    // Right moves focus.
    n00b_event_t right = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_RIGHT, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(bc, &right);
    assert(data->focused_index == 1);

    n00b_widget_handle_event(bc, &right);
    assert(data->focused_index == 2); // max clickable = count-2 = 2

    // Won't go past max clickable.
    n00b_widget_handle_event(bc, &right);
    assert(data->focused_index == 2);

    // Left moves back.
    n00b_event_t left = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_LEFT, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(bc, &left);
    assert(data->focused_index == 1);

    // Activate at index 1.
    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(bc, &enter);
    assert(g_last_clicked == 1);

    printf("  [PASS] breadcrumb keyboard nav\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Test 6: Measure width
// -------------------------------------------------------------------

static void
test_bc_measure(void)
{
    n00b_plane_t *bc = n00b_breadcrumb_new();
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Home"), nullptr);       // 4
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Products"), nullptr);   // 8
    n00b_breadcrumb_push(bc, n00b_string_from_cstr("Current"), nullptr);    // 7

    // Total: 4 + 3(" > ") + 8 + 3(" > ") + 7 = 25
    n00b_isize_t pc, pr, mc, mr;
    n00b_widget_measure(bc, &pc, &pr, &mc, &mr);
    assert(pc == 25);
    assert(pr == 1);

    printf("  [PASS] breadcrumb measure\n");
    n00b_plane_destroy(bc);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running breadcrumb widget tests...\n");

    test_bc_create_push();
    test_bc_pop();
    test_bc_click_callback();
    test_bc_last_not_clickable();
    test_bc_keyboard_nav();
    test_bc_measure();

    printf("All breadcrumb tests passed.\n");

    n00b_shutdown();
    return 0;
}
