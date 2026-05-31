/*
 * Mouse hit-testing, event routing, and capture.
 *
 * Ported from slop's ctui/event/dispatch.c.
 */

#include "n00b.h"
#include "display/mouse.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/box.h"
#include "display/render/composite.h"
#include "display/render/types.h"
#include "display/event.h"
#include "display/widget.h"
#include "display/focus.h"
#include "adt/option.h"
#include "internal/display/plane_geometry.h"

static void
plane_absolute_origin(const n00b_plane_t *plane,
                      int32_t            *out_x,
                      int32_t            *out_y)
{
    if (!plane) {
        *out_x = 0;
        *out_y = 0;
        return;
    }

    if (!plane->parent) {
        n00b_plane_resolve_absolute_origin(plane, 0, 0, out_x, out_y);
        return;
    }

    plane_absolute_origin(plane->parent, out_x, out_y);
    n00b_plane_resolve_absolute_origin(plane, *out_x, *out_y, out_x, out_y);
}

static void
mouse_event_to_plane_local(n00b_canvas_t      *canvas,
                           n00b_plane_t       *plane,
                           const n00b_event_t *absolute,
                           n00b_event_t       *localized)
{
    int32_t cpw = 1;
    int32_t cph = 1;
    n00b_entry_info_t info;
    n00b_composite_entry_t entry = {
        .plane = plane,
        .abs_x = 0,
        .abs_y = 0,
    };

    if (!absolute || !localized) {
        return;
    }

    *localized = *absolute;

    if (!canvas || !plane || absolute->type != N00B_EVENT_MOUSE) {
        return;
    }

    cpw = (int32_t)(canvas->cell_px_w > 0 ? canvas->cell_px_w : 1);
    cph = (int32_t)(canvas->cell_px_h > 0 ? canvas->cell_px_h : 1);

    plane_absolute_origin(plane, &entry.abs_x, &entry.abs_y);
    n00b_composite_entry_info(&entry, &info, cpw, cph);

    localized->mouse.x = absolute->mouse.x - info.content_x;
    localized->mouse.y = absolute->mouse.y - info.content_y;
}
// -------------------------------------------------------------------
// Hit testing
// -------------------------------------------------------------------

static bool
canvas_uses_cell_snapped_bounds(const n00b_canvas_t *canvas)
{
    return canvas
        && (canvas->caps & N00B_RCAP_MANAGES_TTY)
        && !(canvas->caps & N00B_RCAP_PIXEL_COORDS);
}

static inline int32_t
imax32(int32_t a, int32_t b) { return a > b ? a : b; }

static inline int32_t
imin32(int32_t a, int32_t b) { return a < b ? a : b; }

static void
entry_visible_rect(const n00b_composite_entry_t *entry,
                   const n00b_canvas_t          *canvas,
                   int32_t                       cell_px_w,
                   int32_t                       cell_px_h,
                   n00b_rect_t                  *out_rect)
{
    n00b_entry_info_t info;
    n00b_composite_entry_info(entry, &info, cell_px_w, cell_px_h);

    n00b_rect_t visible_rect = {
        .x = info.outer_x,
        .y = info.outer_y,
        .width = (int32_t)info.outer_cols,
        .height = (int32_t)info.outer_rows,
    };

    int32_t clip_r = entry->clip_x + entry->clip_w;
    int32_t clip_b = entry->clip_y + entry->clip_h;
    int32_t rect_r = visible_rect.x + visible_rect.width;
    int32_t rect_b = visible_rect.y + visible_rect.height;

    visible_rect.x = imax32(visible_rect.x, entry->clip_x);
    visible_rect.y = imax32(visible_rect.y, entry->clip_y);
    rect_r = imin32(rect_r, clip_r);
    rect_b = imin32(rect_b, clip_b);
    visible_rect.width = imax32(0, rect_r - visible_rect.x);
    visible_rect.height = imax32(0, rect_b - visible_rect.y);

    if (visible_rect.width == 0 || visible_rect.height == 0) {
        *out_rect = visible_rect;
        return;
    }

    if (canvas_uses_cell_snapped_bounds(canvas)) {
        n00b_composite_snap_rect_to_cells(&visible_rect, cell_px_w, cell_px_h);
    }

    *out_rect = visible_rect;
}

static bool
point_in_rect(int32_t x, int32_t y, const n00b_rect_t *rect)
{
    return x >= rect->x
        && x < rect->x + rect->width
        && y >= rect->y
        && y < rect->y + rect->height;
}

static const n00b_composite_entry_t *
mouse_hit_test_entries(const n00b_composite_entry_t *entries,
                       size_t                        count,
                       const n00b_canvas_t         *canvas,
                       int32_t                       x,
                       int32_t                       y,
                       int32_t                       cell_px_w,
                       int32_t                       cell_px_h)
{
    for (size_t i = count; i > 0; i--) {
        const n00b_composite_entry_t *entry = &entries[i - 1];
        n00b_rect_t visible_rect;

        if (!entry->plane) {
            continue;
        }

        entry_visible_rect(entry,
                           canvas,
                           cell_px_w,
                           cell_px_h,
                           &visible_rect);

        if (visible_rect.width == 0 || visible_rect.height == 0) {
            continue;
        }

        if (point_in_rect(x, y, &visible_rect)) {
            return entry;
        }
    }

    return nullptr;
}

static n00b_plane_t *
mouse_hit_test_plane(n00b_plane_t *plane,
                     int32_t       x,
                     int32_t       y,
                     int32_t       cell_px_w,
                     int32_t       cell_px_h)
{
    if (!plane) {
        return nullptr;
    }

    n00b_plane_t *planes[] = { plane };
    n00b_array_t(n00b_composite_entry_t) flat =
        n00b_composite_flatten(planes, 1, cell_px_w, cell_px_h);
    const n00b_composite_entry_t *hit =
        mouse_hit_test_entries(flat.data,
                               flat.len,
                               plane->canvas,
                               x,
                               y,
                               cell_px_w,
                               cell_px_h);
    n00b_plane_t *result = hit ? hit->plane : nullptr;

    if (flat.data) {
        n00b_array_free(flat);
    }

    return result;
}

static void
mouse_cancel_capture_state(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable
        || !plane->widget_vtable->cancel_mouse_capture) {
        return;
    }

    plane->widget_vtable->cancel_mouse_capture(plane, plane->widget_data);
}

static n00b_plane_t *
mouse_nearest_focusable_ancestor(n00b_plane_t *plane)
{
    n00b_plane_t *current = plane;

    while (current && !n00b_widget_can_focus(current)) {
        current = current->parent;
    }

    return current;
}

n00b_option_t(n00b_plane_t *)
n00b_mouse_hit_test(n00b_plane_t *plane, int32_t x, int32_t y,
                     int32_t cell_px_w, int32_t cell_px_h)
{
    return n00b_option_from_nullable(
        n00b_plane_t *,
        mouse_hit_test_plane(plane, x, y, cell_px_w, cell_px_h));
}

// -------------------------------------------------------------------
// Event routing
// -------------------------------------------------------------------

void
n00b_mouse_route_event(n00b_canvas_t          *canvas,
                        struct n00b_focus_mgr_t *fm,
                        const n00b_event_t      *event)
{
    if (!canvas || event->type != N00B_EVENT_MOUSE) {
        return;
    }

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;

    n00b_plane_t *target = nullptr;

    // If a plane has captured the mouse, route everything to it.
    if (canvas->mouse_capture) {
        target = canvas->mouse_capture;
    }
    else {
        if (canvas->planes.data && canvas->planes.len > 0) {
            n00b_array_t(n00b_composite_entry_t) flat =
                n00b_composite_flatten(canvas->planes.data,
                                       (n00b_isize_t)canvas->planes.len,
                                       cpw,
                                       cph);

            const n00b_composite_entry_t *target_entry =
                mouse_hit_test_entries(flat.data,
                                       flat.len,
                                       canvas,
                                       event->mouse.x,
                                       event->mouse.y,
                                       cpw,
                                       cph);
            target = target_entry ? target_entry->plane : nullptr;

            if (flat.data) {
                n00b_array_free(flat);
            }
        }
    }

    if (!target) {
        return;
    }

    // Click-to-focus: on PRESS, focus the nearest focusable ancestor.
    if (event->mouse.action == N00B_MOUSE_PRESS && fm) {
        n00b_plane_t *focus_target = mouse_nearest_focusable_ancestor(target);

        if (focus_target) {
            n00b_focus_mgr_set(fm, focus_target);
        }
    }

    // Dispatch to the target, then bubble up to parents.
    n00b_plane_t *cur = target;
    while (cur) {
        n00b_event_t local_event;
        mouse_event_to_plane_local(canvas, cur, event, &local_event);
        if (n00b_widget_handle_event(cur, &local_event)) {
            return;  // Consumed.
        }
        cur = cur->parent;
    }
}

// -------------------------------------------------------------------
// Mouse capture
// -------------------------------------------------------------------

void
n00b_canvas_capture_mouse(n00b_canvas_t *c, n00b_plane_t *plane)
{
    if (c) {
        c->mouse_capture = plane;
    }
}

void
n00b_canvas_release_mouse(n00b_canvas_t *c)
{
    if (c) {
        c->mouse_capture = nullptr;
    }
}

void
n00b_canvas_cancel_mouse_capture(n00b_canvas_t *c)
{
    if (!c) {
        return;
    }

    mouse_cancel_capture_state(c->mouse_capture);
    c->mouse_capture = nullptr;
}

n00b_option_t(n00b_plane_t *)
n00b_canvas_get_mouse_capture(n00b_canvas_t *c)
{
    return n00b_option_from_nullable(n00b_plane_t *,
                                     n00b_canvas_get_mouse_capture_plane(c));
}

n00b_plane_t *
n00b_canvas_get_mouse_capture_plane(n00b_canvas_t *c)
{
    return c ? c->mouse_capture : nullptr;
}
