/*
 * Scroll widget: single-child viewport with keyboard, wheel, and thumb scroll.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/event.h"
#include "display/mouse.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/scroll.h"
#include "internal/display/widget_primitives.h"
#include "text/strings/style_ops.h"
#include "text/strings/theme.h"

typedef struct n00b_scroll_impl_t {
    n00b_scroll_t public_state;
    n00b_plane_t *viewport_plane;
} n00b_scroll_impl_t;

static void
scroll_viewport_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
}

static const n00b_widget_vtable_t n00b_widget_scroll_viewport = {
    .kind   = "scroll_viewport",
    .render = scroll_viewport_render,
};

static n00b_scroll_impl_t *
scroll_impl(n00b_plane_t *plane)
{
    return (n00b_scroll_impl_t *)n00b_widget_data_if_kind(plane, &n00b_widget_scroll);
}

static n00b_scroll_t *
scroll_state(n00b_plane_t *plane)
{
    n00b_scroll_impl_t *impl = scroll_impl(plane);
    return impl ? &impl->public_state : nullptr;
}

static void scroll_mark_layout_dirty(n00b_plane_t *plane);

static inline int32_t
scroll_max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static inline int32_t
scroll_min_i32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline int32_t
scroll_clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static inline bool
scroll_has_valid_bounds(const n00b_plane_t *plane)
{
    return plane && plane->bounds.width > 0 && plane->bounds.height > 0;
}

static inline bool
scroll_axis_enabled(const n00b_scroll_t *scroll, n00b_scroll_axis_t axis)
{
    return scroll && (scroll->axes & axis) != 0;
}

static inline bool
scroll_rect_is_empty(n00b_rect_t rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

static bool
scroll_point_in_rect(n00b_rect_t rect, int32_t x, int32_t y)
{
    return !scroll_rect_is_empty(rect)
        && x >= rect.x
        && y >= rect.y
        && x < rect.x + rect.width
        && y < rect.y + rect.height;
}

static int32_t
scroll_max_offset_x(const n00b_scroll_t *scroll)
{
    if (!scroll || !scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
        return 0;
    }

    return scroll_max_i32(scroll->content_width - scroll->viewport_width, 0);
}

static int32_t
scroll_max_offset_y(const n00b_scroll_t *scroll)
{
    if (!scroll || !scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
        return 0;
    }

    return scroll_max_i32(scroll->content_height - scroll->viewport_height, 0);
}

static void
scroll_clear_rects(n00b_scroll_t *scroll)
{
    if (!scroll) {
        return;
    }

    scroll->vscrollbar_rect = (n00b_rect_t){};
    scroll->hscrollbar_rect = (n00b_rect_t){};
    scroll->vthumb_rect     = (n00b_rect_t){};
    scroll->hthumb_rect     = (n00b_rect_t){};
}

static void
scroll_measure_content(n00b_plane_t *content,
                       int32_t      *pref_w,
                       int32_t      *pref_h,
                       int32_t      *min_w,
                       int32_t      *min_h)
{
    if (!content) {
        *pref_w = *pref_h = *min_w = *min_h = 0;
        return;
    }

    if (content->widget_vtable) {
        n00b_widget_measure(content, pref_w, pref_h, min_w, min_h);
        return;
    }

    n00b_widget_measure_plain_plane(content, pref_w, pref_h, min_w, min_h);
}

static void
scroll_measure_content_for_width(n00b_plane_t *content,
                                 int32_t       measure_width,
                                 int32_t      *pref_w,
                                 int32_t      *pref_h,
                                 int32_t      *min_w,
                                 int32_t      *min_h)
{
    int32_t     saved_width;
    int32_t     saved_height;
    n00b_rect_t saved_bounds;

    if (!content || measure_width <= 0 || !content->widget_vtable) {
        scroll_measure_content(content, pref_w, pref_h, min_w, min_h);
        return;
    }

    saved_width  = content->width;
    saved_height = content->height;
    saved_bounds = content->bounds;

    content->width        = measure_width;
    content->bounds.width = measure_width;
    if (content->bounds.height <= 0) {
        content->bounds.height = saved_bounds.height > 0 ? saved_bounds.height
                                                         : scroll_max_i32(saved_height, 1);
    }

    n00b_widget_measure(content, pref_w, pref_h, min_w, min_h);

    content->width  = saved_width;
    content->height = saved_height;
    content->bounds = saved_bounds;
}

static void
scroll_release_capture_if_owned(n00b_plane_t *plane)
{
    if (plane && plane->canvas
        && n00b_canvas_get_mouse_capture_plane(plane->canvas) == plane) {
        n00b_canvas_release_mouse(plane->canvas);
    }
}

static void
scroll_stop_thumb_drag(n00b_plane_t *plane, n00b_scroll_t *scroll)
{
    if (!scroll) {
        return;
    }

    if (scroll->dragging_vertical_thumb || scroll->dragging_horizontal_thumb) {
        scroll_release_capture_if_owned(plane);
    }

    scroll->dragging_vertical_thumb   = false;
    scroll->dragging_horizontal_thumb = false;
    scroll->drag_anchor_px            = 0;
    scroll->drag_anchor_offset_px     = 0;
}

static void
scroll_cancel_mouse_capture(n00b_plane_t *plane, void *data)
{
    n00b_scroll_t *scroll = data;

    if (!plane || !scroll) {
        return;
    }

    scroll->dragging_vertical_thumb   = false;
    scroll->dragging_horizontal_thumb = false;
    scroll->hover_vertical_thumb      = false;
    scroll->hover_horizontal_thumb    = false;
    scroll->drag_anchor_px            = 0;
    scroll->drag_anchor_offset_px     = 0;
    scroll_mark_layout_dirty(plane);
}

static void
scroll_mark_layout_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    n00b_plane_mark_dirty(plane);
}

static void
scroll_refresh_styles(n00b_scroll_t *scroll)
{
    if (!scroll) {
        return;
    }

    *scroll->track_style = (n00b_text_style_t){
        .fg_palette_ix = N00B_PAL_UNSET,
        .bg_palette_ix = N00B_PAL_UNSET,
        .bg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_TRACK),
        .fg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_TRACK),
    };
    *scroll->thumb_style = (n00b_text_style_t){
        .fg_palette_ix = N00B_PAL_UNSET,
        .bg_palette_ix = N00B_PAL_UNSET,
        .bg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_THUMB),
        .fg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_THUMB),
    };
    *scroll->thumb_hover_style = (n00b_text_style_t){
        .fg_palette_ix = N00B_PAL_UNSET,
        .bg_palette_ix = N00B_PAL_UNSET,
        .bg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_THUMB_HOVER),
        .fg_rgb = n00b_theme_resolve_color(N00B_PAL_SCROLLBAR_THUMB_HOVER),
    };
}

static void
scroll_update_viewport_plane(n00b_scroll_impl_t *impl, n00b_rect_t bounds)
{
    if (!impl || !impl->viewport_plane) {
        return;
    }

    impl->viewport_plane->bounds = bounds;
    impl->viewport_plane->x      = bounds.x;
    impl->viewport_plane->y      = bounds.y;
    impl->viewport_plane->width  = bounds.width;
    impl->viewport_plane->height = bounds.height;
}

static void
scroll_clamp_offsets(n00b_scroll_t *scroll)
{
    int32_t max_x;
    int32_t max_y;

    if (!scroll) {
        return;
    }

    max_x = scroll_max_offset_x(scroll);
    max_y = scroll_max_offset_y(scroll);

    scroll->offset_x = scroll_clamp_i32(scroll->offset_x, 0, max_x);
    scroll->offset_y = scroll_clamp_i32(scroll->offset_y, 0, max_y);
}

static void
scroll_update_hover_state(n00b_plane_t *plane,
                          n00b_scroll_t *scroll,
                          int32_t        local_x,
                          int32_t        local_y)
{
    bool hover_v;
    bool hover_h;

    if (!scroll) {
        return;
    }

    hover_v = scroll_point_in_rect(scroll->vthumb_rect, local_x, local_y);
    hover_h = scroll_point_in_rect(scroll->hthumb_rect, local_x, local_y);

    if (scroll->dragging_vertical_thumb) {
        hover_v = true;
    }
    if (scroll->dragging_horizontal_thumb) {
        hover_h = true;
    }

    if (scroll->hover_vertical_thumb == hover_v
        && scroll->hover_horizontal_thumb == hover_h) {
        return;
    }

    scroll->hover_vertical_thumb   = hover_v;
    scroll->hover_horizontal_thumb = hover_h;
    scroll_mark_layout_dirty(plane);
}

static void
scroll_cache_scrollbar_geometry(n00b_plane_t *plane, n00b_scroll_t *scroll)
{
    int32_t thickness;
    int32_t max_offset_x;
    int32_t max_offset_y;
    int32_t thumb_h;
    int32_t thumb_w;
    int32_t max_thumb_y;
    int32_t max_thumb_x;
    int32_t thumb_y;
    int32_t thumb_x;

    (void)plane;

    scroll_clear_rects(scroll);

    if (!scroll) {
        return;
    }

    thickness = scroll->scrollbar_thickness_px;

    if (scroll->show_vscrollbar) {
        scroll->vscrollbar_rect = (n00b_rect_t){
            .x      = scroll->viewport_width,
            .y      = 0,
            .width  = thickness,
            .height = scroll->viewport_height,
        };
    }

    if (scroll->show_hscrollbar) {
        scroll->hscrollbar_rect = (n00b_rect_t){
            .x      = 0,
            .y      = scroll->viewport_height,
            .width  = scroll->viewport_width,
            .height = thickness,
        };
    }

    max_offset_y = scroll_max_offset_y(scroll);
    if (scroll->show_vscrollbar && max_offset_y > 0 && scroll->content_height > 0) {
        thumb_h = scroll_max_i32(thickness,
                                 (scroll->viewport_height * scroll->viewport_height)
                                 / scroll->content_height);
        thumb_h = scroll_min_i32(thumb_h, scroll->viewport_height);
        max_thumb_y = scroll_max_i32(scroll->viewport_height - thumb_h, 0);
        thumb_y = max_offset_y > 0
                ? (scroll->offset_y * max_thumb_y) / max_offset_y
                : 0;
        thumb_y = scroll_clamp_i32(thumb_y, 0, max_thumb_y);

        scroll->vthumb_rect = (n00b_rect_t){
            .x      = scroll->vscrollbar_rect.x,
            .y      = thumb_y,
            .width  = thickness,
            .height = thumb_h,
        };
    }
    else {
        scroll->hover_vertical_thumb = false;
    }

    max_offset_x = scroll_max_offset_x(scroll);
    if (scroll->show_hscrollbar && max_offset_x > 0 && scroll->content_width > 0) {
        thumb_w = scroll_max_i32(thickness,
                                 (scroll->viewport_width * scroll->viewport_width)
                                 / scroll->content_width);
        thumb_w = scroll_min_i32(thumb_w, scroll->viewport_width);
        max_thumb_x = scroll_max_i32(scroll->viewport_width - thumb_w, 0);
        thumb_x = max_offset_x > 0
                ? (scroll->offset_x * max_thumb_x) / max_offset_x
                : 0;
        thumb_x = scroll_clamp_i32(thumb_x, 0, max_thumb_x);

        scroll->hthumb_rect = (n00b_rect_t){
            .x      = thumb_x,
            .y      = scroll->hscrollbar_rect.y,
            .width  = thumb_w,
            .height = thickness,
        };
    }
    else {
        scroll->hover_horizontal_thumb = false;
    }
}

static void
scroll_resolve_viewport(n00b_scroll_t *scroll,
                        int32_t        bounds_width,
                        int32_t        bounds_height)
{
    bool show_v = false;
    bool show_h = false;
    int  passes = 0;

    if (!scroll) {
        return;
    }

    do {
        bool prev_v = show_v;
        bool prev_h = show_h;
        int32_t viewport_width = bounds_width;
        int32_t viewport_height = bounds_height;

        if (show_v) {
            viewport_width -= scroll->scrollbar_thickness_px;
        }
        if (show_h) {
            viewport_height -= scroll->scrollbar_thickness_px;
        }

        viewport_width = scroll_max_i32(viewport_width, 1);
        viewport_height = scroll_max_i32(viewport_height, 1);

        if (!scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)
            || scroll->scrollbar_mode == N00B_SCROLLBAR_NEVER) {
            show_v = false;
        }
        else if (scroll->scrollbar_mode == N00B_SCROLLBAR_ALWAYS) {
            show_v = true;
        }
        else {
            show_v = scroll->content_height > viewport_height;
        }

        if (!scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)
            || scroll->scrollbar_mode == N00B_SCROLLBAR_NEVER) {
            show_h = false;
        }
        else if (scroll->scrollbar_mode == N00B_SCROLLBAR_ALWAYS) {
            show_h = true;
        }
        else {
            show_h = scroll->content_width > viewport_width;
        }

        passes++;
        if (show_v == prev_v && show_h == prev_h) {
            break;
        }
    } while (passes < 2);

    scroll->show_vscrollbar = show_v;
    scroll->show_hscrollbar = show_h;
    scroll->viewport_width  = bounds_width - (show_v ? scroll->scrollbar_thickness_px : 0);
    scroll->viewport_height = bounds_height - (show_h ? scroll->scrollbar_thickness_px : 0);
    scroll->viewport_width  = scroll_max_i32(scroll->viewport_width, 1);
    scroll->viewport_height = scroll_max_i32(scroll->viewport_height, 1);
}

static void
scroll_layout_content(n00b_plane_t *plane, n00b_scroll_impl_t *impl, n00b_rect_t bounds)
{
    n00b_scroll_t *scroll;
    int32_t content_pref_w;
    int32_t content_pref_h;
    int32_t content_min_w;
    int32_t content_min_h;
    n00b_rect_t viewport_bounds;
    n00b_rect_t child_bounds;
    bool        constrain_width;
    int32_t     measure_width;

    if (!plane || !impl) {
        return;
    }

    scroll = &impl->public_state;
    constrain_width = scroll->content
                   && !scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL);
    measure_width   = constrain_width ? scroll_max_i32(bounds.width, 1) : 0;

    for (int pass = 0; pass < 3; pass++) {
        content_pref_w = 1;
        content_pref_h = 1;
        content_min_w  = 1;
        content_min_h  = 1;

        if (scroll->content) {
            if (constrain_width) {
                scroll_measure_content_for_width(scroll->content,
                                                 measure_width,
                                                 &content_pref_w,
                                                 &content_pref_h,
                                                 &content_min_w,
                                                 &content_min_h);
            }
            else {
                scroll_measure_content(scroll->content,
                                       &content_pref_w,
                                       &content_pref_h,
                                       &content_min_w,
                                       &content_min_h);
            }
        }

        content_pref_w = scroll_max_i32(content_pref_w, 1);
        content_pref_h = scroll_max_i32(content_pref_h, 1);
        content_min_w  = scroll_max_i32(content_min_w, 1);
        content_min_h  = scroll_max_i32(content_min_h, 1);
        (void)content_min_w;
        (void)content_min_h;

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
            scroll->content_width = scroll_max_i32(content_pref_w, bounds.width);
        }
        else {
            scroll->content_width = scroll_max_i32(measure_width, 1);
        }

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
            scroll->content_height = scroll_max_i32(content_pref_h, bounds.height);
        }
        else {
            scroll->content_height = bounds.height;
        }

        scroll_resolve_viewport(scroll, bounds.width, bounds.height);

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
            scroll->content_width = scroll_max_i32(scroll->content_width,
                                                   scroll->viewport_width);
        }
        else {
            scroll->content_width = scroll->viewport_width;
        }

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
            scroll->content_height = scroll_max_i32(scroll->content_height,
                                                    scroll->viewport_height);
        }
        else {
            scroll->content_height = scroll->viewport_height;
        }

        if (!constrain_width || scroll->viewport_width == measure_width) {
            break;
        }

        measure_width = scroll->viewport_width;
    }

    scroll_clamp_offsets(scroll);

    viewport_bounds = (n00b_rect_t){
        .x      = bounds.x,
        .y      = bounds.y,
        .width  = scroll->viewport_width,
        .height = scroll->viewport_height,
    };
    scroll_update_viewport_plane(impl, viewport_bounds);

    if (impl->viewport_plane) {
        n00b_widget_layout(impl->viewport_plane, viewport_bounds);
    }

    if (scroll->content) {
        child_bounds = (n00b_rect_t){
            .x      = viewport_bounds.x - scroll->offset_x,
            .y      = viewport_bounds.y - scroll->offset_y,
            .width  = scroll->content_width,
            .height = scroll->content_height,
        };
        n00b_widget_layout(scroll->content, child_bounds);
    }

    scroll_cache_scrollbar_geometry(plane, scroll);

    if (scroll_rect_is_empty(scroll->vthumb_rect)) {
        if (scroll->dragging_vertical_thumb) {
            scroll_stop_thumb_drag(plane, scroll);
        }
        scroll->hover_vertical_thumb = false;
    }

    if (scroll_rect_is_empty(scroll->hthumb_rect)) {
        if (scroll->dragging_horizontal_thumb) {
            scroll_stop_thumb_drag(plane, scroll);
        }
        scroll->hover_horizontal_thumb = false;
    }
}

static void
scroll_relayout_if_possible(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    if (scroll_has_valid_bounds(plane)) {
        n00b_widget_layout(plane, plane->bounds);
        return;
    }

    scroll_mark_layout_dirty(plane);
}

static void
scroll_apply_offsets(n00b_plane_t *plane,
                     n00b_scroll_t *scroll,
                     int32_t        offset_x,
                     int32_t        offset_y)
{
    if (!plane || !scroll) {
        return;
    }

    scroll->offset_x = offset_x;
    scroll->offset_y = offset_y;
    scroll_clamp_offsets(scroll);
    scroll_relayout_if_possible(plane);
}

static void
scroll_scroll_vertical_thumb_to_pointer(n00b_plane_t *plane,
                                        n00b_scroll_t *scroll,
                                        int32_t        local_y)
{
    int32_t max_offset;
    int32_t max_thumb_y;
    int32_t thumb_h;
    int32_t drag_delta;
    int32_t new_offset;

    if (!plane || !scroll || !scroll->dragging_vertical_thumb
        || scroll_rect_is_empty(scroll->vthumb_rect)) {
        return;
    }

    max_offset = scroll_max_offset_y(scroll);
    thumb_h = scroll->vthumb_rect.height;
    max_thumb_y = scroll_max_i32(scroll->viewport_height - thumb_h, 0);

    if (max_offset <= 0 || max_thumb_y <= 0) {
        return;
    }

    drag_delta = local_y - scroll->drag_anchor_px;
    new_offset = scroll->drag_anchor_offset_px + (drag_delta * max_offset) / max_thumb_y;
    scroll_apply_offsets(plane, scroll, scroll->offset_x, new_offset);
}

static void
scroll_scroll_horizontal_thumb_to_pointer(n00b_plane_t *plane,
                                          n00b_scroll_t *scroll,
                                          int32_t        local_x)
{
    int32_t max_offset;
    int32_t max_thumb_x;
    int32_t thumb_w;
    int32_t drag_delta;
    int32_t new_offset;

    if (!plane || !scroll || !scroll->dragging_horizontal_thumb
        || scroll_rect_is_empty(scroll->hthumb_rect)) {
        return;
    }

    max_offset = scroll_max_offset_x(scroll);
    thumb_w = scroll->hthumb_rect.width;
    max_thumb_x = scroll_max_i32(scroll->viewport_width - thumb_w, 0);

    if (max_offset <= 0 || max_thumb_x <= 0) {
        return;
    }

    drag_delta = local_x - scroll->drag_anchor_px;
    new_offset = scroll->drag_anchor_offset_px + (drag_delta * max_offset) / max_thumb_x;
    scroll_apply_offsets(plane, scroll, new_offset, scroll->offset_y);
}

static void
scroll_destroy(n00b_plane_t *plane, void *data)
{
    n00b_scroll_impl_t *impl = data;
    n00b_scroll_t      *scroll = impl ? &impl->public_state : nullptr;

    scroll_release_capture_if_owned(plane);

    if (scroll) {
        n00b_free(scroll->track_style);
        n00b_free(scroll->thumb_style);
        n00b_free(scroll->thumb_hover_style);
    }

    if (impl) {
        n00b_free(impl);
    }
}

static void
scroll_render(n00b_plane_t *plane, void *data)
{
    n00b_scroll_t *scroll = data;
    n00b_text_style_t *thumb_style;

    if (!plane || !scroll) {
        return;
    }

    n00b_plane_clear(plane);
    scroll_refresh_styles(scroll);

    if (scroll->show_vscrollbar) {
        n00b_plane_fill_rect(plane,
                             scroll->vscrollbar_rect.x,
                             scroll->vscrollbar_rect.y,
                             scroll->vscrollbar_rect.width,
                             scroll->vscrollbar_rect.height,
                             .style = scroll->track_style);
    }

    if (scroll->show_hscrollbar) {
        n00b_plane_fill_rect(plane,
                             scroll->hscrollbar_rect.x,
                             scroll->hscrollbar_rect.y,
                             scroll->hscrollbar_rect.width,
                             scroll->hscrollbar_rect.height,
                             .style = scroll->track_style);
    }

    if (scroll->show_vscrollbar && !scroll_rect_is_empty(scroll->vthumb_rect)) {
        thumb_style = (scroll->hover_vertical_thumb || scroll->dragging_vertical_thumb)
                    ? scroll->thumb_hover_style
                    : scroll->thumb_style;
        n00b_plane_fill_rect(plane,
                             scroll->vthumb_rect.x,
                             scroll->vthumb_rect.y,
                             scroll->vthumb_rect.width,
                             scroll->vthumb_rect.height,
                             .style = thumb_style);
    }

    if (scroll->show_hscrollbar && !scroll_rect_is_empty(scroll->hthumb_rect)) {
        thumb_style = (scroll->hover_horizontal_thumb || scroll->dragging_horizontal_thumb)
                    ? scroll->thumb_hover_style
                    : scroll->thumb_style;
        n00b_plane_fill_rect(plane,
                             scroll->hthumb_rect.x,
                             scroll->hthumb_rect.y,
                             scroll->hthumb_rect.width,
                             scroll->hthumb_rect.height,
                             .style = thumb_style);
    }

    if (scroll->show_vscrollbar && scroll->show_hscrollbar) {
        n00b_plane_fill_rect(plane,
                             scroll->viewport_width,
                             scroll->viewport_height,
                             scroll->scrollbar_thickness_px,
                             scroll->scrollbar_thickness_px,
                             .style = scroll->track_style);
    }
}

static void
scroll_measure(n00b_plane_t *plane, void *data,
               int32_t *pref_w, int32_t *pref_h,
               int32_t *min_w, int32_t *min_h)
{
    n00b_scroll_t *scroll = data;
    int32_t content_pref_w = 1;
    int32_t content_pref_h = 1;
    int32_t content_min_w = 1;
    int32_t content_min_h = 1;
    int32_t default_viewport_w;
    int32_t default_viewport_h;
    int32_t cell_w;
    int32_t line_h;
    int32_t measure_width = 0;

    if (!scroll) {
        *pref_w = *pref_h = *min_w = *min_h = 1;
        return;
    }

    if (scroll->content) {
        if (!scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL) && plane) {
            measure_width = plane->width > 0 ? plane->width : plane->bounds.width;
            if (measure_width > 0
                && scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)
                && scroll->scrollbar_mode == N00B_SCROLLBAR_ALWAYS) {
                measure_width -= scroll->scrollbar_thickness_px;
            }
            measure_width = scroll_max_i32(measure_width, 1);
        }

        if (measure_width > 0) {
            scroll_measure_content_for_width(scroll->content,
                                             measure_width,
                                             &content_pref_w,
                                             &content_pref_h,
                                             &content_min_w,
                                             &content_min_h);
        }
        else {
            scroll_measure_content(scroll->content,
                                   &content_pref_w,
                                   &content_pref_h,
                                   &content_min_w,
                                   &content_min_h);
        }
    }

    content_pref_w = scroll_max_i32(content_pref_w, 1);
    content_pref_h = scroll_max_i32(content_pref_h, 1);
    content_min_w = scroll_max_i32(content_min_w, 1);
    content_min_h = scroll_max_i32(content_min_h, 1);

    cell_w = n00b_widget_cell_px_width(plane);
    line_h = n00b_widget_line_px_height(plane);
    default_viewport_w = 20 * cell_w;
    default_viewport_h = 3 * line_h;

    if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
        *pref_w = scroll_min_i32(content_pref_w, default_viewport_w);
        *min_w  = cell_w;
    }
    else {
        *pref_w = content_pref_w;
        *min_w  = content_min_w;
    }

    if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
        *pref_h = scroll_min_i32(content_pref_h, default_viewport_h);
        *min_h  = line_h;
    }
    else {
        *pref_h = content_pref_h;
        *min_h  = content_min_h;
    }

    if (scroll->scrollbar_mode == N00B_SCROLLBAR_ALWAYS) {
        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
            *pref_w += scroll->scrollbar_thickness_px;
            *min_w  += scroll->scrollbar_thickness_px;
        }
        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
            *pref_h += scroll->scrollbar_thickness_px;
            *min_h  += scroll->scrollbar_thickness_px;
        }
    }
}

static bool
scroll_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_scroll_t *scroll = data;
    int32_t line_h;
    int32_t cell_w;
    int32_t step_px;

    if (!plane || !scroll || !event) {
        return false;
    }

    if (event->type == N00B_EVENT_KEY) {
        line_h = n00b_widget_line_px_height(plane);
        cell_w = n00b_widget_cell_px_width(plane);

        switch (event->key.key) {
        case N00B_KEY_UP:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_by(plane, 0, -line_h);
                return true;
            }
            return false;
        case N00B_KEY_DOWN:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_by(plane, 0, line_h);
                return true;
            }
            return false;
        case N00B_KEY_LEFT:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
                n00b_scroll_by(plane, -cell_w, 0);
                return true;
            }
            return false;
        case N00B_KEY_RIGHT:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
                n00b_scroll_by(plane, cell_w, 0);
                return true;
            }
            return false;
        case N00B_KEY_PAGE_UP:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_by(plane, 0, -scroll->viewport_height);
                return true;
            }
            return false;
        case N00B_KEY_PAGE_DOWN:
            if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_by(plane, 0, scroll->viewport_height);
                return true;
            }
            return false;
        case N00B_KEY_HOME:
            if ((event->key.mods & N00B_MOD_CTRL)
                && scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_to_top(plane);
                return true;
            }
            return false;
        case N00B_KEY_END:
            if ((event->key.mods & N00B_MOD_CTRL)
                && scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
                n00b_scroll_to_bottom(plane);
                return true;
            }
            return false;
        default:
            return false;
        }
    }

    if (event->type != N00B_EVENT_MOUSE) {
        return false;
    }

    line_h = n00b_widget_line_px_height(plane);
    cell_w = n00b_widget_cell_px_width(plane);
    step_px = scroll->scroll_step_lines * line_h;

    if ((event->mouse.button == N00B_MOUSE_SCROLL_UP
         || event->mouse.button == N00B_MOUSE_SCROLL_DOWN)
        && event->mouse.action == N00B_MOUSE_PRESS) {
        int32_t direction = event->mouse.button == N00B_MOUSE_SCROLL_UP ? -1 : 1;

        if ((event->mouse.mods & N00B_MOD_SHIFT)
            && scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
            n00b_scroll_by(plane, direction * scroll->scroll_step_lines * cell_w, 0);
            return true;
        }

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
            n00b_scroll_by(plane, 0, direction * step_px);
            return true;
        }

        if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
            n00b_scroll_by(plane, direction * scroll->scroll_step_lines * cell_w, 0);
            return true;
        }

        return false;
    }

    if (scroll->dragging_vertical_thumb
        && (event->mouse.action == N00B_MOUSE_MOVE
            || event->mouse.action == N00B_MOUSE_DRAG)) {
        scroll_scroll_vertical_thumb_to_pointer(plane, scroll, event->mouse.y);
        scroll_update_hover_state(plane, scroll, event->mouse.x, event->mouse.y);
        return true;
    }

    if (scroll->dragging_horizontal_thumb
        && (event->mouse.action == N00B_MOUSE_MOVE
            || event->mouse.action == N00B_MOUSE_DRAG)) {
        scroll_scroll_horizontal_thumb_to_pointer(plane, scroll, event->mouse.x);
        scroll_update_hover_state(plane, scroll, event->mouse.x, event->mouse.y);
        return true;
    }

    if ((scroll->dragging_vertical_thumb || scroll->dragging_horizontal_thumb)
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_RELEASE) {
        scroll_stop_thumb_drag(plane, scroll);
        scroll_update_hover_state(plane, scroll, event->mouse.x, event->mouse.y);
        scroll_mark_layout_dirty(plane);
        return true;
    }

    if (event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS
        && scroll_point_in_rect(scroll->vthumb_rect, event->mouse.x, event->mouse.y)) {
        scroll_stop_thumb_drag(plane, scroll);
        scroll->dragging_vertical_thumb = true;
        scroll->hover_vertical_thumb = true;
        scroll->drag_anchor_px = event->mouse.y;
        scroll->drag_anchor_offset_px = scroll->offset_y;
        if (plane->canvas) {
            n00b_canvas_capture_mouse(plane->canvas, plane);
        }
        scroll_mark_layout_dirty(plane);
        return true;
    }

    if (event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS
        && scroll_point_in_rect(scroll->hthumb_rect, event->mouse.x, event->mouse.y)) {
        scroll_stop_thumb_drag(plane, scroll);
        scroll->dragging_horizontal_thumb = true;
        scroll->hover_horizontal_thumb = true;
        scroll->drag_anchor_px = event->mouse.x;
        scroll->drag_anchor_offset_px = scroll->offset_x;
        if (plane->canvas) {
            n00b_canvas_capture_mouse(plane->canvas, plane);
        }
        scroll_mark_layout_dirty(plane);
        return true;
    }

    if (event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS
        && scroll_point_in_rect(scroll->vscrollbar_rect, event->mouse.x, event->mouse.y)) {
        if (event->mouse.y < scroll->vthumb_rect.y) {
            n00b_scroll_by(plane, 0, -scroll->viewport_height);
        }
        else if (event->mouse.y >= scroll->vthumb_rect.y + scroll->vthumb_rect.height) {
            n00b_scroll_by(plane, 0, scroll->viewport_height);
        }
        return true;
    }

    if (event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS
        && scroll_point_in_rect(scroll->hscrollbar_rect, event->mouse.x, event->mouse.y)) {
        if (event->mouse.x < scroll->hthumb_rect.x) {
            n00b_scroll_by(plane, -scroll->viewport_width, 0);
        }
        else if (event->mouse.x >= scroll->hthumb_rect.x + scroll->hthumb_rect.width) {
            n00b_scroll_by(plane, scroll->viewport_width, 0);
        }
        return true;
    }

    if (event->mouse.action == N00B_MOUSE_MOVE) {
        scroll_update_hover_state(plane, scroll, event->mouse.x, event->mouse.y);
        return false;
    }

    return false;
}

static bool
scroll_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    return true;
}

static void
scroll_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    n00b_scroll_impl_t *impl = data;

    if (!plane || !impl) {
        return;
    }

    scroll_layout_content(plane, impl, bounds);
}

const n00b_widget_vtable_t n00b_widget_scroll = {
    .kind                 = "scroll",
    .destroy              = scroll_destroy,
    .render               = scroll_render,
    .measure              = scroll_measure,
    .handle_event         = scroll_handle_event,
    .can_focus            = scroll_can_focus,
    .cancel_mouse_capture = scroll_cancel_mouse_capture,
    .layout               = scroll_layout,
};

n00b_plane_t *
n00b_scroll_new(n00b_plane_t *content) _kargs {
    n00b_scroll_axis_t    axes                   = N00B_SCROLL_AXIS_VERTICAL;
    n00b_scrollbar_mode_t scrollbar_mode         = N00B_SCROLLBAR_AUTO;
    int32_t               scroll_step_lines      = 3;
    int32_t               scrollbar_thickness_px = 1;
    n00b_canvas_t        *canvas                 = nullptr;
    n00b_allocator_t     *allocator              = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                         .canvas    = canvas,
                                         .allocator = allocator);
    n00b_scroll_impl_t *impl = n00b_alloc(n00b_scroll_impl_t);
    n00b_scroll_t      *scroll = &impl->public_state;

    memset(impl, 0, sizeof(*impl));

    scroll->axes = axes;
    scroll->scrollbar_mode = scrollbar_mode;
    scroll->scroll_step_lines = scroll_max_i32(scroll_step_lines, 1);
    scroll->scrollbar_thickness_px = scroll_max_i32(scrollbar_thickness_px, 1);
    scroll->track_style = n00b_str_style_new();
    scroll->thumb_style = n00b_str_style_new();
    scroll->thumb_hover_style = n00b_str_style_new();

    impl->viewport_plane = n00b_new_kargs(n00b_plane_t, plane,
                                          .canvas    = canvas,
                                          .allocator = allocator);
    n00b_widget_attach(impl->viewport_plane, &n00b_widget_scroll_viewport, nullptr);

    n00b_widget_attach(plane, &n00b_widget_scroll, impl);
    n00b_plane_add_child(plane, impl->viewport_plane, 0, 0);

    if (content) {
        assert(content->parent == nullptr);
        scroll->content = content;
        n00b_plane_add_child(impl->viewport_plane, content, 0, 0);
    }

    scroll_mark_layout_dirty(plane);
    return plane;
}

void
n00b_scroll_set_content(n00b_plane_t *plane, n00b_plane_t *content)
{
    n00b_scroll_impl_t *impl = scroll_impl(plane);
    n00b_scroll_t      *scroll = impl ? &impl->public_state : nullptr;

    if (!impl || !scroll) {
        return;
    }

    if (scroll->content) {
        (void)n00b_plane_remove_child(impl->viewport_plane, scroll->content);
        scroll->content = nullptr;
    }

    if (content) {
        assert(content->parent == nullptr);
        scroll->content = content;
        n00b_plane_add_child(impl->viewport_plane, content, 0, 0);
    }

    scroll_stop_thumb_drag(plane, scroll);
    scroll->hover_vertical_thumb = false;
    scroll->hover_horizontal_thumb = false;
    scroll->offset_x = 0;
    scroll->offset_y = 0;

    if (scroll_has_valid_bounds(plane)) {
        n00b_widget_layout(plane, plane->bounds);
        return;
    }

    scroll_mark_layout_dirty(plane);
}

n00b_plane_t *
n00b_scroll_get_content(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll ? scroll->content : nullptr;
}

void
n00b_scroll_to(n00b_plane_t *plane, int32_t x_px, int32_t y_px)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    scroll_apply_offsets(plane, scroll, x_px, y_px);
}

void
n00b_scroll_by(n00b_plane_t *plane, int32_t dx_px, int32_t dy_px)
{
    n00b_scroll_t *scroll = scroll_state(plane);

    if (!scroll) {
        return;
    }

    scroll_apply_offsets(plane,
                         scroll,
                         scroll->offset_x + dx_px,
                         scroll->offset_y + dy_px);
}

void
n00b_scroll_to_top(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    if (!scroll) {
        return;
    }

    scroll_apply_offsets(plane, scroll, scroll->offset_x, 0);
}

void
n00b_scroll_to_bottom(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    if (!scroll) {
        return;
    }

    scroll_apply_offsets(plane, scroll, scroll->offset_x, scroll_max_offset_y(scroll));
}

void
n00b_scroll_to_start(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    if (!scroll) {
        return;
    }

    scroll_apply_offsets(plane, scroll, 0, scroll->offset_y);
}

void
n00b_scroll_to_end(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    if (!scroll) {
        return;
    }

    scroll_apply_offsets(plane, scroll, scroll_max_offset_x(scroll), scroll->offset_y);
}

void
n00b_scroll_ensure_visible(n00b_plane_t *plane, n00b_rect_t rect_px)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    int32_t        target_x;
    int32_t        target_y;

    if (!scroll) {
        return;
    }

    target_x = scroll->offset_x;
    target_y = scroll->offset_y;

    if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_HORIZONTAL)) {
        if (rect_px.x < target_x) {
            target_x = rect_px.x;
        }
        else if (rect_px.x + rect_px.width > target_x + scroll->viewport_width) {
            target_x = rect_px.x + rect_px.width - scroll->viewport_width;
        }
    }

    if (scroll_axis_enabled(scroll, N00B_SCROLL_AXIS_VERTICAL)) {
        if (rect_px.y < target_y) {
            target_y = rect_px.y;
        }
        else if (rect_px.y + rect_px.height > target_y + scroll->viewport_height) {
            target_y = rect_px.y + rect_px.height - scroll->viewport_height;
        }
    }

    scroll_apply_offsets(plane, scroll, target_x, target_y);
}

int32_t
n00b_scroll_get_offset_x(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll ? scroll->offset_x : 0;
}

int32_t
n00b_scroll_get_offset_y(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll ? scroll->offset_y : 0;
}

bool
n00b_scroll_can_scroll_up(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll && scroll->offset_y > 0;
}

bool
n00b_scroll_can_scroll_down(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll && scroll->offset_y < scroll_max_offset_y(scroll);
}

bool
n00b_scroll_can_scroll_left(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll && scroll->offset_x > 0;
}

bool
n00b_scroll_can_scroll_right(n00b_plane_t *plane)
{
    n00b_scroll_t *scroll = scroll_state(plane);
    return scroll && scroll->offset_x < scroll_max_offset_x(scroll);
}
