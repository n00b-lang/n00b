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

static inline int32_t
floor_div_i32(int32_t v, int32_t d)
{
    if (d <= 0) {
        return v;
    }
    if (v >= 0) {
        return v / d;
    }
    return -(((-v) + d - 1) / d);
}

static inline int32_t
align_down_i32(int32_t v, int32_t step)
{
    return floor_div_i32(v, step) * step;
}

static inline bool
plane_has_layout_bounds(const n00b_plane_t *plane)
{
    return plane && plane->bounds.width > 0 && plane->bounds.height > 0;
}

static inline void
resolve_plane_origin(const n00b_plane_t *plane,
                     int32_t             parent_abs_x,
                     int32_t             parent_abs_y,
                     int32_t            *out_x,
                     int32_t            *out_y)
{
    if (plane_has_layout_bounds(plane)) {
        *out_x = plane->bounds.x;
        *out_y = plane->bounds.y;
        return;
    }

    *out_x = parent_abs_x + plane->x;
    *out_y = parent_abs_y + plane->y;
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

static void
mouse_event_to_plane_local(n00b_canvas_t      *canvas,
                           n00b_plane_t       *plane,
                           const n00b_event_t *absolute,
                           n00b_event_t       *localized)
{
    int32_t abs_x = 0;
    int32_t abs_y = 0;
    int32_t cpw = 1;
    int32_t cph = 1;

    if (!absolute || !localized) {
        return;
    }

    *localized = *absolute;

    if (!canvas || !plane || absolute->type != N00B_EVENT_MOUSE) {
        return;
    }

    cpw = (int32_t)(canvas->cell_px_w > 0 ? canvas->cell_px_w : 1);
    cph = (int32_t)(canvas->cell_px_h > 0 ? canvas->cell_px_h : 1);

    plane_absolute_origin(plane, &abs_x, &abs_y);

    if (plane->box) {
        int32_t it, ib, il, ir;
        n00b_box_insets_px(plane->box, cpw, cph, &it, &ib, &il, &ir);
        abs_x += il;
        abs_y += it;
    }

    if (canvas->caps & N00B_RCAP_MANAGES_TTY) {
        abs_x = align_down_i32(abs_x, cpw);
        abs_y = align_down_i32(abs_y, cph);
    }

    localized->mouse.x = absolute->mouse.x - abs_x;
    localized->mouse.y = absolute->mouse.y - abs_y;
}

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
    int32_t w = (int32_t)plane->width;  // Content size in pixels.
    int32_t h = (int32_t)plane->height;

    // When layout has run, bounds track the assigned outer size.
    // Prefer them for hit-testing so border/padding footprint stays exact.
    if (plane->bounds.width > 0 && plane->bounds.height > 0) {
        w = plane->bounds.width;
        h = plane->bounds.height;
    }
    else if (plane->box) {
        int32_t it, ib, il, ir;
        n00b_box_insets_px(plane->box, cell_px_w, cell_px_h,
                            &it, &ib, &il, &ir);
        w += il + ir;
        h += it + ib;
    }

    *out_w = w;
    *out_h = h;
}

static n00b_plane_t *
mouse_hit_test_recurse(n00b_plane_t *plane,
                       int32_t       x,
                       int32_t       y,
                       int32_t       parent_abs_x,
                       int32_t       parent_abs_y,
                       int32_t       cell_px_w,
                       int32_t       cell_px_h)
{
    if (!plane) {
        return nullptr;
    }
    if (!(plane->flags & N00B_PLANE_VISIBLE)) {
        return nullptr;
    }

    // Check if (x, y) is within this plane's full bounding box
    // (including borders + padding). All coordinates are absolute pixels.
    int32_t px = 0;
    int32_t py = 0;
    int32_t pw = 0;
    int32_t ph = 0;
    resolve_plane_origin(plane, parent_abs_x, parent_abs_y, &px, &py);
    plane_full_pixel_size(plane, cell_px_w, cell_px_h, &pw, &ph);

    // Terminal backends place visuals on a cell grid, while layout/hit
    // coordinates are pixel-space. Quantize bounds the same way rendering
    // does so hit-testing tracks what users actually see.
    bool tty_quantized = plane->canvas
                      && (plane->canvas->caps & N00B_RCAP_MANAGES_TTY);
    if (tty_quantized) {
        int32_t cpw = cell_px_w > 0 ? cell_px_w : 1;
        int32_t cph = cell_px_h > 0 ? cell_px_h : 1;
        px = align_down_i32(px, cpw);
        py = align_down_i32(py, cph);
        pw = ((pw + cpw - 1) / cpw) * cpw;
        ph = ((ph + cph - 1) / cph) * cph;
    }

    if (x < px || x >= px + pw || y < py || y >= py + ph) {
        return nullptr;
    }

    // Check children in reverse order (topmost first).
    if (plane->children.data) {
        for (size_t i = plane->children.len; i > 0; i--) {
            n00b_plane_t *child = plane->children.data[i - 1];
            n00b_plane_t *hit   = mouse_hit_test_recurse(child,
                                                         x,
                                                         y,
                                                         px,
                                                         py,
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
    return mouse_hit_test_recurse(plane, x, y, 0, 0, cell_px_w, cell_px_h);
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

n00b_plane_t *
n00b_canvas_get_mouse_capture(n00b_canvas_t *c)
{
    return c ? c->mouse_capture : nullptr;
}
