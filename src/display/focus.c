/*
 * Focus manager: tracks focusable planes in depth-first tab order.
 */

#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "display/focus.h"
#include "display/widget.h"
#include "display/render/plane.h"
#include "display/render/canvas.h"
#include "display/render/types.h"

// -------------------------------------------------------------------
// Internal: depth-first walk to collect focusable planes
// -------------------------------------------------------------------

static void
collect_focusable(n00b_focus_mgr_t *fm, n00b_plane_t *plane)
{
    if (!plane || !(plane->flags & N00B_PLANE_VISIBLE)) {
        return;
    }

    if (n00b_widget_can_focus(plane)) {
        if (fm->count >= fm->capacity) {
            n00b_isize_t new_cap = fm->capacity ? fm->capacity * 2 : 16;
            n00b_plane_t **new_arr = n00b_alloc_array(n00b_plane_t *, new_cap);
            if (fm->focusable) {
                memcpy(new_arr, fm->focusable,
                       (size_t)fm->count * sizeof(n00b_plane_t *));
                n00b_free(fm->focusable);
            }
            fm->focusable = new_arr;
            fm->capacity  = new_cap;
        }
        fm->focusable[fm->count++] = plane;
    }

    // Recurse into children.
    if (plane->children.data) {
        for (size_t i = 0; i < plane->children.len; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (child) {
                collect_focusable(fm, child);
            }
        }
    }
}

// -------------------------------------------------------------------
// Focus transitions
// -------------------------------------------------------------------

static void
blur_current(n00b_focus_mgr_t *fm)
{
    if (fm->current < fm->count) {
        n00b_plane_t *old = fm->focusable[fm->current];
        n00b_plane_set_state(old, N00B_WSTATE_NORMAL);
        n00b_plane_mark_dirty(old);
    }
}

static void
focus_current(n00b_focus_mgr_t *fm)
{
    if (fm->current < fm->count) {
        n00b_plane_t *cur = fm->focusable[fm->current];
        n00b_plane_set_state(cur, N00B_WSTATE_FOCUSED);
        n00b_plane_mark_dirty(cur);
    }
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_focus_mgr_t *
n00b_focus_mgr_new(n00b_canvas_t *canvas)
{
    n00b_focus_mgr_t *fm = n00b_alloc(n00b_focus_mgr_t);
    *fm = (n00b_focus_mgr_t){
        .canvas = canvas,
    };

    if (canvas) {
        canvas->focus = fm;
    }

    n00b_focus_mgr_rebuild(fm);

    return fm;
}

void
n00b_focus_mgr_destroy(n00b_focus_mgr_t *fm)
{
    if (!fm) {
        return;
    }
    if (fm->canvas && fm->canvas->focus == fm) {
        fm->canvas->focus = nullptr;
    }
    if (fm->focusable) {
        n00b_free(fm->focusable);
    }
    n00b_free(fm);
}

void
n00b_focus_mgr_rebuild(n00b_focus_mgr_t *fm)
{
    if (!fm) {
        return;
    }

    n00b_plane_t *old_focused = nullptr;
    if (fm->current < fm->count) {
        old_focused = fm->focusable[fm->current];
    }

    fm->count = 0;

    if (fm->canvas && fm->canvas->planes.data) {
        for (size_t i = 0; i < fm->canvas->planes.len; i++) {
            n00b_plane_t *p = fm->canvas->planes.data[i];
            if (p) {
                collect_focusable(fm, p);
            }
        }
    }

    fm->current = fm->count;
    if (old_focused) {
        for (n00b_isize_t i = 0; i < fm->count; i++) {
            if (fm->focusable[i] == old_focused) {
                fm->current = i;
                break;
            }
        }
    }

    if (fm->current >= fm->count) {
        if (old_focused) {
            n00b_plane_set_state(old_focused, N00B_WSTATE_NORMAL);
            n00b_plane_mark_dirty(old_focused);
        }
    }

    if (fm->current >= fm->count && fm->count > 0) {
        fm->current = 0;
        focus_current(fm);
    }
}

n00b_option_t(n00b_plane_t *)
n00b_focus_mgr_next(n00b_focus_mgr_t *fm)
{
    if (fm->count == 0) {
        return n00b_option_none(n00b_plane_t *);
    }

    blur_current(fm);

    fm->current++;
    if (fm->current >= fm->count) {
        fm->current = 0; // Wrap around.
    }

    focus_current(fm);
    return n00b_option_set(n00b_plane_t *, fm->focusable[fm->current]);
}

n00b_plane_t *
n00b_focus_mgr_next_plane(n00b_focus_mgr_t *fm)
{
    if (!fm) {
        return nullptr;
    }
    return n00b_option_get_or_else(n00b_focus_mgr_next(fm), nullptr);
}

n00b_option_t(n00b_plane_t *)
n00b_focus_mgr_prev(n00b_focus_mgr_t *fm)
{
    if (fm->count == 0) {
        return n00b_option_none(n00b_plane_t *);
    }

    blur_current(fm);

    if (fm->current == 0) {
        fm->current = fm->count - 1; // Wrap around.
    }
    else {
        fm->current--;
    }

    focus_current(fm);
    return n00b_option_set(n00b_plane_t *, fm->focusable[fm->current]);
}

n00b_plane_t *
n00b_focus_mgr_prev_plane(n00b_focus_mgr_t *fm)
{
    if (!fm) {
        return nullptr;
    }
    return n00b_option_get_or_else(n00b_focus_mgr_prev(fm), nullptr);
}

bool
n00b_focus_mgr_set(n00b_focus_mgr_t *fm, n00b_plane_t *plane)
{
    for (n00b_isize_t i = 0; i < fm->count; i++) {
        if (fm->focusable[i] == plane) {
            blur_current(fm);
            fm->current = i;
            focus_current(fm);
            return true;
        }
    }
    return false;
}

n00b_option_t(n00b_plane_t *)
n00b_focus_mgr_current(n00b_focus_mgr_t *fm)
{
    return n00b_option_from_nullable(n00b_plane_t *,
                                     n00b_focus_mgr_current_plane(fm));
}

n00b_plane_t *
n00b_focus_mgr_current_plane(n00b_focus_mgr_t *fm)
{
    if (fm && fm->current < fm->count) {
        return fm->focusable[fm->current];
    }
    return nullptr;
}
