/*
 * Unit tests for the list widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/list_widget.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static int g_last_selected = -1;
static int g_select_count  = 0;

static void
test_select_handler(n00b_plane_t *plane, int index, void *data)
{
    (void)plane;
    (void)data;
    g_last_selected = index;
    g_select_count++;
}

static n00b_string_t *
make_items(n00b_string_t **out, int count)
{
    const char *labels[] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon",
                            "Zeta", "Eta", "Theta", "Iota", "Kappa"};
    for (int i = 0; i < count && i < 10; i++) {
        out[i] = n00b_string_from_cstr(labels[i]);
    }
    return nullptr;
}

// -------------------------------------------------------------------
// Test 1: Create
// -------------------------------------------------------------------

static void
test_list_create(void)
{
    n00b_string_t *items[3];
    make_items(items, 3);

    n00b_plane_t *lst = n00b_list_widget_new(items, 3);

    assert(lst != nullptr);
    assert(lst->widget_vtable == &n00b_widget_list);
    assert(n00b_widget_can_focus(lst));
    assert(n00b_list_widget_get_selected(lst) == -1);
    assert(n00b_list_widget_count(lst) == 3);

    printf("  [PASS] list create\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 2: Arrow keys select
// -------------------------------------------------------------------

static void
test_list_arrow_keys(void)
{
    n00b_string_t *items[3];
    make_items(items, 3);

    n00b_plane_t *lst = n00b_list_widget_new(items, 3);

    n00b_event_t down = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_DOWN, .mods = N00B_MOD_NONE },
    };

    // First down selects item 0.
    n00b_widget_handle_event(lst, &down);
    assert(n00b_list_widget_get_selected(lst) == 0);

    // Second down selects item 1.
    n00b_widget_handle_event(lst, &down);
    assert(n00b_list_widget_get_selected(lst) == 1);

    // Up goes back to 0.
    n00b_event_t up = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_UP, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(lst, &up);
    assert(n00b_list_widget_get_selected(lst) == 0);

    printf("  [PASS] list arrow keys\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 3: Wrap around
// -------------------------------------------------------------------

static void
test_list_wrap(void)
{
    n00b_string_t *items[3];
    make_items(items, 3);

    n00b_plane_t *lst = n00b_list_widget_new(items, 3, .selected = 2);

    n00b_event_t down = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_DOWN, .mods = N00B_MOD_NONE },
    };

    // From last item, down wraps to 0.
    n00b_widget_handle_event(lst, &down);
    assert(n00b_list_widget_get_selected(lst) == 0);

    // From first item, up wraps to last.
    n00b_event_t up = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_UP, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(lst, &up);
    assert(n00b_list_widget_get_selected(lst) == 2);

    printf("  [PASS] list wrap around\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 4: Page up/down
// -------------------------------------------------------------------

static void
test_list_page(void)
{
    n00b_string_t *items[10];
    make_items(items, 10);

    // 3 visible rows.
    n00b_plane_t *lst = n00b_list_widget_new(items, 10,
                                               .selected = 0,
                                               .height   = 3);

    n00b_event_t pgdn = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_PAGE_DOWN, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(lst, &pgdn);
    assert(n00b_list_widget_get_selected(lst) == 3); // 0 + 3

    n00b_event_t pgup = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_PAGE_UP, .mods = N00B_MOD_NONE },
    };
    n00b_widget_handle_event(lst, &pgup);
    assert(n00b_list_widget_get_selected(lst) == 0); // 3 - 3

    printf("  [PASS] list page up/down\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 5: Mouse click selects
// -------------------------------------------------------------------

static void
test_list_mouse_click(void)
{
    n00b_string_t *items[5];
    make_items(items, 5);

    n00b_plane_t *lst = n00b_list_widget_new(items, 5);

    n00b_event_t click = {
        .type  = N00B_EVENT_MOUSE,
        .mouse = { .x = 5, .y = 2,
                   .button = N00B_MOUSE_LEFT,
                   .action = N00B_MOUSE_PRESS,
                   .mods   = N00B_MOD_NONE },
    };

    n00b_widget_handle_event(lst, &click);
    assert(n00b_list_widget_get_selected(lst) == 2);

    printf("  [PASS] list mouse click\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 6: Add/clear items
// -------------------------------------------------------------------

static void
test_list_add_clear(void)
{
    n00b_string_t *items[2];
    make_items(items, 2);

    n00b_plane_t *lst = n00b_list_widget_new(items, 2);
    assert(n00b_list_widget_count(lst) == 2);

    n00b_list_widget_add_item(lst, n00b_string_from_cstr("New"));
    assert(n00b_list_widget_count(lst) == 3);

    n00b_list_widget_clear(lst);
    assert(n00b_list_widget_count(lst) == 0);
    assert(n00b_list_widget_get_selected(lst) == -1);

    printf("  [PASS] list add/clear\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 7: Callback fires on activate
// -------------------------------------------------------------------

static void
test_list_callback(void)
{
    g_select_count  = 0;
    g_last_selected = -1;

    n00b_string_t *items[3];
    make_items(items, 3);

    n00b_plane_t *lst = n00b_list_widget_new(items, 3,
                                               .selected  = 1,
                                               .on_select = test_select_handler);

    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE },
    };

    n00b_widget_handle_event(lst, &enter);
    assert(g_select_count == 1);
    assert(g_last_selected == 1);

    printf("  [PASS] list callback\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Test 8: Measure
// -------------------------------------------------------------------

static void
test_list_measure(void)
{
    n00b_string_t *items[3];
    make_items(items, 3);

    n00b_plane_t *lst = n00b_list_widget_new(items, 3);

    n00b_isize_t pc, pr, mc, mr;
    n00b_widget_measure(lst, &pc, &pr, &mc, &mr);

    // Longest item: "Gamma" = 5 cols.  marker(2) + 5 + scroll(1) = 8
    assert(pc == 8);
    assert(pr == 3); // 3 items < default 5

    printf("  [PASS] list measure\n");
    n00b_plane_destroy(lst);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running list widget tests...\n");

    test_list_create();
    test_list_arrow_keys();
    test_list_wrap();
    test_list_page();
    test_list_mouse_click();
    test_list_add_clear();
    test_list_callback();
    test_list_measure();

    printf("All list widget tests passed.\n");

    n00b_shutdown();
    return 0;
}
