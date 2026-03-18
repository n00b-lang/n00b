#include "n00b.h"
#include "display/render/backend.h"
#include "internal/display/backend_services.h"

static n00b_render_size_t
fallback_size(n00b_canvas_t *canvas)
{
    n00b_render_size_t size = {};
    if (!canvas) {
        return size;
    }

    size.cell_pixel_w = canvas->cell_px_w > 0 ? canvas->cell_px_w : 1;
    size.cell_pixel_h = canvas->cell_px_h > 0 ? canvas->cell_px_h : 1;
    size.pixel_w      = canvas->frame_cols;
    size.pixel_h      = canvas->frame_rows;
    size.cols         = size.cell_pixel_w > 0
                      ? canvas->frame_cols / size.cell_pixel_w
                      : canvas->frame_cols;
    size.rows         = size.cell_pixel_h > 0
                      ? canvas->frame_rows / size.cell_pixel_h
                      : canvas->frame_rows;

    if (size.cols < 1 && canvas->frame_cols > 0) {
        size.cols = canvas->frame_cols;
    }
    if (size.rows < 1 && canvas->frame_rows > 0) {
        size.rows = canvas->frame_rows;
    }

    return size;
}

n00b_render_size_t
n00b_display_backend_get_size(n00b_canvas_t *canvas)
{
    n00b_render_size_t size = fallback_size(canvas);

    if (!n00b_canvas_backend_ready(canvas)
        || !canvas->vtable
        || !canvas->vtable->get_size) {
        return size;
    }

    n00b_render_size_t backend_size = canvas->vtable->get_size(canvas->backend_ctx);

    if (backend_size.cols > 0) {
        size.cols = backend_size.cols;
    }
    if (backend_size.rows > 0) {
        size.rows = backend_size.rows;
    }
    if (backend_size.pixel_w > 0) {
        size.pixel_w = backend_size.pixel_w;
    }
    if (backend_size.pixel_h > 0) {
        size.pixel_h = backend_size.pixel_h;
    }
    if (backend_size.cell_pixel_w > 0) {
        size.cell_pixel_w = backend_size.cell_pixel_w;
    }
    if (backend_size.cell_pixel_h > 0) {
        size.cell_pixel_h = backend_size.cell_pixel_h;
    }

    if (size.cell_pixel_w < 1) {
        size.cell_pixel_w = 1;
    }
    if (size.cell_pixel_h < 1) {
        size.cell_pixel_h = 1;
    }

    return size;
}

n00b_render_cap_t
n00b_display_backend_caps(n00b_canvas_t *canvas)
{
    if (!n00b_canvas_backend_ready(canvas)
        || !canvas->vtable
        || !canvas->vtable->capabilities) {
        return N00B_RCAP_NONE;
    }
    return canvas->vtable->capabilities(canvas->backend_ctx);
}

bool
n00b_display_backend_poll_event(n00b_canvas_t *canvas,
                                 int32_t        timeout_ms,
                                 n00b_event_t  *out)
{
    if (out) {
        out->type = N00B_EVENT_NONE;
    }
    if (!n00b_canvas_backend_ready(canvas)
        || !canvas->vtable
        || !canvas->vtable->poll_event
        || !out) {
        return false;
    }
    return canvas->vtable->poll_event(canvas->backend_ctx, timeout_ms, out);
}

void
n00b_display_backend_set_cursor_visible(n00b_canvas_t *canvas, bool visible)
{
    if (!n00b_canvas_backend_ready(canvas)
        || !canvas->vtable
        || !canvas->vtable->cursor_set_visible) {
        return;
    }
    canvas->vtable->cursor_set_visible(canvas->backend_ctx, visible);
}
