/**
 * Canvas: compositing surface with pluggable renderer backend.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/atomic.h"
#include "render/canvas.h"
#include "render/composite.h"

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

static inline void
canvas_lock(n00b_canvas_t *c)
{
    while (n00b_atomic_or(&c->lock, 1) != 0)
        ;
}

static inline void
canvas_unlock(n00b_canvas_t *c)
{
    n00b_atomic_store(&c->lock, 0);
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
    c->frame      = n00b_alloc_array(n00b_rcell_t,
				     total,
				     .allocator = c->allocator,
				     .no_scan = true);
    c->prev_frame = n00b_alloc_array(n00b_rcell_t,
				     total,
				     .allocator = c->allocator,
				     .no_scan = true);
}

// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

n00b_canvas_t *
n00b_canvas_new(const n00b_renderer_vtable_t *vtable) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    assert(vtable);

    n00b_canvas_t *c = n00b_alloc(n00b_canvas_t, .allocator = allocator);

    c->vtable    = vtable;
    c->allocator = allocator;

    // Initialize backend.
    c->backend_ctx = vtable->init();
    c->caps        = vtable->capabilities(c->backend_ctx);

    // Get initial size from backend.
    n00b_render_size_t sz = vtable->get_size(c->backend_ctx);
    c->frame_rows         = sz.rows;
    c->frame_cols         = sz.cols;

    if (c->frame_rows > 0 && c->frame_cols > 0) {
        canvas_alloc_frames(c);
    }

    c->needs_full_redraw = true;

    return c;
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
    if (c->planes) {
        n00b_free(c->planes);
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

    if (c->num_planes >= c->planes_cap) {
        n00b_isize_t   new_cap = c->planes_cap ? c->planes_cap * 2 : 8;
        n00b_plane_t **new_arr
            = n00b_alloc_array(n00b_plane_t *, new_cap, .allocator = c->allocator);
        if (c->planes) {
            memcpy(new_arr, c->planes, c->num_planes * sizeof(n00b_plane_t *));
            n00b_free(c->planes);
        }
        c->planes     = new_arr;
        c->planes_cap = new_cap;
    }

    c->planes[c->num_planes++] = p;

    canvas_unlock(c);
}

bool
n00b_canvas_remove_plane(n00b_canvas_t *c, n00b_plane_t *p)
{
    canvas_lock(c);

    for (n00b_isize_t i = 0; i < c->num_planes; i++) {
        if (c->planes[i] == p) {
            // Shift remaining.
            for (n00b_isize_t j = i; j + 1 < c->num_planes; j++) {
                c->planes[j] = c->planes[j + 1];
            }
            c->num_planes--;
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
    if (c->vtable->prepare_gui && c->num_planes > 0) {
        c->vtable->prepare_gui(c->backend_ctx, c->planes, c->num_planes);
    }

    // Flatten plane hierarchy.
    n00b_array_t(n00b_composite_entry_t) flat
        = n00b_composite_flatten(c->planes, c->num_planes);

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
