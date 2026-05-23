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
#include "display/render/types.h"
#include "display/event.h"
#include "display/widget.h"
#include "display/focus.h"
#include "adt/option.h"

// -------------------------------------------------------------------
// Hit testing
// -------------------------------------------------------------------

/*
 * Compute the full pixel footprint of a plane including box decorations.
 * width/height are already in pixels.  Box insets are cell counts
 * that need scaling by cell_px_w/h.
 */
static void
plane_full_pixel_size(n00b_plane_t *plane,
                       int32_t cell_px_w, int32_t cell_px_h,
                       int32_t *out_w, int32_t *out_h)
{
    int32_t w = (int32_t)plane->width;  // Already pixels.
    int32_t h = (int32_t)plane->height;

    if (plane->box) {
        int32_t it, ib, il, ir;
        n00b_box_insets_px(plane->box, cell_px_w, cell_px_h,
                            &it, &ib, &il, &ir);
        w += il + ir;
        h += it + ib;
    }

    *out_w = w;
    *out_h = h;
}

n00b_option_t(n00b_plane_t *)
n00b_mouse_hit_test(n00b_plane_t *plane, int32_t x, int32_t y,
                     int32_t cell_px_w, int32_t cell_px_h)
{
    if (!plane) {
        return n00b_option_none(n00b_plane_t *);
    }
    if (!(plane->flags & N00B_PLANE_VISIBLE)) {
        return n00b_option_none(n00b_plane_t *);
    }

    // Check if (x, y) is within this plane's full bounding box
    // (including borders + padding).  All coordinates are pixels.
    int32_t px = plane->x;
    int32_t py = plane->y;
    int32_t pw, ph;
    plane_full_pixel_size(plane, cell_px_w, cell_px_h, &pw, &ph);

    if (x < px || x >= px + pw || y < py || y >= py + ph) {
        return n00b_option_none(n00b_plane_t *);
    }

    // Convert to plane-local coordinates for child testing.
    int32_t lx = x - px;
    int32_t ly = y - py;

    // Check children in reverse order (topmost first).
    if (plane->children.data) {
        for (size_t i = plane->children.len; i > 0; i--) {
            n00b_plane_t *child = plane->children.data[i - 1];
            n00b_option_t(n00b_plane_t *) hit_opt =
                n00b_mouse_hit_test(child, lx, ly, cell_px_w, cell_px_h);
            if (n00b_option_is_set(hit_opt)) {
                return hit_opt;
            }
        }
    }

    // No child hit — return this plane.
    return n00b_option_set(n00b_plane_t *, plane);
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
                n00b_option_t(n00b_plane_t *) hit_opt =
                    n00b_mouse_hit_test(p, event->mouse.x,
                                         event->mouse.y,
                                         cpw, cph);
                if (n00b_option_is_set(hit_opt)) {
                    target = n00b_option_get(hit_opt);
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
    // pixel position, then subtract box insets (border + padding in pixels).
    int32_t abs_x = 0, abs_y = 0;
    for (n00b_plane_t *p = target; p; p = p->parent) {
        abs_x += p->x;
        abs_y += p->y;
    }

    // Subtract box insets (scaled to pixels) to get content-area origin.
    if (target->box) {
        int32_t it, ib, il, ir;
        n00b_box_insets_px(target->box, cpw, cph, &it, &ib, &il, &ir);
        abs_x += il;
        abs_y += it;
    }

    n00b_event_t local_event = *event;
    local_event.mouse.x = event->mouse.x - abs_x;
    local_event.mouse.y = event->mouse.y - abs_y;

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

n00b_option_t(n00b_plane_t *)
n00b_canvas_get_mouse_capture(n00b_canvas_t *c)
{
    if (!c) return n00b_option_none(n00b_plane_t *);
    return n00b_option_from_nullable(n00b_plane_t *, c->mouse_capture);
}
