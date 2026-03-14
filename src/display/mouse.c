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

static inline bool
plane_has_layout_bounds(const n00b_plane_t *plane)
{
    return plane && plane->bounds.width > 0 && plane->bounds.height > 0;
}

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

    if (plane_has_layout_bounds(plane)) {
        *out_x = plane->bounds.x;
        *out_y = plane->bounds.y;
        return;
    }

    if (!plane->parent) {
        *out_x = plane->x;
        *out_y = plane->y;
        return;
    }

    plane_absolute_origin(plane->parent, out_x, out_y);
    *out_x += plane->x;
    *out_y += plane->y;
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

n00b_plane_t *
n00b_mouse_hit_test(n00b_plane_t *plane, int32_t x, int32_t y,
                     int32_t cell_px_w, int32_t cell_px_h)
{
    return mouse_hit_test_plane(plane, x, y, cell_px_w, cell_px_h);
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
    const n00b_composite_entry_t *target_entry = nullptr;

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

            target_entry = mouse_hit_test_entries(flat.data,
                                                  flat.len,
                                                  canvas,
                                                  event->mouse.x,
                                                  event->mouse.y,
                                                  cpw,
                                                  cph);
            target = target_entry ? target_entry->plane : nullptr;

            if (!target_entry && flat.data) {
                n00b_array_free(flat);
            }
            else if (target_entry) {
                // Keep the flattened entry alive until local coordinates are derived.
                target_entry = flat.data + (target_entry - flat.data);
            }

            if (!target) {
                if (flat.data) {
                    n00b_array_free(flat);
                }
                return;
            }

            n00b_entry_info_t info;
            n00b_composite_entry_info(target_entry, &info, cpw, cph);

            n00b_event_t local_event = *event;
            local_event.mouse.x = event->mouse.x - info.content_x;
            local_event.mouse.y = event->mouse.y - info.content_y;

            if (event->mouse.action == N00B_MOUSE_PRESS && fm) {
                if (n00b_widget_can_focus(target)) {
                    n00b_focus_mgr_set(fm, target);
                }
            }

            n00b_plane_t *cur = target;
            while (cur) {
                if (n00b_widget_handle_event(cur, &local_event)) {
                    n00b_array_free(flat);
                    return;
                }
                cur = cur->parent;
            }

            n00b_array_free(flat);
            return;
        }
    }

    if (!target) {
        return;
    }

    // Click-to-focus: on PRESS, focus the hit plane if focusable.
    if (event->mouse.action == N00B_MOUSE_PRESS && fm) {
        if (n00b_widget_can_focus(target)) {
            n00b_focus_mgr_set(fm, target);
        }
    }

    n00b_entry_info_t info;
    n00b_composite_entry_t entry = {
        .plane = target,
        .abs_x = 0,
        .abs_y = 0,
    };
    plane_absolute_origin(target, &entry.abs_x, &entry.abs_y);
    n00b_composite_entry_info(&entry, &info, cpw, cph);

    n00b_event_t local_event = *event;
    local_event.mouse.x = event->mouse.x - info.content_x;
    local_event.mouse.y = event->mouse.y - info.content_y;

    // Dispatch to the target, then bubble up to parents.
    n00b_plane_t *cur = target;
    while (cur) {
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

n00b_plane_t *
n00b_canvas_get_mouse_capture(n00b_canvas_t *c)
{
    return c ? c->mouse_capture : nullptr;
}
