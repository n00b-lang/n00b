/*
 * Canvas: compositing surface with pluggable renderer backend.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/data_lock.h"
#include "core/arena.h"
#include "display/render/canvas.h"
#include "display/render/composite.h"

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

static void
canvas_alloc_frames(n00b_canvas_t *c)
{
    size_t total = (size_t)c->frame_rows * c->frame_cols;

    if (c->frame) {
        n00b_free(c->frame);
    }
    if (c->prev_frame) {
        n00b_free(c->prev_frame);
    }

    // clang-format off
    c->frame      = n00b_alloc_array_with_opts(n00b_rcell_t,
				     total,
				     &(n00b_alloc_opts_t){.allocator = c->allocator,
				                          .no_scan = true});
    c->prev_frame = n00b_alloc_array_with_opts(n00b_rcell_t,
				     total,
				     &(n00b_alloc_opts_t){.allocator = c->allocator,
				                          .no_scan = true});
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
    assert(vtable);

    c->lock      = n00b_data_lock_new();
    c->vtable    = vtable;
    c->allocator = allocator;

    // Initialize backend, passing the output topic.
    c->backend_ctx = vtable->init(output);
    c->caps        = vtable->capabilities(c->backend_ctx);

    // Get initial size from backend.
    n00b_render_size_t sz = vtable->get_size(c->backend_ctx);
    c->frame_rows         = sz.rows;
    c->frame_cols         = sz.cols;

    if (c->frame_rows > 0 && c->frame_cols > 0) {
        canvas_alloc_frames(c);
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

    if (c->frame) {
        n00b_free(c->frame);
    }
    if (c->prev_frame) {
        n00b_free(c->prev_frame);
    }
    if (c->planes.data) {
        n00b_list_free(c->planes);
    }

    n00b_free(c);
}

// -------------------------------------------------------------------
// Plane management
// -------------------------------------------------------------------

void
n00b_canvas_add_plane(n00b_canvas_t *c, n00b_plane_t *p)
{
    canvas_lock(c);

    if (!c->planes.data) {
        c->planes = n00b_list_new(n00b_plane_ptr_t);
    }

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

    // Refresh size from backend, unless the caller has set an explicit
    // size via canvas_resize() (size_set == true).
    if (!c->size_set) {
        n00b_render_size_t sz = c->vtable->get_size(c->backend_ctx);
        if (sz.rows != c->frame_rows || sz.cols != c->frame_cols) {
            c->frame_rows = sz.rows;
            c->frame_cols = sz.cols;
            canvas_alloc_frames(c);
            c->needs_full_redraw = true;
        }
    }

    if (c->frame_rows == 0 || c->frame_cols == 0 || !c->frame) {
        canvas_unlock(c);
        return;
    }

    // GUI prepare hook.
    n00b_isize_t n_planes = (n00b_isize_t)c->planes.len;

    if (c->vtable->prepare_gui && n_planes > 0) {
        c->vtable->prepare_gui(c->backend_ctx, c->planes.data, n_planes);
    }

    // Flatten plane hierarchy.
    n00b_array_t(n00b_composite_entry_t) flat
        = n00b_composite_flatten(c->planes.data, n_planes);

    // Composite into frame.
    n00b_composite_render(flat.data,
                          (n00b_isize_t)flat.len,
                          c->frame,
                          c->frame_rows,
                          c->frame_cols,
                          c->default_style);

    // Degrade based on capabilities.
    n00b_composite_degrade(c->frame, c->frame_rows, c->frame_cols, c->caps);

    // Render to backend.
    n00b_rcell_t *prev = nullptr;
    if ((c->caps & N00B_RCAP_DIFF_RENDER) && !c->needs_full_redraw) {
        prev = c->prev_frame;
    }

    c->vtable->render_frame(c->backend_ctx, c->frame, c->frame_rows, c->frame_cols, prev);
    c->vtable->flush(c->backend_ctx);

    // Swap frames.
    size_t frame_size = sizeof(n00b_rcell_t) * c->frame_rows * c->frame_cols;
    memcpy(c->prev_frame, c->frame, frame_size);
    c->needs_full_redraw = false;

    if (flat.data) {
        n00b_array_free(flat);
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
    canvas_alloc_frames(c);
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
