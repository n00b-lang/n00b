#include "n00b.h"
#include "display/widget.h"
#include "internal/display/backend_services.h"
#include "internal/display/scene_contracts.h"

static bool
plane_tree_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return false;
    }
    if (plane->flags & N00B_PLANE_DIRTY) {
        return true;
    }
    if (!plane->children.data) {
        return false;
    }
    for (size_t i = 0; i < plane->children.len; i++) {
        if (plane_tree_dirty(plane->children.data[i])) {
            return true;
        }
    }
    return false;
}

static void
mark_plane_tree_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }
    plane->flags |= N00B_PLANE_DIRTY;
    if (!plane->children.data) {
        return;
    }
    for (size_t i = 0; i < plane->children.len; i++) {
        n00b_plane_t *child = plane->children.data[i];
        if (child) {
            mark_plane_tree_dirty(child);
        }
    }
}

static void
rerender_dirty_plane_tree(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    if (plane->flags & N00B_PLANE_DIRTY) {
        if (plane->widget_vtable) {
            n00b_widget_render(plane);
        }
        plane->flags &= ~N00B_PLANE_DIRTY;
    }

    if (!plane->children.data) {
        return;
    }

    for (size_t i = 0; i < plane->children.len; i++) {
        n00b_plane_t *child = plane->children.data[i];
        if (child) {
            rerender_dirty_plane_tree(child);
        }
    }
}

n00b_array_t(n00b_composite_entry_t)
n00b_display_scene_build(n00b_canvas_t *canvas)
{
    if (!canvas || !canvas->planes.data || canvas->planes.len == 0) {
        return (n00b_array_t(n00b_composite_entry_t)){};
    }

    int32_t cell_px_w = (int32_t)(canvas->cell_px_w > 0 ? canvas->cell_px_w : 1);
    int32_t cell_px_h = (int32_t)(canvas->cell_px_h > 0 ? canvas->cell_px_h : 1);

    return n00b_composite_flatten(canvas->planes.data,
                                   (n00b_isize_t)canvas->planes.len,
                                   cell_px_w,
                                   cell_px_h);
}

void
n00b_display_scene_free(n00b_array_t(n00b_composite_entry_t) scene)
{
    if (scene.data) {
        n00b_array_free(scene);
    }
}

bool
n00b_display_scene_any_dirty(n00b_canvas_t *canvas)
{
    if (!canvas) {
        return false;
    }
    if (canvas->needs_full_redraw) {
        return true;
    }
    if (!canvas->planes.data) {
        return false;
    }
    for (size_t i = 0; i < canvas->planes.len; i++) {
        n00b_plane_t *p = canvas->planes.data[i];
        if (p && plane_tree_dirty(p)) {
            return true;
        }
    }
    return false;
}

void
n00b_display_scene_mark_all_dirty(n00b_canvas_t *canvas)
{
    if (!canvas || !canvas->planes.data) {
        return;
    }
    for (size_t i = 0; i < canvas->planes.len; i++) {
        n00b_plane_t *p = canvas->planes.data[i];
        if (p) {
            mark_plane_tree_dirty(p);
        }
    }
}

void
n00b_display_scene_rerender_dirty(n00b_canvas_t *canvas)
{
    if (!canvas || !canvas->planes.data) {
        return;
    }
    for (size_t i = 0; i < canvas->planes.len; i++) {
        n00b_plane_t *p = canvas->planes.data[i];
        if (p) {
            rerender_dirty_plane_tree(p);
        }
    }
}

void
n00b_display_scene_run_layout(n00b_canvas_t *canvas)
{
    if (!canvas || !canvas->planes.data) {
        return;
    }

    n00b_render_size_t sz = n00b_display_backend_get_size(canvas);

    int32_t cell_px_w = (int32_t)(sz.cell_pixel_w > 0
                                  ? sz.cell_pixel_w
                                  : (canvas->cell_px_w > 0 ? canvas->cell_px_w : 1));
    int32_t cell_px_h = (int32_t)(sz.cell_pixel_h > 0
                                  ? sz.cell_pixel_h
                                  : (canvas->cell_px_h > 0 ? canvas->cell_px_h : 1));

    int32_t width = (int32_t)(sz.cols > 0
                              ? sz.cols * cell_px_w
                              : canvas->frame_cols);
    int32_t height = (int32_t)(sz.rows > 0
                               ? sz.rows * cell_px_h
                               : canvas->frame_rows);

    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }

    n00b_rect_t root_bounds = {
        .x      = 0,
        .y      = 0,
        .width  = width,
        .height = height,
    };

    for (size_t i = 0; i < canvas->planes.len; i++) {
        n00b_plane_t *p = canvas->planes.data[i];
        if (p) {
            n00b_widget_layout(p, root_bounds);
        }
    }
}
