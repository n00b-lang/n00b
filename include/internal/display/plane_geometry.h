#pragma once

#include "n00b.h"
#include "display/render/plane.h"

static inline bool
n00b_plane_has_layout_bounds(const n00b_plane_t *plane)
{
    return plane && plane->bounds.width > 0 && plane->bounds.height > 0;
}

static inline void
n00b_plane_resolve_absolute_origin(const n00b_plane_t *plane,
                                   int32_t             parent_x,
                                   int32_t             parent_y,
                                   int32_t            *out_x,
                                   int32_t            *out_y)
{
    if (out_x) {
        *out_x = 0;
    }
    if (out_y) {
        *out_y = 0;
    }

    if (!plane) {
        return;
    }

    if (n00b_plane_has_layout_bounds(plane)) {
        if (out_x) {
            *out_x = plane->bounds.x;
        }
        if (out_y) {
            *out_y = plane->bounds.y;
        }
        return;
    }

    if (out_x) {
        *out_x = parent_x + plane->x;
    }
    if (out_y) {
        *out_y = parent_y + plane->y;
    }
}
