#include <stdbool.h>
#include <stdlib.h>

#include "n00b.h"
#include "display/render/backend.h"
#include "display/render/backend_registry.h"

typedef struct {
    n00b_render_size_t size;
} m5_test_ctx_t;

static void *
m5_test_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    (void)output;

    m5_test_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return nullptr;
    }

    ctx->size = (n00b_render_size_t){
        .rows         = 6,
        .cols         = 24,
        .pixel_w      = 24,
        .pixel_h      = 6,
        .cell_pixel_w = 1,
        .cell_pixel_h = 1,
    };

    return ctx;
}

static void
m5_test_destroy(void *vctx)
{
    free(vctx);
}

static n00b_render_cap_t
m5_test_caps(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_UNICODE
         | N00B_RCAP_CURSOR_MOVE;
}

static n00b_render_size_t
m5_test_get_size(void *vctx)
{
    m5_test_ctx_t *ctx = vctx;
    if (!ctx) {
        return (n00b_render_size_t){0};
    }
    return ctx->size;
}

static void
m5_test_render_frame(void         *vctx,
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
m5_test_flush(void *vctx)
{
    (void)vctx;
}

static void
m5_test_render_planes(void                         *vctx,
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

static bool
m5_test_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    (void)vctx;
    (void)timeout_ms;
    if (out) {
        out->type = N00B_EVENT_NONE;
    }
    return false;
}

static const n00b_renderer_vtable_t m5_test_vtable = {
    .name          = "m5_test",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = m5_test_init,
    .destroy       = m5_test_destroy,
    .capabilities  = m5_test_caps,
    .get_size      = m5_test_get_size,
    .render_frame  = m5_test_render_frame,
    .flush         = m5_test_flush,
    .render_planes = m5_test_render_planes,
    .poll_event    = m5_test_poll_event,
};

extern const n00b_renderer_plugin_t n00b_renderer_plugin;

const n00b_renderer_plugin_t n00b_renderer_plugin = {
    .abi_version = N00B_RENDERER_ABI_VERSION,
    .name        = "m5_test",
    .vtable      = &m5_test_vtable,
};
