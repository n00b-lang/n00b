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
               n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
               n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_spacer_t *spacer = (n00b_spacer_t *)data;

    *pref_cols = spacer ? spacer->min_cols : 1;
    *pref_rows = spacer ? spacer->min_rows : 1;
    *min_cols  = *pref_cols;
    *min_rows  = *pref_rows;
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
    n00b_isize_t      cols      = 1;
    n00b_isize_t      rows      = 1;
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .cols      = cols,
                                           .rows      = rows,
                                           .allocator = allocator);

    n00b_spacer_t *spacer = n00b_alloc(n00b_spacer_t);
    spacer->min_cols = cols;
    spacer->min_rows = rows;

    n00b_widget_attach(plane, &n00b_widget_spacer, spacer);

    return plane;
}
