#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <X11/X.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event_loop.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "internal/display/x11_backend_contracts.h"

typedef struct {
    n00b_render_size_t size;
    int                poll_calls;
    int                render_calls;
    n00b_isize_t       last_render_rows;
    n00b_isize_t       last_render_cols;
} resize_contract_ctx_t;

static void *
resize_contract_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    resize_contract_ctx_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != nullptr);
    ctx->size.rows = 4;
    ctx->size.cols = 10;
    ctx->size.cell_pixel_w = 9;
    ctx->size.cell_pixel_h = 16;
    ctx->size.pixel_w = 90;
    ctx->size.pixel_h = 64;
    return ctx;
}

static void
resize_contract_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
resize_contract_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_MANAGES_TTY | N00B_RCAP_PIXEL_COORDS;
}

static n00b_render_size_t
resize_contract_size(void *vctx)
{
    resize_contract_ctx_t *ctx = vctx;
    return ctx->size;
}

static void
resize_contract_render_frame(void         *vctx,
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
resize_contract_flush(void *vctx)
{
    (void)vctx;
}

static void
resize_contract_render_planes(void                         *vctx,
                              const n00b_composite_entry_t *entries,
                              n00b_isize_t                  count,
                              n00b_isize_t                  total_rows,
                              n00b_isize_t                  total_cols,
                              n00b_text_style_t            *default_style,
                              n00b_render_cap_t             caps)
{
    resize_contract_ctx_t *ctx = vctx;
    (void)entries;
    (void)count;
    (void)default_style;
    (void)caps;

    ctx->render_calls++;
    ctx->last_render_rows = total_rows;
    ctx->last_render_cols = total_cols;
}

static bool
resize_contract_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    resize_contract_ctx_t *ctx = vctx;
    (void)timeout_ms;

    if (ctx->poll_calls == 0) {
        ctx->poll_calls++;
        ctx->size.rows = 2;
        ctx->size.cols = 11;
        ctx->size.pixel_w = 101;
        ctx->size.pixel_h = 33;
        *out = (n00b_event_t){
            .type = N00B_EVENT_RESIZE,
            .resize = {.rows = 2, .cols = 11},
        };
        return true;
    }

    if (ctx->poll_calls == 1) {
        ctx->poll_calls++;
        *out = (n00b_event_t){
            .type = N00B_EVENT_KEY,
            .key = {.key = (uint32_t)'c', .mods = N00B_MOD_CTRL},
        };
        return true;
    }

    out->type = N00B_EVENT_NONE;
    return false;
}

static const n00b_renderer_vtable_t resize_contract_renderer = {
    .name = "x11_resize_contract",
    .version = N00B_RENDERER_ABI_VERSION,
    .init = resize_contract_init,
    .destroy = resize_contract_destroy,
    .capabilities = resize_contract_caps,
    .get_size = resize_contract_size,
    .render_frame = resize_contract_render_frame,
    .flush = resize_contract_flush,
    .render_planes = resize_contract_render_planes,
    .poll_event = resize_contract_poll_event,
};

static void
test_utf8_lookup_bytes_decode_full_codepoint(void)
{
    n00b_event_t ev = {};
    assert(n00b_x11_translate_lookup_bytes("\xC3\xA9", 2, N00B_MOD_NONE, &ev));
    assert(ev.type == N00B_EVENT_KEY);
    assert(ev.key.key == 0xE9u);
    assert(ev.key.mods == N00B_MOD_NONE);

    printf("  [PASS] x11 UTF-8 lookup bytes decode a full codepoint\n");
}

static void
test_motion_state_maps_drag_semantics(void)
{
    n00b_mouse_button_t button = N00B_MOUSE_NONE;
    n00b_mouse_action_t action = N00B_MOUSE_MOVE;

    n00b_x11_translate_motion_state(Button1Mask, &button, &action);
    assert(button == N00B_MOUSE_LEFT);
    assert(action == N00B_MOUSE_DRAG);

    n00b_x11_translate_motion_state(0, &button, &action);
    assert(button == N00B_MOUSE_NONE);
    assert(action == N00B_MOUSE_MOVE);

    printf("  [PASS] x11 motion state maps drag semantics\n");
}

static void
test_expose_schedules_synthetic_repaint(void)
{
    n00b_x11_pending_state_t pending = {};
    n00b_event_t ev = {};

    n00b_x11_note_expose(&pending, 1);
    assert(!n00b_x11_take_pending_event(&pending, 8, 40, &ev));

    n00b_x11_note_expose(&pending, 0);
    assert(n00b_x11_take_pending_event(&pending, 8, 40, &ev));
    assert(ev.type == N00B_EVENT_RESIZE);
    assert(ev.resize.rows == 8);
    assert(ev.resize.cols == 40);

    printf("  [PASS] x11 expose schedules a repaint event\n");
}

static void
test_resize_preserves_exact_pixel_extent(void)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                           .vtable = &resize_contract_renderer);
    resize_contract_ctx_t *ctx = canvas->backend_ctx;

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = canvas->frame_cols;
    root->height = canvas->frame_rows;
    n00b_canvas_add_plane(canvas, root);

    n00b_canvas_run(canvas, .tick_ms = 100);

    assert(ctx->render_calls >= 2);
    assert(ctx->last_render_rows == 33);
    assert(ctx->last_render_cols == 101);

    n00b_canvas_remove_plane(canvas, root);
    n00b_plane_destroy(root);
    n00b_canvas_destroy(canvas);

    printf("  [PASS] x11-style resize keeps exact pixel extents\n");
}

static void
test_x11_capabilities_are_honest(void)
{
    n00b_render_cap_t caps = n00b_renderer_x11.capabilities(nullptr);
    assert(caps & N00B_RCAP_MANAGES_TTY);
    assert(caps & N00B_RCAP_PIXEL_COORDS);
    assert(!(caps & N00B_RCAP_FONT_METRICS));
    assert(!(caps & N00B_RCAP_DIFF_RENDER));

    printf("  [PASS] x11 capabilities match implemented surface\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display x11-contract tests...\n");
    test_utf8_lookup_bytes_decode_full_codepoint();
    test_motion_state_maps_drag_semantics();
    test_expose_schedules_synthetic_repaint();
    test_resize_preserves_exact_pixel_extent();
    test_x11_capabilities_are_honest();

    printf("Display x11-contract tests passed.\n");
    n00b_shutdown();
    return 0;
}
