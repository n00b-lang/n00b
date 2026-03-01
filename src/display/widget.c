/*
 * Widget attach/detach/render helpers.
 */

#include "n00b.h"
#include "display/widget.h"
#include "display/event.h"
#include "display/render/plane.h"

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

void
n00b_widget_render(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->render) {
        return;
    }

    plane->widget_vtable->render(plane, plane->widget_data);
    plane->flags |= N00B_PLANE_DIRTY;
}

void
n00b_widget_measure(n00b_plane_t *plane,
                     n00b_isize_t *pref_cols,
                     n00b_isize_t *pref_rows,
                     n00b_isize_t *min_cols,
                     n00b_isize_t *min_rows)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->measure) {
        *pref_cols = 0;
        *pref_rows = 0;
        *min_cols  = 0;
        *min_rows  = 0;
        return;
    }

    plane->widget_vtable->measure(plane, plane->widget_data,
                                   pref_cols, pref_rows,
                                   min_cols, min_rows);
}

bool
n00b_widget_handle_event(n00b_plane_t       *plane,
                          const n00b_event_t *event)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->handle_event) {
        return false;
    }

    return plane->widget_vtable->handle_event(plane, plane->widget_data, event);
}

bool
n00b_widget_can_focus(n00b_plane_t *plane)
{
    if (!plane || !plane->widget_vtable || !plane->widget_vtable->can_focus) {
        return false;
    }

    return plane->widget_vtable->can_focus(plane, plane->widget_data);
}
