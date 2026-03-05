#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/backend_stream_internal.h"
#include "display/render/backend.h"
#include "display/render/box.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/box.h"
#include "display/widgets/button.h"
#include "internal/display/event_dispatch.h"
#include "internal/display/scene_contracts.h"

typedef struct {
    int  key_events;
    bool consume;
} widget_state_t;

static void
dispatch_render(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static bool
dispatch_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    widget_state_t *state = data;
    if (event->type == N00B_EVENT_KEY) {
        state->key_events++;
    }
    return state->consume;
}

static bool
dispatch_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t dispatch_widget = {
    .kind         = "dispatch_dummy",
    .render       = dispatch_render,
    .handle_event = dispatch_handle_event,
    .can_focus    = dispatch_can_focus,
};

static void
test_event_dispatch_contracts(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 6, 40);
    n00b_canvas_resize(canvas, 6, 40);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width  = 40;
    root->height = 6;

    widget_state_t s1 = {.consume = true};
    widget_state_t s2 = {.consume = true};

    n00b_plane_t *p1 = n00b_new_kargs(n00b_plane_t, plane);
    p1->width        = 8;
    p1->height       = 1;
    n00b_widget_attach(p1, &dispatch_widget, &s1);

    n00b_plane_t *p2 = n00b_new_kargs(n00b_plane_t, plane);
    p2->width        = 8;
    p2->height       = 1;
    n00b_widget_attach(p2, &dispatch_widget, &s2);

    n00b_plane_add_child(root, p1, 1, 1);
    n00b_plane_add_child(root, p2, 12, 1);
    n00b_canvas_add_plane(canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(n00b_focus_mgr_current(fm) == p1);

    n00b_event_t tab = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_NONE},
    };
    n00b_display_dispatch_result_t r = n00b_display_dispatch_event(canvas, fm, &tab);
    assert(r.handled);
    assert(r.focus_changed);
    assert(!r.should_stop);
    assert(n00b_focus_mgr_current(fm) == p2);

    n00b_event_t shift_tab = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    r = n00b_display_dispatch_event(canvas, fm, &shift_tab);
    assert(r.handled);
    assert(r.focus_changed);
    assert(n00b_focus_mgr_current(fm) == p1);

    n00b_event_t key_x = {
        .type = N00B_EVENT_KEY,
        .key = {.key = 'x', .mods = N00B_MOD_NONE},
    };
    r = n00b_display_dispatch_event(canvas, fm, &key_x);
    assert(r.handled);
    assert(s1.key_events == 1);

    assert(n00b_focus_mgr_set(fm, p1));
    n00b_event_t click = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = 12,
            .y = 1,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };
    r = n00b_display_dispatch_event(canvas, fm, &click);
    assert(r.handled);
    assert(r.focus_changed);
    assert(n00b_focus_mgr_current(fm) == p2);

    n00b_event_t ctrl_c = {
        .type = N00B_EVENT_KEY,
        .key = {.key = 'c', .mods = N00B_MOD_CTRL},
    };
    r = n00b_display_dispatch_event(canvas, fm, &ctrl_c);
    assert(r.handled);
    assert(r.should_stop);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, p1);
    n00b_plane_remove_child(root, p2);
    n00b_widget_detach(p1);
    n00b_widget_detach(p2);
    n00b_plane_destroy(p1);
    n00b_plane_destroy(p2);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] event dispatch key/mouse/stop\n");
}

static void
test_button_border_click_focuses(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 20, 80);
    n00b_canvas_resize(canvas, 20, 80);

    n00b_plane_t *root = n00b_box_new(.canvas = canvas,
                                        .direction = N00B_FLEX_COLUMN,
                                        .gap = 1);
    root->width  = 80;
    root->height = 20;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *btn1 = n00b_button_new(n00b_string_from_cstr("One"),
                                           .canvas = canvas);
    n00b_plane_t *btn2 = n00b_button_new(n00b_string_from_cstr("Two"),
                                           .canvas = canvas);
    n00b_plane_add_child(root, btn1, 0, 0);
    n00b_plane_add_child(root, btn2, 0, 0);

    n00b_display_scene_run_layout(canvas);
    n00b_display_scene_mark_all_dirty(canvas);
    n00b_display_scene_rerender_dirty(canvas);

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t it = 0;
    n00b_box_insets_px(btn2->box, cpw, cph, &it, nullptr, nullptr, nullptr);

    // Click exactly on the left border cell (outside content area).
    n00b_event_t click = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = btn2->x,
            .y = btn2->y + (it > 0 ? it : 0),
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(n00b_focus_mgr_set(fm, btn1));
    assert(n00b_focus_mgr_current(fm) == btn1);

    n00b_display_dispatch_result_t r = n00b_display_dispatch_event(canvas, fm, &click);
    assert(r.handled);
    assert(n00b_focus_mgr_current(fm) == btn2);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, btn1);
    n00b_plane_remove_child(root, btn2);
    n00b_widget_detach(btn1);
    n00b_widget_detach(btn2);
    n00b_plane_destroy(btn1);
    n00b_plane_destroy(btn2);
    n00b_widget_detach(root);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] button border click focuses button\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display event-dispatch tests...\n");
    test_event_dispatch_contracts();
    test_button_border_click_focuses();

    printf("Display event-dispatch tests passed.\n");
    n00b_shutdown();
    return 0;
}
