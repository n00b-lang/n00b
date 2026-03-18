/*
 * Canvas: compositing surface with pluggable renderer backend.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/data_lock.h"
#include "core/arena.h"
#include "display/render/canvas.h"
#include "internal/display/backend_services.h"
#include "internal/display/scene_contracts.h"

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

static inline void
canvas_lock(n00b_canvas_t *c)
{
    n00b_data_write_lock(c->lock);
}

static inline void
canvas_unlock(n00b_canvas_t *c)
{
    n00b_data_unlock(c->lock);
}

static inline n00b_isize_t
canvas_size_in_pixels(n00b_isize_t pixels,
                      n00b_isize_t cells,
                      n00b_isize_t cell_px)
{
    if (pixels > 0) {
        return pixels;
    }

    if (cells > 0 && cell_px > 0) {
        return cells * cell_px;
    }

    return 0;
}

// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

void
n00b_canvas_init(n00b_canvas_t *c) _kargs
{
    const n00b_renderer_vtable_t           *vtable    = nullptr;
    n00b_allocator_t                       *allocator = nullptr;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output    = nullptr;
}
{
    assert(c);
    assert(vtable);

    *c = (n00b_canvas_t){
        .vtable    = vtable,
        .allocator = allocator,
    };

    c->lock      = n00b_data_lock_new();

    // Initialize backend, passing the output topic.
    c->backend_ctx = vtable->init(output);
    c->caps        = n00b_display_backend_caps(c);

    // Get initial size from backend (cells) and convert to pixels.
    n00b_render_size_t sz = n00b_display_backend_get_size(c);
    c->cell_px_w          = sz.cell_pixel_w > 0 ? sz.cell_pixel_w : 1;
    c->cell_px_h          = sz.cell_pixel_h > 0 ? sz.cell_pixel_h : 1;
    c->frame_rows         = canvas_size_in_pixels(sz.pixel_h, sz.rows, c->cell_px_h);
    c->frame_cols         = canvas_size_in_pixels(sz.pixel_w, sz.cols, c->cell_px_w);

    // Initialize font metrics: ask backend first, fall back to cell metrics.
    if (vtable->get_font_metrics) {
        c->metrics = vtable->get_font_metrics(c->backend_ctx);
    }
    else {
        c->metrics = n00b_font_metrics_fallback((int32_t)c->cell_px_w,
                                                  (int32_t)c->cell_px_h);
    }

    c->needs_full_redraw = true;
}

void
n00b_canvas_destroy(n00b_canvas_t *c)
{
    if (!c) {
        return;
    }

    if (c->vtable && c->vtable->destroy) {
        c->vtable->destroy(c->backend_ctx);
    }

    if (c->planes.data) {
        n00b_list_free(c->planes);
    }

    n00b_free(c);
}

// -------------------------------------------------------------------
// Plane management
// -------------------------------------------------------------------

static void
propagate_canvas(n00b_plane_t *p, n00b_canvas_t *c)
{
    p->canvas = c;
    p->flags |= N00B_PLANE_DIRTY;

    if (p->children.data) {
        for (size_t i = 0; i < p->children.len; i++) {
            n00b_plane_t *child = p->children.data[i];
            if (child) {
                propagate_canvas(child, c);
            }
        }
    }
}

void
n00b_canvas_add_plane(n00b_canvas_t *c, n00b_plane_t *p)
{
    canvas_lock(c);

    if (!c->planes.data) {
        c->planes = n00b_list_new(n00b_plane_ptr_t);
    }

    // Set canvas back-pointer on the plane and all its children
    // so they can access font metrics, and mark dirty so their
    // draw commands get regenerated with proper metrics.
    propagate_canvas(p, c);

    n00b_list_push(c->planes, p);

    canvas_unlock(c);
}

bool
n00b_canvas_remove_plane(n00b_canvas_t *c, n00b_plane_t *p)
{
    canvas_lock(c);

    size_t n = c->planes.len;
    for (size_t i = 0; i < n; i++) {
        if (n00b_list_get(c->planes, i) == p) {
            (void)n00b_list_delete(c->planes, i);
            p->canvas = nullptr;
            canvas_unlock(c);
            return true;
        }
    }

    canvas_unlock(c);
    return false;
}

// -------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------

void
n00b_canvas_render(n00b_canvas_t *c)
{
    canvas_lock(c);

    // Refresh size from backend (cells → pixels), unless the caller
    // has set an explicit size via canvas_resize() (size_set == true).
    if (!c->size_set) {
        n00b_render_size_t sz = n00b_display_backend_get_size(c);
        c->cell_px_w = sz.cell_pixel_w > 0 ? sz.cell_pixel_w : 1;
        c->cell_px_h = sz.cell_pixel_h > 0 ? sz.cell_pixel_h : 1;
        n00b_isize_t px_rows = canvas_size_in_pixels(sz.pixel_h, sz.rows, c->cell_px_h);
        n00b_isize_t px_cols = canvas_size_in_pixels(sz.pixel_w, sz.cols, c->cell_px_w);
        if (px_rows != c->frame_rows || px_cols != c->frame_cols) {
            c->frame_rows = px_rows;
            c->frame_cols = px_cols;
            c->needs_full_redraw = true;
        }

        // Refresh fallback metrics if backend doesn't provide its own.
        if (!c->vtable->get_font_metrics) {
            c->metrics = n00b_font_metrics_fallback((int32_t)c->cell_px_w,
                                                      (int32_t)c->cell_px_h);
        }
    }

    if (c->frame_rows == 0 || c->frame_cols == 0) {
        canvas_unlock(c);
        return;
    }

    // Re-render any dirty widget planes so draw commands reflect
    // current viewport sizes and font metrics.
    n00b_display_scene_rerender_dirty(c);

    // GUI prepare hook.
    n00b_isize_t n_planes = (n00b_isize_t)c->planes.len;

    if (c->vtable->prepare_gui && n_planes > 0) {
        c->vtable->prepare_gui(c->backend_ctx, c->planes.data, n_planes);
    }

    // Flatten plane hierarchy (pixel coordinates).
    n00b_array_t(n00b_composite_entry_t) flat = n00b_display_scene_build(c);

    // Dispatch to backend's plane-based renderer.
    c->vtable->render_planes(c->backend_ctx,
                              flat.data, (n00b_isize_t)flat.len,
                              c->frame_rows, c->frame_cols,
                              c->default_style, c->caps);
    c->vtable->flush(c->backend_ctx);

    c->needs_full_redraw = false;

    if (flat.data) {
        n00b_display_scene_free(flat);
    }

    canvas_unlock(c);
}

void
n00b_canvas_invalidate(n00b_canvas_t *c)
{
    c->needs_full_redraw = true;
}

void
n00b_canvas_resize(n00b_canvas_t *c, n00b_isize_t rows, n00b_isize_t cols)
{
    canvas_lock(c);

    c->frame_rows = rows;
    c->frame_cols = cols;
    c->size_set   = true;

    c->needs_full_redraw = true;

    canvas_unlock(c);
}

void
n00b_canvas_flush(n00b_canvas_t *c)
{
    if (c->vtable && c->vtable->flush) {
        c->vtable->flush(c->backend_ctx);
    }
}

void
n00b_canvas_alt_screen_enter(n00b_canvas_t *c)
{
    if (c->vtable && c->vtable->alt_screen_enter) {
        c->vtable->alt_screen_enter(c->backend_ctx);
    }
}

void
n00b_canvas_alt_screen_leave(n00b_canvas_t *c)
{
    if (c->vtable && c->vtable->alt_screen_leave) {
        c->vtable->alt_screen_leave(c->backend_ctx);
    }
}
