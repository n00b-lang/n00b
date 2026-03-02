/*
 * Box widget: flex container for laying out child planes.
 *
 * Ported from ~/slop/src/ctui/layout/flex.c and
 * ~/slop/src/ctui/widgets/box.c, adapted to the n00b plane/widget
 * model with pixel-based layout.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/widgets/box.h"
#include "display/render/plane.h"
#include "display/render/canvas.h"
#include "display/widget.h"

// -------------------------------------------------------------------
// Flex layout algorithm (4-phase, all in pixels)
// -------------------------------------------------------------------

void
n00b_flex_layout(n00b_plane_t          *container,
                  n00b_rect_t            bounds,
                  n00b_flex_container_t *flex)
{
    if (!container || !flex) {
        return;
    }
    if (!container->children.data) {
        return;
    }

    size_t n = container->children.len;
    if (n == 0) {
        return;
    }

    bool is_row = (flex->direction == N00B_FLEX_ROW);

    int32_t gap_px = flex->gap;  // gap is now in pixels directly

    int32_t available_main  = is_row ? bounds.width : bounds.height;
    int32_t available_cross = is_row ? bounds.height : bounds.width;

    // Account for gaps between children.
    int32_t total_gap = (int32_t)(n - 1) * gap_px;
    available_main -= total_gap;
    if (available_main < 0) {
        available_main = 0;
    }

    // Phase 1: Measure preferred sizes and calculate flex totals.
    int32_t main_sizes_buf[64];
    int32_t cross_sizes_buf[64];
    int32_t *main_sizes  = n <= 64 ? main_sizes_buf  : n00b_alloc_array(int32_t, n);
    int32_t *cross_sizes = n <= 64 ? cross_sizes_buf : n00b_alloc_array(int32_t, n);

    int32_t total_basis = 0;
    float   total_grow  = 0.0f;
    float   total_shrink = 0.0f;

    for (size_t i = 0; i < n; i++) {
        n00b_plane_t *child = container->children.data[i];
        if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
            main_sizes[i]  = 0;
            cross_sizes[i] = 0;
            continue;
        }

        // Use flex.basis if set, otherwise measure preferred size.
        int32_t basis = child->flex.basis;
        if (basis <= 0) {
            int32_t pref_w = 0, pref_h = 0, min_w = 0, min_h = 0;
            n00b_widget_measure(child, &pref_w, &pref_h, &min_w, &min_h);
            basis = is_row ? pref_w : pref_h;
            cross_sizes[i] = is_row ? pref_h : pref_w;
        }
        else {
            // When basis is set, still need cross size.
            int32_t pref_w = 0, pref_h = 0, min_w = 0, min_h = 0;
            n00b_widget_measure(child, &pref_w, &pref_h, &min_w, &min_h);
            cross_sizes[i] = is_row ? pref_h : pref_w;
        }

        main_sizes[i] = basis;
        total_basis   += basis;
        total_grow    += child->flex.grow;
        total_shrink  += child->flex.shrink;
    }

    // Phase 2: Distribute remaining space.
    int32_t remaining = available_main - total_basis;

    if (remaining > 0 && total_grow > 0) {
        for (size_t i = 0; i < n; i++) {
            n00b_plane_t *child = container->children.data[i];
            if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
                continue;
            }
            if (child->flex.grow > 0) {
                int32_t grow = (int32_t)((float)remaining * child->flex.grow
                                         / total_grow);
                main_sizes[i] += grow;
            }
        }
    }
    else if (remaining < 0 && total_shrink > 0) {
        int32_t overflow = -remaining;
        for (size_t i = 0; i < n; i++) {
            n00b_plane_t *child = container->children.data[i];
            if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
                continue;
            }
            if (child->flex.shrink > 0) {
                int32_t shrink = (int32_t)((float)overflow * child->flex.shrink
                                            / total_shrink);
                main_sizes[i] -= shrink;
                if (main_sizes[i] < 0) main_sizes[i] = 0;
            }
        }
    }

    // Phase 3: Compute starting position based on justify.
    int32_t used_main = 0;
    size_t  visible_count = 0;
    for (size_t i = 0; i < n; i++) {
        n00b_plane_t *child = container->children.data[i];
        if (child && (child->flags & N00B_PLANE_VISIBLE)) {
            used_main += main_sizes[i];
            visible_count++;
        }
    }
    used_main += total_gap;

    int32_t free_space = (is_row ? bounds.width : bounds.height) - used_main;
    if (free_space < 0) free_space = 0;

    int32_t pos_main = is_row ? bounds.x : bounds.y;
    int32_t gap_extra = 0;

    switch (flex->justify) {
    case N00B_JUSTIFY_END:
        pos_main += free_space;
        break;
    case N00B_JUSTIFY_CENTER:
        pos_main += free_space / 2;
        break;
    case N00B_JUSTIFY_SPACE_BETWEEN:
        if (visible_count > 1) {
            gap_extra = free_space / (int32_t)(visible_count - 1);
        }
        break;
    case N00B_JUSTIFY_SPACE_AROUND:
        if (visible_count > 0) {
            int32_t margin = free_space / (int32_t)(visible_count * 2);
            pos_main += margin;
            gap_extra = margin * 2;
        }
        break;
    case N00B_JUSTIFY_SPACE_EVENLY:
        if (visible_count > 0) {
            int32_t space = free_space / (int32_t)(visible_count + 1);
            pos_main += space;
            gap_extra = space;
        }
        break;
    default: // N00B_JUSTIFY_START
        break;
    }

    // Phase 4: Position and size children.
    for (size_t i = 0; i < n; i++) {
        n00b_plane_t *child = container->children.data[i];
        if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
            continue;
        }

        // Cross-axis alignment.
        n00b_align_items_t align = child->flex.align_self;
        if (align == N00B_ALIGN_AUTO_CROSS) {
            align = flex->align;
        }

        int32_t size_cross;
        int32_t pos_cross;

        switch (align) {
        case N00B_ALIGN_STRETCH_CROSS:
            size_cross = available_cross;
            pos_cross  = is_row ? bounds.y : bounds.x;
            break;
        case N00B_ALIGN_END_CROSS:
            size_cross = cross_sizes[i];
            pos_cross  = (is_row ? bounds.y : bounds.x)
                       + available_cross - size_cross;
            break;
        case N00B_ALIGN_CENTER_CROSS:
            size_cross = cross_sizes[i];
            pos_cross  = (is_row ? bounds.y : bounds.x)
                       + (available_cross - size_cross) / 2;
            break;
        default: // N00B_ALIGN_START_CROSS
            size_cross = cross_sizes[i];
            pos_cross  = is_row ? bounds.y : bounds.x;
            break;
        }

        n00b_rect_t child_bounds;
        if (is_row) {
            child_bounds = (n00b_rect_t){
                .x      = pos_main,
                .y      = pos_cross,
                .width  = main_sizes[i],
                .height = size_cross,
            };
        }
        else {
            child_bounds = (n00b_rect_t){
                .x      = pos_cross,
                .y      = pos_main,
                .width  = size_cross,
                .height = main_sizes[i],
            };
        }

        n00b_widget_layout(child, child_bounds);

        pos_main += main_sizes[i] + gap_px + gap_extra;
    }

    if (n > 64) {
        if (main_sizes != main_sizes_buf) n00b_free(main_sizes);
        if (cross_sizes != cross_sizes_buf) n00b_free(cross_sizes);
    }
}

// -------------------------------------------------------------------
// Box widget vtable callbacks
// -------------------------------------------------------------------

static void
box_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
box_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    // Container has no visible content of its own — just clear.
    n00b_plane_clear(plane);
}

static void
box_measure(n00b_plane_t *plane, void *data,
            int32_t *pref_w, int32_t *pref_h,
            int32_t *min_w,  int32_t *min_h)
{
    n00b_box_data_t *box = data;
    if (!box) {
        *pref_w = (int32_t)plane->width;
        *pref_h = (int32_t)plane->height;
        *min_w  = 1;
        *min_h  = 1;
        return;
    }

    bool is_row = (box->flex.direction == N00B_FLEX_ROW);
    int32_t main_size  = 0;
    int32_t cross_size = 0;

    if (plane->children.data) {
        size_t n = plane->children.len;
        for (size_t i = 0; i < n; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
                continue;
            }
            int32_t pw = 0, ph = 0, mw = 0, mh = 0;
            n00b_widget_measure(child, &pw, &ph, &mw, &mh);
            if (is_row) {
                main_size += pw;
                if (ph > cross_size) cross_size = ph;
            }
            else {
                main_size += ph;
                if (pw > cross_size) cross_size = pw;
            }
        }

        // Add gaps.
        if (n > 1) {
            main_size += (int32_t)(n - 1) * box->flex.gap;
        }
    }

    // Add padding (already in pixels).
    int32_t pad_w = box->padding.left + box->padding.right;
    int32_t pad_h = box->padding.top + box->padding.bottom;

    if (is_row) {
        *pref_w = main_size + pad_w;
        *pref_h = cross_size + pad_h;
    }
    else {
        *pref_w = cross_size + pad_w;
        *pref_h = main_size + pad_h;
    }

    if (*pref_w < 1) *pref_w = 1;
    if (*pref_h < 1) *pref_h = 1;
    *min_w = 1;
    *min_h = 1;
}

static void
box_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    n00b_box_data_t *box = data;
    if (!box) {
        return;
    }

    // Subtract padding from bounds to get content area.
    n00b_rect_t content = bounds;
    content.x      += box->padding.left;
    content.y      += box->padding.top;
    content.width  -= (box->padding.left + box->padding.right);
    content.height -= (box->padding.top + box->padding.bottom);
    if (content.width  < 0) content.width  = 0;
    if (content.height < 0) content.height = 0;

    n00b_flex_layout(plane, content, &box->flex);
}

static bool
box_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return false;
}

// -------------------------------------------------------------------
// Vtable
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_box = {
    .kind      = "box",
    .destroy   = box_destroy,
    .render    = box_render,
    .measure   = box_measure,
    .can_focus = box_can_focus,
    .layout    = box_layout,
};

// -------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------

n00b_plane_t *
n00b_box_new() _kargs {
    n00b_flex_direction_t direction  = N00B_FLEX_COLUMN;
    n00b_justify_t        justify    = N00B_JUSTIFY_START;
    n00b_align_items_t    align      = N00B_ALIGN_STRETCH_CROSS;
    int32_t               gap        = 0;
    int32_t               pad_top    = 0;
    int32_t               pad_right  = 0;
    int32_t               pad_bottom = 0;
    int32_t               pad_left   = 0;
    n00b_canvas_t        *canvas     = nullptr;
    n00b_allocator_t     *allocator  = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .canvas    = canvas,
                                           .allocator = allocator);

    n00b_box_data_t *data = n00b_alloc(n00b_box_data_t);
    data->flex.direction  = direction;
    data->flex.justify    = justify;
    data->flex.align      = align;
    data->flex.gap        = gap;
    data->padding.top     = pad_top;
    data->padding.right   = pad_right;
    data->padding.bottom  = pad_bottom;
    data->padding.left    = pad_left;

    n00b_widget_attach(plane, &n00b_widget_box, data);

    return plane;
}
