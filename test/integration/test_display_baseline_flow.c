#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/widgets/label.h"
#include "text/strings/string_ops.h"

extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);
extern void          n00b_stream_backend_set_size(void         *ctx,
                                                   n00b_isize_t  rows,
                                                   n00b_isize_t  cols);

static int g_button_clicks = 0;

static void
on_click_update_status(n00b_plane_t *plane, void *data)
{
    (void)plane;
    g_button_clicks++;

    n00b_plane_t *status_label = (n00b_plane_t *)data;
    n00b_label_set_text(status_label, n00b_string_from_cstr("Status: clicked"));
}

static void
test_display_flow_focus_mouse_render(void)
{
    g_button_clicks = 0;

    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, 10, 48);
    n00b_canvas_resize(canvas, 10, 48);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width        = 48;
    root->height       = 10;

    n00b_plane_t *status = n00b_label_new(
        n00b_string_from_cstr("Status: idle"),
        .canvas = canvas,
        .width  = 24,
        .height = 1);

    n00b_plane_t *button = n00b_button_new(
        n00b_string_from_cstr("Run"),
        .canvas        = canvas,
        .width         = 10,
        .height        = 1,
        .on_click      = on_click_update_status,
        .on_click_data = status);

    n00b_plane_add_child(root, status, 2, 1);
    n00b_plane_add_child(root, button, 2, 3);
    n00b_canvas_add_plane(canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(fm->count == 1);
    assert(n00b_focus_mgr_current(fm) == button);

    n00b_event_t click = {
        .type         = N00B_EVENT_MOUSE,
        .mouse.x      = 2,
        .mouse.y      = 3,
        .mouse.button = N00B_MOUSE_LEFT,
        .mouse.action = N00B_MOUSE_PRESS,
        .mouse.mods   = N00B_MOD_NONE,
    };

    n00b_mouse_route_event(canvas, fm, &click);
    assert(g_button_clicks == 1);
    assert(n00b_focus_mgr_current(fm) == button);
    assert(n00b_focus_mgr_next(fm) == button);

    n00b_canvas_render(canvas);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    assert(buf != nullptr);
    assert(n00b_unicode_str_contains(buf, r"Status: clicked"));
    assert(n00b_unicode_str_contains(buf, r"Run"));

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, status);
    n00b_plane_remove_child(root, button);
    n00b_widget_detach(status);
    n00b_widget_detach(button);
    n00b_plane_destroy(status);
    n00b_plane_destroy(button);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] display flow focus/mouse/render\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display baseline flow integration test...\n");

    test_display_flow_focus_mouse_render();

    printf("Display baseline flow integration test passed.\n");
    n00b_shutdown();
    return 0;
}
