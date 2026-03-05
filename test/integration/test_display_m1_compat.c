#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef struct {
    n00b_event_t      events[8];
    size_t            count;
    size_t            ix;
    int               poll_calls;
    int               render_calls;
    bool              saw_cursor_hide;
    bool              saw_cursor_show;
    n00b_render_size_t size;
} compat_backend_t;

typedef struct {
    int key_events;
} compat_widget_state_t;

static void *
compat_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    compat_backend_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->size.rows = 8;
    ctx->size.cols = 40;
    ctx->size.cell_pixel_w = 1;
    ctx->size.cell_pixel_h = 1;

    ctx->events[0] = (n00b_event_t){.type = N00B_EVENT_NONE};
    ctx->events[1] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_NONE},
    };
    ctx->events[2] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'x', .mods = N00B_MOD_NONE},
    };
    ctx->events[3] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'c', .mods = N00B_MOD_CTRL},
    };
    ctx->count = 4;

    return ctx;
}

static void
compat_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
compat_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY;
}

static n00b_render_size_t
compat_get_size(void *vctx)
{
    compat_backend_t *ctx = vctx;
    return ctx->size;
}

static void
compat_render_frame(void         *vctx,
                    n00b_rcell_t *cells,
                    n00b_isize_t  rows,
                    n00b_isize_t  cols,
                    n00b_rcell_t *prev_cells)
{
    (void)vctx;
    (void)cells;
    (void)rows;
    (void)cols;
    (void)prev_cells;
}

static void
compat_flush(void *vctx)
{
    (void)vctx;
}

static void
compat_render_planes(void                         *vctx,
                     const n00b_composite_entry_t *entries,
                     n00b_isize_t                  count,
                     n00b_isize_t                  total_rows,
                     n00b_isize_t                  total_cols,
                     n00b_text_style_t            *default_style,
                     n00b_render_cap_t             caps)
{
    compat_backend_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_calls++;
}

static void
compat_cursor_set_visible(void *vctx, bool visible)
{
    compat_backend_t *ctx = vctx;
    if (visible) {
        ctx->saw_cursor_show = true;
    }
    else {
        ctx->saw_cursor_hide = true;
    }
}

static bool
compat_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    compat_backend_t *ctx = vctx;
    (void)timeout_ms;

    ctx->poll_calls++;
    if (ctx->ix < ctx->count) {
        *out = ctx->events[ctx->ix++];
        return out->type != N00B_EVENT_NONE;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static void
compat_widget_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'W');
}

static bool
compat_widget_handle(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    compat_widget_state_t *state = data;
    if (event->type == N00B_EVENT_KEY && event->key.key == 'x') {
        state->key_events++;
        return true;
    }
    return false;
}

static bool
compat_widget_focusable(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t compat_widget = {
    .kind         = "m1_compat",
    .render       = compat_widget_render,
    .handle_event = compat_widget_handle,
    .can_focus    = compat_widget_focusable,
};

static const n00b_renderer_vtable_t compat_renderer = {
    .name               = "m1_compat_test",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = compat_init,
    .destroy            = compat_destroy,
    .capabilities       = compat_caps,
    .get_size           = compat_get_size,
    .render_frame       = compat_render_frame,
    .flush              = compat_flush,
    .render_planes      = compat_render_planes,
    .cursor_set_visible = compat_cursor_set_visible,
    .poll_event         = compat_poll_event,
};

static void
test_m1_compat_loop_path(void)
{
    compat_widget_state_t state = {};

    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &compat_renderer);
    n00b_canvas_resize(canvas, 8, 40);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width  = 40;
    root->height = 8;

    n00b_plane_t *widget_plane = n00b_new_kargs(n00b_plane_t, plane);
    widget_plane->width = 8;
    widget_plane->height = 1;
    n00b_widget_attach(widget_plane, &compat_widget, &state);

    n00b_plane_add_child(root, widget_plane, 2, 2);
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas, .tick_ms = 100);

    compat_backend_t *ctx = canvas->backend_ctx;
    assert(ctx->poll_calls >= 4);
    assert(ctx->render_calls >= 1);
    assert(ctx->saw_cursor_hide);
    assert(ctx->saw_cursor_show);
    assert(state.key_events == 1);

    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, widget_plane);
    n00b_widget_detach(widget_plane);
    n00b_plane_destroy(widget_plane);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] m1 compatibility loop path\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display M1 compatibility integration test...\n");
    test_m1_compat_loop_path();

    printf("Display M1 compatibility integration test passed.\n");
    n00b_shutdown();
    return 0;
}
