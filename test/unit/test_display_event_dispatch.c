#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "internal/display/event_dispatch.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

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

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display event-dispatch tests...\n");
    test_event_dispatch_contracts();

    printf("Display event-dispatch tests passed.\n");
    n00b_shutdown();
    return 0;
}
