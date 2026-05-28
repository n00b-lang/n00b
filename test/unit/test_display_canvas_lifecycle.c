#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"

typedef struct {
    int tag;
} lifecycle_backend_ctx_t;

static int g_destroy_calls = 0;

static void *
lifecycle_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;
    lifecycle_backend_ctx_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx != nullptr);
    ctx->tag = 42;
    return ctx;
}

static void
lifecycle_destroy(void *vctx)
{
    lifecycle_backend_ctx_t *ctx = vctx;
    if (!ctx) {
        return;
    }

    g_destroy_calls++;
    free(ctx);
}

static n00b_render_cap_t
lifecycle_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_NONE;
}

static n00b_render_size_t
lifecycle_get_size(void *vctx)
{
    (void)vctx;
    return (n00b_render_size_t){
        .rows = 4,
        .cols = 16,
        .cell_pixel_w = 1,
        .cell_pixel_h = 1,
    };
}

static void
lifecycle_render_frame(void         *vctx,
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
lifecycle_flush(void *vctx)
{
    (void)vctx;
}

static void
lifecycle_render_planes(void                         *vctx,
                        const n00b_composite_entry_t *entries,
                        n00b_isize_t                  count,
                        n00b_isize_t                  total_rows,
                        n00b_isize_t                  total_cols,
                        n00b_text_style_t            *default_style,
                        n00b_render_cap_t             caps)
{
    (void)vctx;
    (void)entries;
    (void)count;
    (void)total_rows;
    (void)total_cols;
    (void)default_style;
    (void)caps;
}

static const n00b_renderer_vtable_t lifecycle_renderer = {
    .name          = "m6_lifecycle",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = lifecycle_init,
    .destroy       = lifecycle_destroy,
    .capabilities  = lifecycle_caps,
    .get_size      = lifecycle_get_size,
    .render_frame  = lifecycle_render_frame,
    .flush         = lifecycle_flush,
    .render_planes = lifecycle_render_planes,
};

static void
test_stack_canvas_backend_name_deinit(void)
{
    n00b_canvas_t canvas = {0};

    n00b_canvas_init(&canvas,
                     .backend_name               = r"stream",
                     .backend_allow_fallback     = false,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = false);
    assert(canvas.backend_ctx != nullptr);
    assert(canvas.vtable == &n00b_renderer_stream);

    n00b_canvas_deinit(&canvas);
    assert(canvas.backend_ctx == nullptr);
    assert(canvas.vtable == nullptr);
    assert(canvas.planes.data == nullptr);

    n00b_canvas_deinit(&canvas);
    assert(canvas.backend_ctx == nullptr);
    assert(canvas.vtable == nullptr);

    n00b_canvas_init(&canvas,
                     .backend_name               = r"stream",
                     .backend_allow_fallback     = false,
                     .backend_allow_dynamic_load = false,
                     .backend_allow_env_override = false);
    assert(canvas.backend_ctx != nullptr);
    assert(canvas.vtable == &n00b_renderer_stream);

    n00b_canvas_deinit(&canvas);
    printf("  [PASS] display canvas stack deinit lifecycle\n");
}

static void
test_direct_vtable_destroy_semantics(void)
{
    g_destroy_calls = 0;

    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas, .vtable = &lifecycle_renderer);
    assert(canvas->backend_ctx != nullptr);
    assert(canvas->vtable == &lifecycle_renderer);

    n00b_canvas_deinit(canvas);
    assert(g_destroy_calls == 1);
    assert(canvas->backend_ctx == nullptr);
    assert(canvas->vtable == nullptr);

    n00b_canvas_destroy(canvas);
    assert(g_destroy_calls == 1);

    canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas, .vtable = &lifecycle_renderer);
    assert(canvas->backend_ctx != nullptr);

    n00b_canvas_destroy(canvas);
    assert(g_destroy_calls == 2);

    printf("  [PASS] display canvas direct-vtable destroy semantics\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display canvas lifecycle tests...\n");
    test_stack_canvas_backend_name_deinit();
    test_direct_vtable_destroy_semantics();
    printf("Display canvas lifecycle tests passed.\n");

    n00b_shutdown();
    return 0;
}
