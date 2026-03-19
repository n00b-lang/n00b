/*
 * Split widget: two-pane layout with a draggable divider.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/mouse.h"
#include "display/render/box.h"
#include "display/render/composite.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/split.h"
#include "internal/display/widget_primitives.h"
#include "text/strings/theme.h"

static n00b_split_t *
split_data(n00b_plane_t *plane)
{
    return n00b_widget_data_if_kind(plane, &n00b_widget_split);
}

static int32_t
split_clamp_non_negative(int32_t value)
{
    return value < 0 ? 0 : value;
}

static float
split_clamp_ratio(float ratio)
{
    if (ratio < 0.0f) {
        return 0.0f;
    }
    if (ratio > 1.0f) {
        return 1.0f;
    }
    return ratio;
}

static int32_t
split_clamp_i32(int32_t value, int32_t low, int32_t high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static bool
split_child_visible(n00b_plane_t *child)
{
    return child && (child->flags & N00B_PLANE_VISIBLE);
}

static void
split_clear_divider(n00b_split_t *split)
{
    if (!split) {
        return;
    }

    split->divider_rect = (n00b_rect_t){};
    split->divider_hovered = false;
}

static bool
split_point_in_divider(const n00b_split_t *split, int32_t x, int32_t y)
{
    if (!split) {
        return false;
    }

    return split->divider_rect.width > 0
        && split->divider_rect.height > 0
        && x >= split->divider_rect.x
        && y >= split->divider_rect.y
        && x < split->divider_rect.x + split->divider_rect.width
        && y < split->divider_rect.y + split->divider_rect.height;
}

static void
split_sync_plane_state(n00b_plane_t *plane, n00b_split_t *split)
{
    n00b_widget_state_t state = N00B_WSTATE_NORMAL;

    if (split && split->dragging) {
        state = N00B_WSTATE_ACTIVE;
    }
    else if (split && split->divider_hovered) {
        state = N00B_WSTATE_HOVER;
    }

    if (n00b_plane_get_state(plane) != state) {
        n00b_plane_set_state(plane, state);
    }
}

static void
split_sync_child_order(n00b_plane_t *plane, const n00b_split_t *split)
{
    size_t write_ix = 0;

    if (!plane || !split || !plane->children.data) {
        return;
    }

    if (split->first && split->first->parent == plane) {
        plane->children.data[write_ix++] = split->first;
    }
    if (split->second && split->second->parent == plane) {
        plane->children.data[write_ix++] = split->second;
    }

    plane->children.len = write_ix;
}

static void
split_relayout_if_needed(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    if (plane->bounds.width > 0 && plane->bounds.height > 0) {
        n00b_widget_layout(plane, plane->bounds);
        return;
    }

    n00b_plane_mark_dirty(plane);
}

static void
split_fire_change(n00b_plane_t *plane, n00b_split_t *split)
{
    if (split && split->on_change) {
        split->on_change(plane, split->ratio, split->on_change_data);
    }
}

static void
split_resolve_visible_children(const n00b_split_t *split,
                               n00b_plane_t      **out_first,
                               n00b_plane_t      **out_second)
{
    n00b_plane_t *first = split_child_visible(split ? split->first : nullptr)
                        ? split->first
                        : nullptr;
    n00b_plane_t *second = split_child_visible(split ? split->second : nullptr)
                         ? split->second
                         : nullptr;

    if (out_first) {
        *out_first = first;
    }
    if (out_second) {
        *out_second = second;
    }
}

static int32_t
split_main_axis_size(n00b_plane_t *plane, const n00b_split_t *split)
{
    if (!plane || !split) {
        return 0;
    }

    return split->orientation == N00B_SPLIT_HORIZONTAL
         ? plane->width
         : plane->height;
}

static int32_t
split_event_main_axis(const n00b_event_t *event, const n00b_split_t *split)
{
    if (!event || !split) {
        return 0;
    }

    return split->orientation == N00B_SPLIT_HORIZONTAL
         ? event->mouse.x
         : event->mouse.y;
}

static int32_t
split_divider_axis_start(const n00b_split_t *split)
{
    if (!split) {
        return 0;
    }

    return split->orientation == N00B_SPLIT_HORIZONTAL
         ? split->divider_rect.x
         : split->divider_rect.y;
}

static int32_t
split_available_main_from_content(const n00b_split_t *split, int32_t content_main)
{
    if (!split) {
        return 0;
    }

    content_main -= split->divider_px;
    return content_main > 0 ? content_main : 0;
}

static void
split_measure_child(n00b_plane_t *child,
                    int32_t      *pref_w,
                    int32_t      *pref_h,
                    int32_t      *min_w,
                    int32_t      *min_h)
{
    if (child && child->widget_vtable) {
        n00b_widget_measure(child, pref_w, pref_h, min_w, min_h);
        return;
    }

    n00b_widget_measure_plain_plane(child, pref_w, pref_h, min_w, min_h);
}

static n00b_box_props_t *
split_make_box(void)
{
    n00b_box_props_t *box = n00b_box_props_new();
    n00b_state_style_t *hover = n00b_alloc(n00b_state_style_t);
    n00b_state_style_t *active = n00b_alloc(n00b_state_style_t);

    box->borders = N00B_BORDER_NONE;
    box->fill_style = n00b_alloc(n00b_text_style_t);
    box->fill_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_BORDER);

    box->text_style = n00b_alloc(n00b_text_style_t);
    box->text_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_BORDER_DARK);
    box->text_style->bold   = N00B_TRI_YES;

    hover->fill_style = n00b_alloc(n00b_text_style_t);
    hover->fill_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_HOVER);

    active->fill_style = n00b_alloc(n00b_text_style_t);
    active->fill_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_ACTIVE);

    box->state_styles[N00B_WSTATE_HOVER] = hover;
    box->state_styles[N00B_WSTATE_ACTIVE] = active;

    return box;
}

static void
split_destroy(n00b_plane_t *plane, void *data)
{
    n00b_split_t *split = data;

    if (plane && plane->canvas
        && n00b_canvas_get_mouse_capture(plane->canvas) == plane) {
        n00b_canvas_cancel_mouse_capture(plane->canvas);
    }

    if (split) {
        n00b_free(split);
    }
}

static void
split_render(n00b_plane_t *plane, void *data)
{
    n00b_split_t *split = data;
    int32_t       cpw;
    int32_t       cph;
    int32_t       grip_len;
    int32_t       grip_start_x;
    int32_t       grip_start_y;
    n00b_text_style_t *fill_style;
    n00b_text_style_t *grip_style;
    n00b_codepoint_t   grip_cp;

    if (!plane || !split) {
        return;
    }

    n00b_plane_clear(plane);

    if (split->divider_rect.width <= 0 || split->divider_rect.height <= 0) {
        return;
    }

    fill_style = n00b_composite_resolve_style(plane,
                                              plane->box ? plane->box->fill_style : plane->default_style,
                                              2);
    grip_style = n00b_composite_resolve_style(plane,
                                              plane->box ? plane->box->text_style : plane->default_style,
                                              0);

    n00b_plane_fill_rect(plane,
                         split->divider_rect.x,
                         split->divider_rect.y,
                         split->divider_rect.width,
                         split->divider_rect.height,
                         .style = fill_style);

    cpw = n00b_widget_cell_px_width(plane);
    cph = n00b_widget_line_px_height(plane);

    if (split->orientation == N00B_SPLIT_HORIZONTAL) {
        if (split->divider_rect.width < cpw || split->divider_rect.height < (3 * cph)) {
            return;
        }

        grip_cp = n00b_border_plain.vertical;
        grip_start_x = split->divider_rect.x + (split->divider_rect.width - cpw) / 2;
        grip_start_y = split->divider_rect.y + (split->divider_rect.height - (3 * cph)) / 2;

        for (int i = 0; i < 3; i++) {
            n00b_plane_draw_glyph(plane,
                                  grip_start_x,
                                  grip_start_y + (i * cph),
                                  grip_cp,
                                  .style = grip_style);
        }

        return;
    }

    if (split->divider_rect.height < cph || split->divider_rect.width < (3 * cpw)) {
        return;
    }

    grip_cp = n00b_border_plain.horizontal;
    grip_len = 3;
    grip_start_x = split->divider_rect.x + (split->divider_rect.width - (grip_len * cpw)) / 2;
    grip_start_y = split->divider_rect.y + (split->divider_rect.height - cph) / 2;

    for (int i = 0; i < grip_len; i++) {
        n00b_plane_draw_glyph(plane,
                              grip_start_x + (i * cpw),
                              grip_start_y,
                              grip_cp,
                              .style = grip_style);
    }
}

static void
split_measure(n00b_plane_t *plane, void *data,
              int32_t *pref_w, int32_t *pref_h,
              int32_t *min_w, int32_t *min_h)
{
    n00b_split_t *split = data;
    n00b_plane_t *first = nullptr;
    n00b_plane_t *second = nullptr;
    int32_t       first_pref_w = 0;
    int32_t       first_pref_h = 0;
    int32_t       first_min_w = 0;
    int32_t       first_min_h = 0;
    int32_t       second_pref_w = 0;
    int32_t       second_pref_h = 0;
    int32_t       second_min_w = 0;
    int32_t       second_min_h = 0;

    (void)plane;

    split_resolve_visible_children(split, &first, &second);

    if (first && second) {
        split_measure_child(first,
                            &first_pref_w,
                            &first_pref_h,
                            &first_min_w,
                            &first_min_h);
        split_measure_child(second,
                            &second_pref_w,
                            &second_pref_h,
                            &second_min_w,
                            &second_min_h);

        if (split->orientation == N00B_SPLIT_HORIZONTAL) {
            first_pref_w = n00b_max(first_pref_w, split->min_first_px);
            first_min_w = n00b_max(first_min_w, split->min_first_px);
            second_pref_w = n00b_max(second_pref_w, split->min_second_px);
            second_min_w = n00b_max(second_min_w, split->min_second_px);

            *pref_w = first_pref_w + split->divider_px + second_pref_w;
            *pref_h = n00b_max(first_pref_h, second_pref_h);
            *min_w  = first_min_w + split->divider_px + second_min_w;
            *min_h  = n00b_max(first_min_h, second_min_h);
        }
        else {
            first_pref_h = n00b_max(first_pref_h, split->min_first_px);
            first_min_h = n00b_max(first_min_h, split->min_first_px);
            second_pref_h = n00b_max(second_pref_h, split->min_second_px);
            second_min_h = n00b_max(second_min_h, split->min_second_px);

            *pref_w = n00b_max(first_pref_w, second_pref_w);
            *pref_h = first_pref_h + split->divider_px + second_pref_h;
            *min_w  = n00b_max(first_min_w, second_min_w);
            *min_h  = first_min_h + split->divider_px + second_min_h;
        }

        return;
    }

    if (first || second) {
        n00b_plane_t *only = first ? first : second;
        split_measure_child(only, pref_w, pref_h, min_w, min_h);
        return;
    }

    *pref_w = 1;
    *pref_h = 1;
    *min_w  = 1;
    *min_h  = 1;
}

static bool
split_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    return false;
}

static void
split_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    n00b_split_t *split = data;
    n00b_plane_t *first = nullptr;
    n00b_plane_t *second = nullptr;
    int32_t       main_axis_size;
    int32_t       available_main;
    int32_t       requested_first;
    int32_t       first_size;
    int32_t       second_size;

    if (!plane || !split) {
        return;
    }

    split_resolve_visible_children(split, &first, &second);

    if (first && second) {
        main_axis_size = split->orientation == N00B_SPLIT_HORIZONTAL
                       ? bounds.width
                       : bounds.height;
        available_main = split_available_main_from_content(split, main_axis_size);
        requested_first = (int32_t)(split->ratio * (float)available_main);

        if (split->min_first_px + split->min_second_px <= available_main) {
            first_size = split_clamp_i32(requested_first,
                                         split->min_first_px,
                                         available_main - split->min_second_px);
        }
        else {
            first_size = split_clamp_i32(requested_first, 0, available_main);
        }

        second_size = available_main - first_size;

        if (split->orientation == N00B_SPLIT_HORIZONTAL) {
            split->divider_rect = (n00b_rect_t){
                .x      = first_size,
                .y      = 0,
                .width  = split->divider_px,
                .height = bounds.height,
            };

            n00b_widget_layout(first,
                               (n00b_rect_t){
                                   .x      = bounds.x,
                                   .y      = bounds.y,
                                   .width  = first_size,
                                   .height = bounds.height,
                               });
            n00b_widget_layout(second,
                               (n00b_rect_t){
                                   .x      = bounds.x + first_size + split->divider_px,
                                   .y      = bounds.y,
                                   .width  = second_size,
                                   .height = bounds.height,
                               });
        }
        else {
            split->divider_rect = (n00b_rect_t){
                .x      = 0,
                .y      = first_size,
                .width  = bounds.width,
                .height = split->divider_px,
            };

            n00b_widget_layout(first,
                               (n00b_rect_t){
                                   .x      = bounds.x,
                                   .y      = bounds.y,
                                   .width  = bounds.width,
                                   .height = first_size,
                               });
            n00b_widget_layout(second,
                               (n00b_rect_t){
                                   .x      = bounds.x,
                                   .y      = bounds.y + first_size + split->divider_px,
                                   .width  = bounds.width,
                                   .height = second_size,
                               });
        }

        if (split->dragging) {
            split->divider_hovered = true;
        }
        split_sync_plane_state(plane, split);
        return;
    }

    split_clear_divider(split);
    split_sync_plane_state(plane, split);

    if (first || second) {
        n00b_widget_layout(first ? first : second, bounds);
    }
}

static bool
split_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_split_t *split = data;
    bool          hovered;
    int32_t       available_main;
    int32_t       requested_first;
    float         new_ratio;

    if (!plane || !split || !event || event->type != N00B_EVENT_MOUSE) {
        return false;
    }

    hovered = split_point_in_divider(split, event->mouse.x, event->mouse.y);

    if (event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS
        && hovered) {
        split->dragging = true;
        split->divider_hovered = true;
        split->drag_pointer_offset_px = split_event_main_axis(event, split)
                                      - split_divider_axis_start(split);
        split_sync_plane_state(plane, split);

        if (plane->canvas) {
            n00b_canvas_capture_mouse(plane->canvas, plane);
        }

        return true;
    }

    if (split->dragging) {
        if (event->mouse.action == N00B_MOUSE_MOVE
            || event->mouse.action == N00B_MOUSE_DRAG) {
            available_main = split_available_main_from_content(split,
                                                               split_main_axis_size(plane, split));
            if (available_main > 0) {
                requested_first = split_event_main_axis(event, split)
                                - split->drag_pointer_offset_px;
                new_ratio = (float)requested_first / (float)available_main;
                new_ratio = split_clamp_ratio(new_ratio);

                if (new_ratio != split->ratio) {
                    split->ratio = new_ratio;
                    split_relayout_if_needed(plane);
                    split_fire_change(plane, split);
                }
            }

            return true;
        }

        if (event->mouse.button == N00B_MOUSE_LEFT
            && event->mouse.action == N00B_MOUSE_RELEASE) {
            split->dragging = false;
            if (plane->canvas) {
                n00b_canvas_release_mouse(plane->canvas);
            }
            split->divider_hovered = split_point_in_divider(split,
                                                            event->mouse.x,
                                                            event->mouse.y);
            split_sync_plane_state(plane, split);
            return true;
        }
    }

    if (event->mouse.action == N00B_MOUSE_MOVE
        || event->mouse.action == N00B_MOUSE_DRAG) {
        if (split->divider_hovered != hovered) {
            split->divider_hovered = hovered;
            split_sync_plane_state(plane, split);
        }
        return false;
    }

    return false;
}

const n00b_widget_vtable_t n00b_widget_split = {
    .kind         = "split",
    .destroy      = split_destroy,
    .render       = split_render,
    .measure      = split_measure,
    .handle_event = split_handle_event,
    .can_focus    = split_can_focus,
    .layout       = split_layout,
};

n00b_plane_t *
n00b_split_new(n00b_plane_t *first, n00b_plane_t *second) _kargs {
    n00b_split_orientation_t orientation    = N00B_SPLIT_HORIZONTAL;
    float                    ratio          = 0.5f;
    int32_t                  min_first_px   = 64;
    int32_t                  min_second_px  = 64;
    int32_t                  divider_px     = 1;
    n00b_split_change_cb_t   on_change      = nullptr;
    void                    *on_change_data = nullptr;
    n00b_canvas_t           *canvas         = nullptr;
    n00b_allocator_t        *allocator      = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                         .box       = split_make_box(),
                                         .canvas    = canvas,
                                         .allocator = allocator);
    n00b_split_t *split = n00b_alloc(n00b_split_t);

    *split = (n00b_split_t){
        .first                  = first,
        .second                 = second,
        .orientation            = orientation,
        .ratio                  = split_clamp_ratio(ratio),
        .min_first_px           = split_clamp_non_negative(min_first_px),
        .min_second_px          = split_clamp_non_negative(min_second_px),
        .divider_px             = split_clamp_non_negative(divider_px),
        .on_change              = on_change,
        .on_change_data         = on_change_data,
        .divider_rect           = (n00b_rect_t){},
        .divider_hovered        = false,
        .dragging               = false,
        .drag_pointer_offset_px = 0,
    };

    n00b_widget_attach(plane, &n00b_widget_split, split);

    if (first) {
        assert(first->parent == nullptr);
        n00b_plane_add_child(plane, first, 0, 0);
    }
    if (second) {
        assert(second->parent == nullptr);
        n00b_plane_add_child(plane, second, 0, 0);
    }

    split_sync_child_order(plane, split);
    split_sync_plane_state(plane, split);
    n00b_plane_mark_dirty(plane);

    return plane;
}

void
n00b_split_set_ratio(n00b_plane_t *plane, float ratio)
{
    n00b_split_t *split = split_data(plane);
    float         clamped = split_clamp_ratio(ratio);

    if (!split || clamped == split->ratio) {
        return;
    }

    split->ratio = clamped;
    split_relayout_if_needed(plane);
    split_fire_change(plane, split);
}

float
n00b_split_get_ratio(n00b_plane_t *plane)
{
    n00b_split_t *split = split_data(plane);
    return split ? split->ratio : 0.0f;
}

void
n00b_split_set_first(n00b_plane_t *plane, n00b_plane_t *first)
{
    n00b_split_t *split = split_data(plane);

    if (!split || split->first == first) {
        return;
    }

    if (split->first) {
        (void)n00b_plane_remove_child(plane, split->first);
    }

    split->first = first;

    if (first) {
        assert(first->parent == nullptr);
        n00b_plane_add_child(plane, first, 0, 0);
    }

    split_sync_child_order(plane, split);
    split_relayout_if_needed(plane);
}

void
n00b_split_set_second(n00b_plane_t *plane, n00b_plane_t *second)
{
    n00b_split_t *split = split_data(plane);

    if (!split || split->second == second) {
        return;
    }

    if (split->second) {
        (void)n00b_plane_remove_child(plane, split->second);
    }

    split->second = second;

    if (second) {
        assert(second->parent == nullptr);
        n00b_plane_add_child(plane, second, 0, 0);
    }

    split_sync_child_order(plane, split);
    split_relayout_if_needed(plane);
}

void
n00b_split_set_min_sizes(n00b_plane_t *plane,
                         int32_t       min_first_px,
                         int32_t       min_second_px)
{
    n00b_split_t *split = split_data(plane);
    int32_t       clamped_first = split_clamp_non_negative(min_first_px);
    int32_t       clamped_second = split_clamp_non_negative(min_second_px);

    if (!split
        || (split->min_first_px == clamped_first
            && split->min_second_px == clamped_second)) {
        return;
    }

    split->min_first_px = clamped_first;
    split->min_second_px = clamped_second;
    split_relayout_if_needed(plane);
}

void
n00b_split_set_divider_size(n00b_plane_t *plane, int32_t divider_px)
{
    n00b_split_t *split = split_data(plane);
    int32_t       clamped = split_clamp_non_negative(divider_px);

    if (!split || split->divider_px == clamped) {
        return;
    }

    split->divider_px = clamped;
    split_relayout_if_needed(plane);
}
