#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef struct {
    n00b_event_t       events[16];
    size_t             count;
    size_t             ix;
    int                poll_calls;
    int                render_calls;
    bool               saw_cursor_hide;
    bool               saw_cursor_show;
    n00b_render_size_t size;
} flow_backend_t;

typedef struct {
    int key_events;
    int activations;
    int mouse_presses;
} flow_widget_state_t;

typedef struct {
    int        calls;
    n00b_isize_t rows;
    n00b_isize_t cols;
} resize_observer_t;

static void *
flow_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    flow_backend_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->size.rows = 8;
    ctx->size.cols = 40;
    ctx->size.cell_pixel_w = 1;
    ctx->size.cell_pixel_h = 1;

    ctx->events[0] = (n00b_event_t){
        .type = N00B_EVENT_RESIZE,
        .resize = {.rows = 8, .cols = 40},
    };
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
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    ctx->events[4] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'y', .mods = N00B_MOD_NONE},
    };
    ctx->events[5] = (n00b_event_t){
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = 14,
            .y      = 2,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods   = N00B_MOD_NONE,
        },
    };
    ctx->events[6] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = ' ', .mods = N00B_MOD_NONE},
    };
    ctx->events[7] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = 'c', .mods = N00B_MOD_CTRL},
    };
    ctx->count = 8;

    return ctx;
}

static void
flow_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
flow_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY;
}

static n00b_render_size_t
flow_get_size(void *vctx)
{
    flow_backend_t *ctx = vctx;
    return ctx->size;
}

static void
flow_render_frame(void         *vctx,
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
flow_flush(void *vctx)
{
    (void)vctx;
}

static void
flow_render_planes(void                         *vctx,
                   const n00b_composite_entry_t *entries,
                   n00b_isize_t                  count,
                   n00b_isize_t                  total_rows,
                   n00b_isize_t                  total_cols,
                   n00b_text_style_t            *default_style,
                   n00b_render_cap_t             caps)
{
    flow_backend_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_calls++;
}

static void
flow_cursor_visible(void *vctx, bool visible)
{
    flow_backend_t *ctx = vctx;
    if (visible) {
        ctx->saw_cursor_show = true;
    }
    else {
        ctx->saw_cursor_hide = true;
    }
}

static bool
flow_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    flow_backend_t *ctx = vctx;
    (void)timeout_ms;
    ctx->poll_calls++;

    if (ctx->ix < ctx->count) {
        *out = ctx->events[ctx->ix++];
        return true;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static void
flow_widget_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'W');
}

static bool
flow_widget_handle(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    flow_widget_state_t *state = data;

    if (event->type == N00B_EVENT_MOUSE
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS) {
        state->mouse_presses++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY && event->key.key == ' ') {
        state->activations++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && event->key.key >= 'a'
        && event->key.key <= 'z') {
        state->key_events++;
        return true;
    }

    return false;
}

static bool
flow_widget_focusable(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t flow_widget = {
    .kind         = "m2_terminal_flow",
    .render       = flow_widget_render,
    .handle_event = flow_widget_handle,
    .can_focus    = flow_widget_focusable,
};

static const n00b_renderer_vtable_t flow_renderer = {
    .name               = "m2_terminal_backend",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = flow_init,
    .destroy            = flow_destroy,
    .capabilities       = flow_caps,
    .get_size           = flow_get_size,
    .render_frame       = flow_render_frame,
    .flush              = flow_flush,
    .render_planes      = flow_render_planes,
    .cursor_set_visible = flow_cursor_visible,
    .poll_event         = flow_poll_event,
};

static void
on_resize(n00b_canvas_t *canvas, void *data)
{
    resize_observer_t *obs = data;
    obs->calls++;
    obs->rows = canvas->frame_rows;
    obs->cols = canvas->frame_cols;
}

static void
test_m2_terminal_flow(void)
{
    flow_widget_state_t left = {};
    flow_widget_state_t right = {};
    resize_observer_t   resize = {};

    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &flow_renderer);
    n00b_canvas_resize(canvas, 8, 40);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = 40;
    root->height = 8;

    n00b_plane_t *left_plane = n00b_new_kargs(n00b_plane_t, plane);
    left_plane->width = 8;
    left_plane->height = 1;
    n00b_widget_attach(left_plane, &flow_widget, &left);

    n00b_plane_t *right_plane = n00b_new_kargs(n00b_plane_t, plane);
    right_plane->width = 8;
    right_plane->height = 1;
    n00b_widget_attach(right_plane, &flow_widget, &right);

    n00b_plane_add_child(root, left_plane, 2, 2);
    n00b_plane_add_child(root, right_plane, 14, 2);
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas,
                     .tick_ms = 100,
                     .on_resize = on_resize,
                     .resize_data = &resize);

    flow_backend_t *ctx = canvas->backend_ctx;
    assert(ctx->poll_calls >= 8);
    assert(ctx->render_calls >= 1);
    assert(ctx->saw_cursor_hide);
    assert(ctx->saw_cursor_show);
    assert(resize.calls == 1);
    assert(resize.rows == 8);
    assert(resize.cols == 40);

    assert(left.key_events == 1);
    assert(left.activations == 0);
    assert(left.mouse_presses == 0);

    assert(right.key_events == 1);
    assert(right.activations == 1);
    assert(right.mouse_presses == 1);

    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_remove_child(root, left_plane);
    n00b_plane_remove_child(root, right_plane);
    n00b_widget_detach(left_plane);
    n00b_widget_detach(right_plane);
    n00b_plane_destroy(left_plane);
    n00b_plane_destroy(right_plane);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] m2 terminal flow\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display M2 terminal-flow integration test...\n");
    test_m2_terminal_flow();

    printf("Display M2 terminal-flow integration test passed.\n");
    n00b_shutdown();
    return 0;
}
