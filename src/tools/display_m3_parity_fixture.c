#include <stdbool.h>
#include <stdlib.h>

#include "n00b.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "internal/display/backend_services.h"
#include "display_m3_parity_fixture.h"

typedef struct {
    n00b_event_t       events[16];
    size_t             count;
    size_t             ix;
    int                poll_calls;
    int                render_calls;
    bool               saw_cursor_hide;
    bool               saw_cursor_show;
    bool               gui_mode;
    n00b_render_size_t size;
} parity_backend_t;

typedef struct {
    int key_events;
    int activations;
    int mouse_presses;
} parity_widget_state_t;

typedef struct {
    int          calls;
    n00b_isize_t rows;
    n00b_isize_t cols;
} parity_resize_observer_t;

static void
populate_script(parity_backend_t *ctx)
{
    int32_t cell_w  = (int32_t)(ctx->size.cell_pixel_w > 0 ? ctx->size.cell_pixel_w : 1);
    int32_t cell_h  = (int32_t)(ctx->size.cell_pixel_h > 0 ? ctx->size.cell_pixel_h : 1);
    int32_t mouse_x = 14 * cell_w + (cell_w > 1 ? 1 : 0);
    int32_t mouse_y = 2 * cell_h + (cell_h > 1 ? 1 : 0);

    ctx->events[0] = (n00b_event_t){
        .type = N00B_EVENT_RESIZE,
        .resize = {.rows = 8, .cols = 40},
    };
    ctx->events[1] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {
            .key = ctx->gui_mode ? N00B_KEY_TAB : (uint32_t)'\t',
            .mods = N00B_MOD_NONE,
        },
    };
    ctx->events[2] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'x', .mods = N00B_MOD_NONE},
    };
    ctx->events[3] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_SHIFT},
    };
    ctx->events[4] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'y', .mods = N00B_MOD_NONE},
    };
    ctx->events[5] = (n00b_event_t){
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x = mouse_x,
            .y = mouse_y,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods = N00B_MOD_NONE,
        },
    };
    ctx->events[6] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)' ', .mods = N00B_MOD_NONE},
    };
    ctx->events[7] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {
            .key = ctx->gui_mode ? N00B_KEY_ENTER : (uint32_t)'\r',
            .mods = N00B_MOD_NONE,
        },
    };
    ctx->events[8] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {
            .key = ctx->gui_mode ? (uint32_t)'c' : (uint32_t)3,
            .mods = ctx->gui_mode ? N00B_MOD_CTRL : N00B_MOD_NONE,
        },
    };
    ctx->events[9] = (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key = {.key = (uint32_t)'z', .mods = N00B_MOD_NONE},
    };

    ctx->count = 10;
}

static void *
parity_init_mode(bool gui_mode)
{
    parity_backend_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->gui_mode = gui_mode;
    ctx->size.rows = 8;
    ctx->size.cols = 40;
    ctx->size.cell_pixel_w = gui_mode ? 9 : 1;
    ctx->size.cell_pixel_h = gui_mode ? 16 : 1;
    ctx->size.pixel_w = ctx->size.cols * ctx->size.cell_pixel_w;
    ctx->size.pixel_h = ctx->size.rows * ctx->size.cell_pixel_h;
    populate_script(ctx);

    return ctx;
}

static void *
parity_init_terminal(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    return parity_init_mode(false);
}

static void *
parity_init_gui(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    return parity_init_mode(true);
}

static void
parity_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
parity_caps(void *vctx)
{
    parity_backend_t *ctx = vctx;
    if (ctx->gui_mode) {
        return N00B_RCAP_MANAGES_TTY
             | N00B_RCAP_COLOR_24BIT
             | N00B_RCAP_MOUSE
             | N00B_RCAP_UNICODE
             | N00B_RCAP_WIDE_CHARS
             | N00B_RCAP_PIXEL_COORDS
             | N00B_RCAP_GUI_EXT;
    }

    return N00B_RCAP_MANAGES_TTY
         | N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_CURSOR_MOVE
         | N00B_RCAP_ALT_SCREEN
         | N00B_RCAP_MOUSE
         | N00B_RCAP_UNICODE
         | N00B_RCAP_WIDE_CHARS;
}

static n00b_render_size_t
parity_get_size(void *vctx)
{
    parity_backend_t *ctx = vctx;
    return ctx->size;
}

static void
parity_render_frame(void         *vctx,
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
parity_flush(void *vctx)
{
    (void)vctx;
}

static void
parity_render_planes(void                         *vctx,
                     const n00b_composite_entry_t *entries,
                     n00b_isize_t                  count,
                     n00b_isize_t                  total_rows,
                     n00b_isize_t                  total_cols,
                     n00b_text_style_t            *default_style,
                     n00b_render_cap_t             caps)
{
    parity_backend_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
    ctx->render_calls++;
}

static void
parity_cursor_visible(void *vctx, bool visible)
{
    parity_backend_t *ctx = vctx;
    if (visible) {
        ctx->saw_cursor_show = true;
    }
    else {
        ctx->saw_cursor_hide = true;
    }
}

static bool
parity_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    parity_backend_t *ctx = vctx;
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
parity_widget_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
    n00b_plane_draw_glyph(plane, 0, 0, 'P');
}

static bool
parity_widget_handle(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    parity_widget_state_t *state = data;

    if (event->type == N00B_EVENT_MOUSE
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS) {
        state->mouse_presses++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && (event->key.key == (uint32_t)' ' || event->key.key == N00B_KEY_ENTER)) {
        state->activations++;
        return true;
    }

    if (event->type == N00B_EVENT_KEY
        && event->key.mods == N00B_MOD_NONE
        && event->key.key >= (uint32_t)'a'
        && event->key.key <= (uint32_t)'z') {
        state->key_events++;
        return true;
    }

    return false;
}

static bool
parity_widget_focusable(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t parity_widget = {
    .kind         = "m3_synthetic_parity",
    .render       = parity_widget_render,
    .handle_event = parity_widget_handle,
    .can_focus    = parity_widget_focusable,
};

static const n00b_renderer_vtable_t parity_terminal_renderer = {
    .name               = "m3_terminal_profile",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = parity_init_terminal,
    .destroy            = parity_destroy,
    .capabilities       = parity_caps,
    .get_size           = parity_get_size,
    .render_frame       = parity_render_frame,
    .flush              = parity_flush,
    .render_planes      = parity_render_planes,
    .cursor_set_visible = parity_cursor_visible,
    .poll_event         = parity_poll_event,
};

static const n00b_renderer_vtable_t parity_gui_renderer = {
    .name               = "m3_synthetic_gui_profile",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = parity_init_gui,
    .destroy            = parity_destroy,
    .capabilities       = parity_caps,
    .get_size           = parity_get_size,
    .render_frame       = parity_render_frame,
    .flush              = parity_flush,
    .render_planes      = parity_render_planes,
    .cursor_set_visible = parity_cursor_visible,
    .poll_event         = parity_poll_event,
};

static void
on_resize(n00b_canvas_t *canvas, void *data)
{
    parity_resize_observer_t *obs = data;
    obs->calls++;
    obs->rows = canvas->cell_px_h > 0
              ? canvas->frame_rows / canvas->cell_px_h
              : canvas->frame_rows;
    obs->cols = canvas->cell_px_w > 0
              ? canvas->frame_cols / canvas->cell_px_w
              : canvas->frame_cols;
}

int
n00b_m3_parity_run_case(bool                      gui_mode,
                        n00b_m3_parity_result_t  *out)
{
    if (!out) {
        return -1;
    }

    const n00b_renderer_vtable_t *renderer = gui_mode
        ? &parity_gui_renderer
        : &parity_terminal_renderer;

    int rc = -1;
    n00b_canvas_t *canvas = nullptr;
    n00b_plane_t *root = nullptr;
    n00b_plane_t *left_plane = nullptr;
    n00b_plane_t *right_plane = nullptr;
    parity_widget_state_t left = {};
    parity_widget_state_t right = {};
    parity_resize_observer_t resize = {};

    canvas = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = renderer);
    if (!canvas) {
        goto done;
    }

    n00b_render_size_t size = n00b_display_backend_get_size(canvas);
    n00b_isize_t px_rows = size.pixel_h > 0
                         ? size.pixel_h
                         : size.rows * canvas->cell_px_h;
    n00b_isize_t px_cols = size.pixel_w > 0
                         ? size.pixel_w
                         : size.cols * canvas->cell_px_w;
    n00b_canvas_resize(canvas, px_rows, px_cols);

    root = n00b_new_kargs(n00b_plane_t, plane);
    left_plane = n00b_new_kargs(n00b_plane_t, plane);
    right_plane = n00b_new_kargs(n00b_plane_t, plane);
    if (!root || !left_plane || !right_plane) {
        goto done;
    }

    root->width = px_cols;
    root->height = px_rows;
    left_plane->width = 8 * canvas->cell_px_w;
    left_plane->height = 1 * canvas->cell_px_h;
    right_plane->width = 8 * canvas->cell_px_w;
    right_plane->height = 1 * canvas->cell_px_h;

    n00b_widget_attach(left_plane, &parity_widget, &left);
    n00b_widget_attach(right_plane, &parity_widget, &right);
    n00b_plane_add_child(root, left_plane, 2 * canvas->cell_px_w, 2 * canvas->cell_px_h);
    n00b_plane_add_child(root, right_plane, 14 * canvas->cell_px_w, 2 * canvas->cell_px_h);
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas,
                    .tick_ms = 100,
                    .on_resize = on_resize,
                    .resize_data = &resize);

    parity_backend_t *ctx = canvas->backend_ctx;
    *out = (n00b_m3_parity_result_t){
        .gui_mode = gui_mode,
        .resize_calls = resize.calls,
        .resize_rows = resize.rows,
        .resize_cols = resize.cols,
        .left_key_events = left.key_events,
        .right_key_events = right.key_events,
        .left_activations = left.activations,
        .right_activations = right.activations,
        .left_mouse_presses = left.mouse_presses,
        .right_mouse_presses = right.mouse_presses,
        .poll_calls = ctx->poll_calls,
        .render_calls = ctx->render_calls,
        .cursor_hide = ctx->saw_cursor_hide,
        .cursor_show = ctx->saw_cursor_show,
        .events_consumed = ctx->ix,
        .events_total = ctx->count,
    };
    if (ctx->ix < ctx->count && ctx->events[ctx->ix].type == N00B_EVENT_KEY) {
        out->next_key_after_stop = ctx->events[ctx->ix].key.key;
    }

    rc = 0;

done:
    if (canvas && root) {
        n00b_canvas_remove_plane(canvas, root);
    }
    if (root && left_plane) {
        n00b_plane_remove_child(root, left_plane);
    }
    if (root && right_plane) {
        n00b_plane_remove_child(root, right_plane);
    }
    if (left_plane) {
        n00b_widget_detach(left_plane);
        n00b_plane_destroy(left_plane);
    }
    if (right_plane) {
        n00b_widget_detach(right_plane);
        n00b_plane_destroy(right_plane);
    }
    if (root) {
        n00b_plane_destroy(root);
    }
    if (canvas) {
        n00b_canvas_destroy(canvas);
    }

    return rc;
}

bool
n00b_m3_parity_equivalent(const n00b_m3_parity_result_t *terminal,
                          const n00b_m3_parity_result_t *gui)
{
    if (!terminal || !gui) {
        return false;
    }

    return terminal->resize_calls == gui->resize_calls
        && terminal->resize_rows == gui->resize_rows
        && terminal->resize_cols == gui->resize_cols
        && terminal->left_key_events == gui->left_key_events
        && terminal->right_key_events == gui->right_key_events
        && terminal->left_activations == gui->left_activations
        && terminal->right_activations == gui->right_activations
        && terminal->left_mouse_presses == gui->left_mouse_presses
        && terminal->right_mouse_presses == gui->right_mouse_presses
        && terminal->cursor_hide == gui->cursor_hide
        && terminal->cursor_show == gui->cursor_show
        && terminal->events_consumed == gui->events_consumed
        && terminal->events_total == gui->events_total
        && terminal->next_key_after_stop == gui->next_key_after_stop;
}
