/*
 * Spacer widget: invisible empty space for layout.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/spacer.h"

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
spacer_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
spacer_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    // No-op — spacer is empty space.
    n00b_plane_clear(plane);
}

static void
spacer_measure(n00b_plane_t *plane, void *data,
               int32_t *pref_w, int32_t *pref_h,
               int32_t *min_w,  int32_t *min_h)
{
    (void)plane;
    n00b_spacer_t *spacer = (n00b_spacer_t *)data;

    *pref_w = spacer ? spacer->min_cols : 1;
    *pref_h = spacer ? spacer->min_rows : 1;
    *min_w  = *pref_w;
    *min_h  = *pref_h;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_spacer = {
    .kind    = "spacer",
    .destroy = spacer_destroy,
    .render  = spacer_render,
    .measure = spacer_measure,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_spacer_new() _kargs {
    int32_t           width     = 1;
    int32_t           height    = 1;
    n00b_canvas_t    *canvas    = nullptr;
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .canvas    = canvas,
                                           .allocator = allocator);
    plane->width = width;
    plane->height = height;

    n00b_spacer_t *spacer = n00b_alloc(n00b_spacer_t);
    spacer->min_cols = width;
    spacer->min_rows = height;

    n00b_widget_attach(plane, &n00b_widget_spacer, spacer);

    return plane;
}
