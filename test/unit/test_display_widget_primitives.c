#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "internal/display/widget_primitives.h"

static bool
test_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t test_widget_kind = {
    .kind      = "test_widget_kind",
    .can_focus = test_can_focus,
};

static const n00b_widget_vtable_t other_widget_kind = {
    .kind = "other_widget_kind",
};

static void
test_metric_helpers(void)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    assert(plane != nullptr);

    assert(n00b_widget_cell_px_width(plane) >= 1);
    assert(n00b_widget_line_px_height(plane) >= 1);

    n00b_plane_destroy(plane);
    printf("  [PASS] widget metric primitives\n");
}

static void
test_state_helper(void)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    assert(plane != nullptr);

    n00b_plane_set_state(plane, N00B_WSTATE_NORMAL);
    assert(!n00b_widget_state_is_focused_or_active(plane));

    n00b_plane_set_state(plane, N00B_WSTATE_FOCUSED);
    assert(n00b_widget_state_is_focused_or_active(plane));

    n00b_plane_set_state(plane, N00B_WSTATE_ACTIVE);
    assert(n00b_widget_state_is_focused_or_active(plane));

    n00b_plane_destroy(plane);
    printf("  [PASS] widget state primitive\n");
}

static void
test_event_helpers(void)
{
    n00b_event_t left_press = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = 0,
            .y = 0,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };

    n00b_event_t left_release = left_press;
    left_release.mouse.action = N00B_MOUSE_RELEASE;

    n00b_event_t enter_key = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE},
    };

    n00b_event_t space_key = {
        .type = N00B_EVENT_KEY,
        .key = {.key = ' ', .mods = N00B_MOD_NONE},
    };

    n00b_event_t other_key = {
        .type = N00B_EVENT_KEY,
        .key = {.key = 'z', .mods = N00B_MOD_NONE},
    };

    assert(n00b_widget_event_is_left_press(&left_press));
    assert(!n00b_widget_event_is_left_press(&left_release));
    assert(!n00b_widget_event_is_left_press(&enter_key));

    assert(n00b_widget_event_is_keyboard_activate(&enter_key));
    assert(n00b_widget_event_is_keyboard_activate(&space_key));
    assert(!n00b_widget_event_is_keyboard_activate(&other_key));

    assert(n00b_widget_event_is_primary_activate(&left_press));
    assert(n00b_widget_event_is_primary_activate(&enter_key));
    assert(!n00b_widget_event_is_primary_activate(&other_key));

    printf("  [PASS] widget event primitives\n");
}

static void
test_data_guard_helper(void)
{
    int payload = 7;

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    assert(plane != nullptr);

    n00b_widget_attach(plane, &test_widget_kind, &payload);

    assert(n00b_widget_data_if_kind(plane, &test_widget_kind) == &payload);
    assert(n00b_widget_data_if_kind(plane, &other_widget_kind) == nullptr);
    assert(n00b_widget_data_if_kind(nullptr, &test_widget_kind) == nullptr);

    n00b_widget_detach(plane);
    n00b_plane_destroy(plane);

    printf("  [PASS] widget data guard primitive\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display widget primitive tests...\n");

    test_metric_helpers();
    test_state_helper();
    test_event_helpers();
    test_data_guard_helper();

    printf("Display widget primitive tests passed.\n");
    n00b_shutdown();
    return 0;
}
