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

static void
plane_entry_info_at(n00b_plane_t *plane,
                    int32_t       abs_x,
                    int32_t       abs_y,
                    int32_t       cell_px_w,
                    int32_t       cell_px_h,
                    n00b_entry_info_t *out)
{
    n00b_composite_entry_t entry = {
        .plane = plane,
        .abs_x = abs_x,
        .abs_y = abs_y,
    };
    n00b_composite_entry_info(&entry, out, cell_px_w, cell_px_h);
}

static n00b_plane_t *
mouse_hit_test_absolute(n00b_plane_t *plane,
                        int32_t       parent_abs_x,
                        int32_t       parent_abs_y,
                        int32_t       x,
                        int32_t       y,
                        int32_t       cell_px_w,
                        int32_t       cell_px_h)
{
    if (!plane) {
        return nullptr;
    }
    if (!(plane->flags & N00B_PLANE_VISIBLE)) {
        return nullptr;
    }

    int32_t abs_x = parent_abs_x + plane->x;
    int32_t abs_y = parent_abs_y + plane->y;

    n00b_entry_info_t info;
    plane_entry_info_at(plane, abs_x, abs_y, cell_px_w, cell_px_h, &info);

    n00b_rect_t outer_rect = {
        .x = info.outer_x,
        .y = info.outer_y,
        .width = (int32_t)info.outer_cols,
        .height = (int32_t)info.outer_rows,
    };

    if (canvas_uses_cell_snapped_bounds(plane->canvas)) {
        n00b_composite_snap_rect_to_cells(&outer_rect, cell_px_w, cell_px_h);
    }

    if (x < outer_rect.x
        || x >= outer_rect.x + outer_rect.width
        || y < outer_rect.y
        || y >= outer_rect.y + outer_rect.height) {
        return nullptr;
    }

    if (plane->children.data) {
        for (size_t i = plane->children.len; i > 0; i--) {
            n00b_plane_t *child = plane->children.data[i - 1];
            n00b_plane_t *hit = mouse_hit_test_absolute(child,
                                                        abs_x,
                                                        abs_y,
                                                        x,
                                                        y,
                                                        cell_px_w,
                                                        cell_px_h);
            if (hit) {
                return hit;
            }
        }
    }

    return plane;
}

n00b_plane_t *
n00b_mouse_hit_test(n00b_plane_t *plane, int32_t x, int32_t y,
                     int32_t cell_px_w, int32_t cell_px_h)
{
    return mouse_hit_test_absolute(plane, 0, 0, x, y, cell_px_w, cell_px_h);
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
        // Hit-test top-level planes in reverse order (topmost first).
        // Mouse coords and plane positions are both in pixels.
        if (canvas->planes.data) {
            for (size_t i = canvas->planes.len; i > 0; i--) {
                n00b_plane_t *p = canvas->planes.data[i - 1];
                target = n00b_mouse_hit_test(p, event->mouse.x,
                                              event->mouse.y,
                                              cpw, cph);
                if (target) {
                    break;
                }
            }
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

    // Translate mouse coordinates to content-local pixel space for the
    // target widget.  Walk up the parent chain to compute the absolute
    // plane position, then derive the content origin from compositor
    // metadata so local coordinates stay in true pixel space.
    int32_t abs_x = 0, abs_y = 0;
    for (n00b_plane_t *p = target; p; p = p->parent) {
        abs_x += p->x;
        abs_y += p->y;
    }

    n00b_entry_info_t info;
    plane_entry_info_at(target, abs_x, abs_y, cpw, cph, &info);

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
