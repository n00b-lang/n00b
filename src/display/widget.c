/*
 * Widget behavior: attach/detach, render, measure, layout, events.
 */

#include "n00b.h"
#include "display/widget.h"
#include "display/event.h"
#include "display/render/plane.h"
#include "display/render/canvas.h"
#include "display/render/box.h"

// -------------------------------------------------------------------
// Attach / detach
// -------------------------------------------------------------------

void
n00b_widget_attach(n00b_plane_t               *plane,
                    const n00b_widget_vtable_t *vtable,
                    void                       *data)
{
    assert(plane);
    assert(vtable);
    assert(plane->widget_vtable == nullptr);

    plane->widget_vtable = vtable;
    plane->widget_data   = data;
}

void
n00b_widget_detach(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable) {
        return;
    }

    if (plane->widget_vtable->destroy) {
        plane->widget_vtable->destroy(plane, plane->widget_data);
    }

    plane->widget_vtable = nullptr;
    plane->widget_data   = nullptr;
}

// -------------------------------------------------------------------
// Render
// -------------------------------------------------------------------

void
n00b_widget_render(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->render) {
        return;
    }

    plane->widget_vtable->render(plane, plane->widget_data);
    // Clear dirty — the render callback issues draw commands which
    // re-set DIRTY, but we just consumed that work.
    plane->flags &= (uint16_t)~N00B_PLANE_DIRTY;
}

// -------------------------------------------------------------------
// Measure (pixels)
// -------------------------------------------------------------------

void
n00b_widget_measure(n00b_plane_t *plane,
                     int32_t *pref_w, int32_t *pref_h,
                     int32_t *min_w,  int32_t *min_h)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->measure) {
        *pref_w = *pref_h = *min_w = *min_h = 0;
        return;
    }

    // The measure callback reports content size in pixels.
    int32_t pw = 0, ph = 0, mw = 0, mh = 0;
    plane->widget_vtable->measure(plane, plane->widget_data,
                                   &pw, &ph, &mw, &mh);

    // Add box insets (border + padding) to get total widget footprint.
    if (plane->box && plane->canvas) {
        int32_t cpw = (int32_t)plane->canvas->cell_px_w;
        int32_t cph = (int32_t)plane->canvas->cell_px_h;
        int32_t it = 0, ib = 0, il = 0, ir = 0;
        n00b_box_insets_px(plane->box, cpw, cph, &it, &ib, &il, &ir);

        pw += il + ir;
        ph += it + ib;
        mw += il + ir;
        mh += it + ib;
    }

    *pref_w = pw;
    *pref_h = ph;
    *min_w  = mw;
    *min_h  = mh;
}

// -------------------------------------------------------------------
// Event dispatch
// -------------------------------------------------------------------

bool
n00b_widget_handle_event(n00b_plane_t       *plane,
                          const n00b_event_t *event)
{
    if (!plane || !plane->widget_vtable
        || !plane->widget_vtable->handle_event) {
        return false;
    }

    return plane->widget_vtable->handle_event(plane, plane->widget_data,
                                               event);
}

bool
n00b_widget_can_focus(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable
        || !plane->widget_vtable->can_focus) {
        return false;
    }

    return plane->widget_vtable->can_focus(plane, plane->widget_data);
}

// -------------------------------------------------------------------
// Layout
// -------------------------------------------------------------------

void
n00b_widget_layout(n00b_plane_t *plane, n00b_rect_t bounds)
{
    if (!plane) {
        return;
    }

    plane->bounds = bounds;

    // Subtract box insets (borders+padding) to get the viewport
    // (content) area; compositor adds them back during compositing.
    int32_t inset_top = 0, inset_bot = 0, inset_left = 0, inset_right = 0;

    if (plane->box && plane->canvas) {
        int32_t cpw = (int32_t)plane->canvas->cell_px_w;
        int32_t cph = (int32_t)plane->canvas->cell_px_h;
        n00b_box_insets_px(plane->box, cpw, cph,
                            &inset_top, &inset_bot,
                            &inset_left, &inset_right);
    }

    int32_t vp_px_w = bounds.width  - (inset_left + inset_right);
    int32_t vp_px_h = bounds.height - (inset_top  + inset_bot);

    if (vp_px_w < 1) vp_px_w = 1;
    if (vp_px_h < 1) vp_px_h = 1;

    n00b_plane_move(plane, bounds.x, bounds.y);

    // Content area stored in pixels directly.
    plane->width  = vp_px_w;
    plane->height = vp_px_h;
    plane->flags  |= N00B_PLANE_DIRTY;

    // Custom container layout: receives viewport bounds (insets removed)
    // so it distributes space among content only.
    if (plane->widget_vtable && plane->widget_vtable->layout) {
        int32_t vp_px_x = bounds.x + inset_left;
        int32_t vp_px_y = bounds.y + inset_top;
        n00b_rect_t content_bounds = {
            .x      = vp_px_x,
            .y      = vp_px_y,
            .width  = vp_px_w,
            .height = vp_px_h,
        };
        plane->widget_vtable->layout(plane, plane->widget_data,
                                      content_bounds);
    }
}
