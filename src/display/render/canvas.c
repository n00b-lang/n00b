/*
 * Canvas: compositing surface with pluggable renderer backend.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/data_lock.h"
#include "core/arena.h"
#include "display/render/canvas.h"
#include "display/render/backend_registry.h"
#include "internal/display/backend_services.h"
#include "internal/display/diagnostics.h"
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

static const char *
safe_name(n00b_string_t *name)
{
    if (!name || !name->data || !name->data[0]) {
        return "<null>";
    }
    return name->data;
}

static const char *
safe_vtable_name(const n00b_renderer_vtable_t *vtable)
{
    if (!vtable || !vtable->name || !vtable->name[0]) {
        return "<unknown>";
    }
    return vtable->name;
}

static bool
canvas_bind_backend(n00b_canvas_t                *c,
                    const n00b_renderer_vtable_t *vtable,
                    void                         *backend_ctx)
{
    if (!c || !vtable || !backend_ctx) {
        return false;
    }

    c->vtable     = vtable;
    c->backend_ctx = backend_ctx;
    c->caps       = n00b_display_backend_caps(c);

    // Get initial size from backend (cells) and convert to pixels.
    n00b_render_size_t sz = n00b_display_backend_get_size(c);
    c->cell_px_w          = sz.cell_pixel_w > 0 ? sz.cell_pixel_w : 1;
    c->cell_px_h          = sz.cell_pixel_h > 0 ? sz.cell_pixel_h : 1;
    c->frame_rows         = sz.rows * c->cell_px_h;
    c->frame_cols         = sz.cols * c->cell_px_w;

    // Initialize font metrics: use backend metrics only when explicitly
    // advertised; otherwise use deterministic cell fallback metrics.
    if (vtable->get_font_metrics && (c->caps & N00B_RCAP_FONT_METRICS)) {
        c->metrics = vtable->get_font_metrics(c->backend_ctx);
    }
    else {
        c->metrics = n00b_font_metrics_fallback((int32_t)c->cell_px_w,
                                                (int32_t)c->cell_px_h);
    }

    c->needs_full_redraw = true;
    return true;
}

static bool
canvas_try_vtable(n00b_canvas_t                       *c,
                  const n00b_renderer_vtable_t        *vtable,
                  n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    if (!vtable || !vtable->init) {
        return false;
    }

    void *backend_ctx = vtable->init(output);
    if (!backend_ctx) {
        return false;
    }

    return canvas_bind_backend(c, vtable, backend_ctx);
}

// -------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------

void
n00b_canvas_init(n00b_canvas_t *c) _kargs
{
    const n00b_renderer_vtable_t           *vtable    = nullptr;
    n00b_string_t                          *backend_name = nullptr;
    bool                                    backend_allow_fallback = true;
    bool                                    backend_allow_dynamic_load = true;
    bool                                    backend_allow_env_override = true;
    n00b_allocator_t                       *allocator = nullptr;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output    = nullptr;
}
{
    if (!c) {
        return;
    }

    c->lock      = n00b_data_lock_new();
    c->allocator = allocator;
    c->vtable    = nullptr;
    c->backend_ctx = nullptr;
    c->caps      = N00B_RCAP_NONE;
    c->cell_px_w = 1;
    c->cell_px_h = 1;
    c->metrics   = n00b_font_metrics_fallback(1, 1);
    c->needs_full_redraw = true;

    if (vtable) {
        if (!canvas_try_vtable(c, vtable, output)) {
            c->vtable = vtable;
            n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                                  "canvas",
                                  "backend init failed for direct vtable '%s'",
                                  safe_vtable_name(vtable));
        }
        return;
    }

    n00b_string_t *requested = backend_name ? backend_name : r"auto";
    n00b_list_t(n00b_string_t *) candidates =
        n00b_renderer_candidate_names(requested,
                                       .allow_fallback     = backend_allow_fallback,
                                       .allow_env_override = backend_allow_env_override);

    for (size_t i = 0; i < candidates.len; i++) {
        n00b_string_t *candidate_name = n00b_list_get(candidates, i);
        n00b_result_t(n00b_renderer_vtable_ptr_t) resolved =
            n00b_renderer_resolve_exact(candidate_name,
                                        .allow_dynamic_load = backend_allow_dynamic_load);
        if (!n00b_result_is_ok(resolved)) {
            n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                  "canvas",
                                  "backend candidate unresolved: requested=%s candidate=%s err=%d",
                                  safe_name(requested),
                                  safe_name(candidate_name),
                                  n00b_result_get_err(resolved));
            continue;
        }

        const n00b_renderer_vtable_t *candidate_vtable = n00b_result_get(resolved);
        if (!candidate_vtable) {
            continue;
        }

        if (canvas_try_vtable(c, candidate_vtable, output)) {
            n00b_display_diag_log(N00B_DISPLAY_DIAG_INFO,
                                  "canvas",
                                  "backend selected: requested=%s selected=%s fallback=%s",
                                  safe_name(requested),
                                  safe_vtable_name(candidate_vtable),
                                  i == 0 ? "false" : "true");
            return;
        }

        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                              "canvas",
                              "backend candidate init failed: requested=%s candidate=%s",
                              safe_name(requested),
                              safe_vtable_name(candidate_vtable));
    }

    n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                          "canvas",
                          "backend selection failed: requested=%s candidates=%zu",
                          safe_name(requested),
                          candidates.len);
}

void
n00b_canvas_deinit(n00b_canvas_t *c)
{
    if (!c) {
        return;
    }

    if (c->vtable && c->vtable->destroy && c->backend_ctx) {
        c->vtable->destroy(c->backend_ctx);
    }

    if (c->planes.data) {
        n00b_list_free(c->planes);
    }
    if (c->lock) {
        n00b_finalize_data_lock(c->lock);
    }

    c->vtable            = nullptr;
    c->backend_ctx       = nullptr;
    c->caps              = N00B_RCAP_NONE;
    c->frame_rows        = 0;
    c->frame_cols        = 0;
    c->default_style     = nullptr;
    c->cell_px_w         = 1;
    c->cell_px_h         = 1;
    c->metrics           = n00b_font_metrics_fallback(1, 1);
    c->needs_full_redraw = true;
    c->size_set          = false;
    c->lock              = nullptr;
    c->allocator         = nullptr;
    c->focus             = nullptr;
    c->mouse_capture     = nullptr;
}

void
n00b_canvas_destroy(n00b_canvas_t *c)
{
    if (!c) {
        return;
    }

    n00b_canvas_deinit(c);
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
        n00b_isize_t prev_cell_w = c->cell_px_w;
        n00b_isize_t prev_cell_h = c->cell_px_h;
        n00b_isize_t prev_rows   = c->frame_rows;
        n00b_isize_t prev_cols   = c->frame_cols;

        n00b_render_size_t sz = n00b_display_backend_get_size(c);
        c->cell_px_w = sz.cell_pixel_w > 0 ? sz.cell_pixel_w : 1;
        c->cell_px_h = sz.cell_pixel_h > 0 ? sz.cell_pixel_h : 1;
        n00b_isize_t px_rows = sz.rows * c->cell_px_h;
        n00b_isize_t px_cols = sz.cols * c->cell_px_w;
        bool size_changed = (px_rows != prev_rows || px_cols != prev_cols);
        bool cell_changed = (c->cell_px_w != prev_cell_w
                             || c->cell_px_h != prev_cell_h);
        if (size_changed) {
            c->frame_rows = px_rows;
            c->frame_cols = px_cols;
            c->needs_full_redraw = true;
        }

        // Refresh fallback metrics if backend doesn't provide its own.
        if (!c->vtable->get_font_metrics) {
            c->metrics = n00b_font_metrics_fallback((int32_t)c->cell_px_w,
                                                      (int32_t)c->cell_px_h);
        }

        // If backend cell metrics changed (even without a resize event),
        // existing pixel-layout assignments are stale. Re-run layout now.
        if (cell_changed) {
            n00b_display_scene_run_layout(c);
            n00b_display_scene_mark_all_dirty(c);
            c->needs_full_redraw = true;
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
