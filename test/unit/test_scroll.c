/*
 * Unit tests for the scroll widget.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/box.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/scroll.h"
#include "display/widgets/text.h"
#include "internal/display/event_dispatch.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

typedef struct {
    int32_t     pref_w;
    int32_t     pref_h;
    int32_t     min_w;
    int32_t     min_h;
    bool        can_focus;
    bool        consume_click;
    int         click_count;
    int         layout_calls;
    n00b_rect_t last_bounds;
} dummy_widget_t;

typedef struct {
    int32_t      pref_w;
    int32_t      pref_h;
    int32_t      min_w;
    int32_t      min_h;
    n00b_plane_t *anchored_child;
    int32_t      child_x;
    int32_t      child_y;
    int32_t      child_w;
    int32_t      child_h;
    n00b_rect_t  last_bounds;
} dummy_container_t;

static void
dummy_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;

    if (data) {
        n00b_free(data);
    }
}

static void
dummy_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
}

static void
dummy_measure(n00b_plane_t *plane, void *data,
              int32_t *pref_w, int32_t *pref_h,
              int32_t *min_w, int32_t *min_h)
{
    (void)plane;

    dummy_widget_t *dummy = data;
    *pref_w = dummy ? dummy->pref_w : 1;
    *pref_h = dummy ? dummy->pref_h : 1;
    *min_w  = dummy ? dummy->min_w : 1;
    *min_h  = dummy ? dummy->min_h : 1;
}

static bool
dummy_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;

    dummy_widget_t *dummy = data;
    if (!dummy || !event) {
        return false;
    }

    if (event->type == N00B_EVENT_MOUSE
        && event->mouse.button == N00B_MOUSE_LEFT
        && event->mouse.action == N00B_MOUSE_PRESS) {
        dummy->click_count++;
        return dummy->consume_click;
    }

    return false;
}

static bool
dummy_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;

    dummy_widget_t *dummy = data;
    return dummy ? dummy->can_focus : false;
}

static void
dummy_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    (void)plane;

    dummy_widget_t *dummy = data;
    if (dummy) {
        dummy->layout_calls++;
        dummy->last_bounds = bounds;
    }
}

static const n00b_widget_vtable_t dummy_widget = {
    .kind         = "scroll_test_dummy",
    .destroy      = dummy_destroy,
    .render       = dummy_render,
    .measure      = dummy_measure,
    .handle_event = dummy_handle_event,
    .can_focus    = dummy_can_focus,
    .layout       = dummy_layout,
};

static void
container_measure(n00b_plane_t *plane, void *data,
                  int32_t *pref_w, int32_t *pref_h,
                  int32_t *min_w, int32_t *min_h)
{
    (void)plane;

    dummy_container_t *container = data;
    *pref_w = container ? container->pref_w : 1;
    *pref_h = container ? container->pref_h : 1;
    *min_w  = container ? container->min_w : 1;
    *min_h  = container ? container->min_h : 1;
}

static void
container_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    dummy_container_t *container = data;

    (void)plane;

    if (!container) {
        return;
    }

    container->last_bounds = bounds;

    if (container->anchored_child) {
        n00b_widget_layout(container->anchored_child,
                           (n00b_rect_t){
                               .x      = bounds.x + container->child_x,
                               .y      = bounds.y + container->child_y,
                               .width  = container->child_w,
                               .height = container->child_h,
                           });
    }
}

static const n00b_widget_vtable_t container_widget = {
    .kind    = "scroll_test_container",
    .destroy = dummy_destroy,
    .render  = dummy_render,
    .measure = container_measure,
    .layout  = container_layout,
};

static dummy_widget_t *
dummy_state(n00b_plane_t *plane)
{
    assert(plane != nullptr);
    assert(plane->widget_vtable == &dummy_widget);
    return (dummy_widget_t *)plane->widget_data;
}

static n00b_plane_t *
make_dummy_child(int32_t pref_w, int32_t pref_h,
                 int32_t min_w, int32_t min_h,
                 bool consume_click)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    dummy_widget_t *dummy = n00b_alloc(dummy_widget_t);

    *dummy = (dummy_widget_t){
        .pref_w        = pref_w > 0 ? pref_w : 1,
        .pref_h        = pref_h > 0 ? pref_h : 1,
        .min_w         = min_w > 0 ? min_w : 1,
        .min_h         = min_h > 0 ? min_h : 1,
        .can_focus     = false,
        .consume_click = consume_click,
    };

    plane->width  = dummy->pref_w;
    plane->height = dummy->pref_h;
    n00b_widget_attach(plane, &dummy_widget, dummy);

    return plane;
}

static n00b_plane_t *
make_plain_child(int32_t width, int32_t height, bool with_box)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);

    plane->width  = width > 0 ? width : 1;
    plane->height = height > 0 ? height : 1;

    if (with_box) {
        plane->box = n00b_box_props_new(.borders = N00B_BORDER_ALL);
    }

    return plane;
}

static n00b_plane_t *
make_anchored_container(n00b_plane_t *child,
                        int32_t       pref_w,
                        int32_t       pref_h,
                        int32_t       child_x,
                        int32_t       child_y,
                        int32_t       child_w,
                        int32_t       child_h)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    dummy_container_t *container = n00b_alloc(dummy_container_t);

    *container = (dummy_container_t){
        .pref_w        = pref_w > 0 ? pref_w : 1,
        .pref_h        = pref_h > 0 ? pref_h : 1,
        .min_w         = pref_w > 0 ? pref_w : 1,
        .min_h         = pref_h > 0 ? pref_h : 1,
        .anchored_child = child,
        .child_x       = child_x,
        .child_y       = child_y,
        .child_w       = child_w,
        .child_h       = child_h,
    };

    plane->width  = container->pref_w;
    plane->height = container->pref_h;
    n00b_widget_attach(plane, &container_widget, container);

    if (child) {
        n00b_plane_add_child(plane, child, 0, 0);
    }

    return plane;
}

static void
destroy_plane_tree(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    while (plane->children.data && plane->children.len > 0) {
        n00b_plane_t *child = plane->children.data[plane->children.len - 1];
        (void)n00b_plane_remove_child(plane, child);
        destroy_plane_tree(child);
    }

    n00b_widget_detach(plane);
    n00b_plane_destroy(plane);
}

static n00b_canvas_t *
make_stream_canvas(n00b_isize_t rows, n00b_isize_t cols)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, rows, cols);
    n00b_canvas_resize(canvas, rows, cols);

    return canvas;
}

static void
route_mouse(n00b_canvas_t         *canvas,
            int32_t                x,
            int32_t                y,
            n00b_mouse_button_t    button,
            n00b_mouse_action_t    action,
            n00b_key_mod_t         mods)
{
    n00b_event_t event = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = x,
            .y      = y,
            .button = button,
            .action = action,
            .mods   = mods,
        },
    };

    n00b_mouse_route_event(canvas, nullptr, &event);
}

static bool
send_key(n00b_plane_t *plane, uint32_t key, n00b_key_mod_t mods)
{
    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key = {
            .key  = key,
            .mods = mods,
        },
    };

    return n00b_widget_handle_event(plane, &event);
}

static void
assert_rect_eq(n00b_rect_t rect,
               int32_t     x,
               int32_t     y,
               int32_t     width,
               int32_t     height)
{
    assert(rect.x == x);
    assert(rect.y == y);
    assert(rect.width == width);
    assert(rect.height == height);
}

static void
test_scroll_create_and_api(void)
{
    n00b_plane_t *first = make_dummy_child(20, 8, 5, 3, false);
    n00b_plane_t *replacement = make_dummy_child(12, 6, 4, 2, false);
    n00b_plane_t *scroll = n00b_scroll_new(first);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;

    assert(scroll != nullptr);
    assert(scroll->widget_vtable == &n00b_widget_scroll);
    assert(n00b_widget_can_focus(scroll));
    assert(state != nullptr);
    assert(state->content == first);
    assert(state->axes == N00B_SCROLL_AXIS_VERTICAL);
    assert(state->scrollbar_mode == N00B_SCROLLBAR_AUTO);
    assert(state->scroll_step_lines == 3);
    assert(state->scrollbar_thickness_px == 1);
    assert(n00b_scroll_get_content(scroll) == first);
    assert(n00b_scroll_get_offset_x(scroll) == 0);
    assert(n00b_scroll_get_offset_y(scroll) == 0);
    assert(n00b_scroll_get_offset_x(nullptr) == 0);
    assert(n00b_scroll_get_offset_y(first) == 0);

    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 4,
                       });
    n00b_scroll_to(scroll, 7, 7);
    assert(n00b_scroll_get_offset_x(scroll) == 0);
    assert(n00b_scroll_get_offset_y(scroll) == 4);

    n00b_scroll_set_content(scroll, replacement);
    assert(n00b_scroll_get_content(scroll) == replacement);
    assert(first->parent == nullptr);
    assert(n00b_scroll_get_offset_x(scroll) == 0);
    assert(n00b_scroll_get_offset_y(scroll) == 0);

    destroy_plane_tree(scroll);
    destroy_plane_tree(first);
    printf("  [PASS] scroll create and api\n");
}

static void
test_scroll_measure_caps_scroll_axes(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *vertical_content = make_dummy_child(100, 50, 40, 20, false);
    n00b_plane_t *horizontal_content = make_dummy_child(100, 50, 40, 20, false);
    n00b_plane_t *both_content = make_dummy_child(100, 50, 40, 20, false);
    n00b_plane_t *vertical = n00b_scroll_new(vertical_content,
                                             .axes = N00B_SCROLL_AXIS_VERTICAL,
                                             .canvas = canvas);
    n00b_plane_t *horizontal = n00b_scroll_new(horizontal_content,
                                               .axes = N00B_SCROLL_AXIS_HORIZONTAL,
                                               .canvas = canvas);
    n00b_plane_t *both = n00b_scroll_new(both_content,
                                         .axes = N00B_SCROLL_AXIS_BOTH,
                                         .canvas = canvas);
    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w = 0;
    int32_t min_h = 0;

    n00b_widget_measure(vertical, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 100);
    assert(pref_h == 3);
    assert(min_w == 40);
    assert(min_h == 1);

    n00b_widget_measure(horizontal, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 20);
    assert(pref_h == 50);
    assert(min_w == 1);
    assert(min_h == 20);

    n00b_widget_measure(both, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 20);
    assert(pref_h == 3);
    assert(min_w == 1);
    assert(min_h == 1);

    destroy_plane_tree(vertical);
    destroy_plane_tree(horizontal);
    destroy_plane_tree(both);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll measure caps scroll axes\n");
}

static void
test_scroll_plain_plane_overflow_preserves_box_size(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *plain = make_plain_child(15, 6, true);
    n00b_plane_t *scroll = n00b_scroll_new(plain,
                                           .axes = N00B_SCROLL_AXIS_BOTH,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;

    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 8,
                           .height = 5,
                       });

    assert(state->content_width == 17);
    assert(state->content_height == 8);
    assert(state->viewport_width == 7);
    assert(state->viewport_height == 4);
    assert_rect_eq(plain->bounds, 0, 0, 17, 8);
    assert(plain->width == 15);
    assert(plain->height == 6);

    n00b_scroll_to(scroll, 999, 999);
    assert(n00b_scroll_get_offset_x(scroll) == 10);
    assert(n00b_scroll_get_offset_y(scroll) == 4);
    assert_rect_eq(plain->bounds, -10, -4, 17, 8);

    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll plain plane overflow preserves box size\n");
}

static void
test_scroll_offsets_clamp_and_direction_helpers(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(30, 20, 5, 4, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_BOTH,
                                           .canvas = canvas);
    dummy_widget_t *content_state = dummy_state(content);

    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    n00b_scroll_to(scroll, 100, 100);
    assert(n00b_scroll_get_offset_x(scroll) == 21);
    assert(n00b_scroll_get_offset_y(scroll) == 16);
    assert_rect_eq(content_state->last_bounds, -21, -16, 30, 20);
    assert(n00b_scroll_can_scroll_up(scroll));
    assert(!n00b_scroll_can_scroll_down(scroll));
    assert(n00b_scroll_can_scroll_left(scroll));
    assert(!n00b_scroll_can_scroll_right(scroll));

    n00b_scroll_by(scroll, -5, -6);
    assert(n00b_scroll_get_offset_x(scroll) == 16);
    assert(n00b_scroll_get_offset_y(scroll) == 10);
    assert_rect_eq(content_state->last_bounds, -16, -10, 30, 20);

    n00b_scroll_to_bottom(scroll);
    assert(n00b_scroll_get_offset_x(scroll) == 16);
    assert(n00b_scroll_get_offset_y(scroll) == 16);

    n00b_scroll_to_end(scroll);
    assert(n00b_scroll_get_offset_x(scroll) == 21);
    assert(n00b_scroll_get_offset_y(scroll) == 16);

    n00b_scroll_to_top(scroll);
    assert(n00b_scroll_get_offset_x(scroll) == 21);
    assert(n00b_scroll_get_offset_y(scroll) == 0);

    n00b_scroll_to_start(scroll);
    assert(n00b_scroll_get_offset_x(scroll) == 0);
    assert(n00b_scroll_get_offset_y(scroll) == 0);
    assert(!n00b_scroll_can_scroll_up(scroll));
    assert(n00b_scroll_can_scroll_down(scroll));
    assert(!n00b_scroll_can_scroll_left(scroll));
    assert(n00b_scroll_can_scroll_right(scroll));

    n00b_scroll_ensure_visible(scroll,
                               (n00b_rect_t){
                                   .x      = 18,
                                   .y      = 11,
                                   .width  = 4,
                                   .height = 3,
                               });
    assert(n00b_scroll_get_offset_x(scroll) == 13);
    assert(n00b_scroll_get_offset_y(scroll) == 10);
    assert_rect_eq(content_state->last_bounds, -13, -10, 30, 20);
    assert(n00b_scroll_can_scroll_up(scroll));
    assert(n00b_scroll_can_scroll_down(scroll));
    assert(n00b_scroll_can_scroll_left(scroll));
    assert(n00b_scroll_can_scroll_right(scroll));

    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll offsets clamp and direction helpers\n");
}

static void
test_scroll_non_scroll_axis_uses_resolved_viewport(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *vertical_content = make_dummy_child(10, 20, 10, 20, false);
    n00b_plane_t *horizontal_content = make_dummy_child(20, 5, 20, 5, false);
    n00b_plane_t *vertical = n00b_scroll_new(vertical_content,
                                             .axes = N00B_SCROLL_AXIS_VERTICAL,
                                             .canvas = canvas);
    n00b_plane_t *horizontal = n00b_scroll_new(horizontal_content,
                                               .axes = N00B_SCROLL_AXIS_HORIZONTAL,
                                               .canvas = canvas);
    n00b_scroll_t *vertical_state = (n00b_scroll_t *)vertical->widget_data;
    n00b_scroll_t *horizontal_state = (n00b_scroll_t *)horizontal->widget_data;

    n00b_widget_layout(vertical,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });
    assert(vertical_state->show_vscrollbar);
    assert(vertical_state->viewport_width == 9);
    assert(vertical_state->content_width == 9);
    assert_rect_eq(dummy_state(vertical_content)->last_bounds, 0, 0, 9, 20);

    n00b_widget_layout(horizontal,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });
    assert(horizontal_state->show_hscrollbar);
    assert(horizontal_state->viewport_height == 4);
    assert(horizontal_state->content_height == 4);
    assert_rect_eq(dummy_state(horizontal_content)->last_bounds, 0, 0, 20, 4);

    destroy_plane_tree(vertical);
    destroy_plane_tree(horizontal);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll non-scroll axis uses resolved viewport\n");
}

static void
test_scroll_auto_scrollbars_and_thumb_rects(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(10, 6, 10, 6, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_BOTH,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;

    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    assert(state->show_vscrollbar);
    assert(state->show_hscrollbar);
    assert(state->content_width == 10);
    assert(state->content_height == 6);
    assert(state->viewport_width == 9);
    assert(state->viewport_height == 4);
    assert_rect_eq(state->vscrollbar_rect, 9, 0, 1, 4);
    assert_rect_eq(state->hscrollbar_rect, 0, 4, 9, 1);
    assert_rect_eq(state->vthumb_rect, 9, 0, 1, 2);
    assert_rect_eq(state->hthumb_rect, 0, 4, 8, 1);

    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll auto scrollbars and thumb rects\n");
}

static void
test_scroll_content_click_focuses_scroll_for_keyboard_input(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *sibling = make_dummy_child(6, 1, 6, 1, false);
    n00b_plane_t *content = make_dummy_child(10, 20, 10, 20, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_VERTICAL,
                                           .canvas = canvas);
    n00b_focus_mgr_t *fm;
    n00b_display_dispatch_result_t dispatch;
    n00b_event_t click = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = 13,
            .y      = 5,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods   = N00B_MOD_NONE,
        },
    };
    n00b_event_t key_down = {
        .type = N00B_EVENT_KEY,
        .key = {
            .key  = N00B_KEY_DOWN,
            .mods = N00B_MOD_NONE,
        },
    };

    dummy_state(sibling)->can_focus = true;
    n00b_canvas_add_plane(canvas, sibling);
    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(sibling,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 6,
                           .height = 1,
                       });
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 12,
                           .y      = 4,
                           .width  = 10,
                           .height = 5,
                       });

    fm = n00b_focus_mgr_new(canvas);
    assert(n00b_focus_mgr_current(fm) == sibling);
    assert(n00b_scroll_get_offset_y(scroll) == 0);

    dispatch = n00b_display_dispatch_event(canvas, fm, &click);
    assert(dispatch.handled);
    assert(dispatch.focus_changed);
    assert(n00b_focus_mgr_current(fm) == scroll);
    assert(dummy_state(content)->click_count == 1);

    dispatch = n00b_display_dispatch_event(canvas, fm, &key_down);
    assert(dispatch.handled);
    assert(n00b_scroll_get_offset_y(scroll) == 1);

    n00b_focus_mgr_destroy(fm);
    assert(n00b_canvas_remove_plane(canvas, sibling));
    assert(n00b_canvas_remove_plane(canvas, scroll));
    destroy_plane_tree(sibling);
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll content click focuses scroll for keyboard input\n");
}

static void
test_scroll_keyboard_scroll_events(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(30, 20, 5, 4, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_BOTH,
                                           .canvas = canvas);

    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    assert(send_key(scroll, N00B_KEY_DOWN, N00B_MOD_NONE));
    assert(n00b_scroll_get_offset_y(scroll) == 1);

    assert(send_key(scroll, N00B_KEY_RIGHT, N00B_MOD_NONE));
    assert(n00b_scroll_get_offset_x(scroll) == 1);

    assert(send_key(scroll, N00B_KEY_PAGE_DOWN, N00B_MOD_NONE));
    assert(n00b_scroll_get_offset_y(scroll) == 5);

    assert(send_key(scroll, N00B_KEY_UP, N00B_MOD_NONE));
    assert(n00b_scroll_get_offset_y(scroll) == 4);

    assert(send_key(scroll, N00B_KEY_LEFT, N00B_MOD_NONE));
    assert(n00b_scroll_get_offset_x(scroll) == 0);

    assert(send_key(scroll, N00B_KEY_END, N00B_MOD_CTRL));
    assert(n00b_scroll_get_offset_y(scroll) == 16);

    assert(send_key(scroll, N00B_KEY_HOME, N00B_MOD_CTRL));
    assert(n00b_scroll_get_offset_y(scroll) == 0);

    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll keyboard scroll events\n");
}

static void
test_scroll_vertical_thumb_drag_and_track_click(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(10, 20, 10, 20, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_VERTICAL,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;
    int32_t press_x;
    int32_t press_y;

    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    press_x = state->vthumb_rect.x;
    press_y = state->vthumb_rect.y;

    route_mouse(canvas, press_x, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);

    route_mouse(canvas, press_x, press_y + 3, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG, N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_y(scroll) == 11);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);

    route_mouse(canvas, press_x, press_y + 3, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);

    n00b_scroll_to_top(scroll);
    route_mouse(canvas,
                state->vscrollbar_rect.x,
                state->vscrollbar_rect.y + state->vthumb_rect.height + 2,
                N00B_MOUSE_LEFT,
                N00B_MOUSE_PRESS,
                N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_y(scroll) == 5);

    assert(n00b_canvas_remove_plane(canvas, scroll));
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll vertical thumb drag and track click\n");
}

static void
test_scroll_detach_during_thumb_drag_clears_capture(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(10, 8, 10, 8, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_VERTICAL,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;
    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane, .canvas = canvas);
    int32_t press_x;
    int32_t press_y;
    int32_t offset_after_cancel;

    root->width  = 80;
    root->height = 20;

    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    assert(state->vthumb_rect.height > 1);
    press_x = state->vthumb_rect.x;
    press_y = state->vthumb_rect.y + state->vthumb_rect.height - 1;

    route_mouse(canvas, press_x, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);
    assert(state->dragging_vertical_thumb);
    assert(state->hover_vertical_thumb);
    assert(state->drag_anchor_px == press_y);
    assert(state->drag_anchor_offset_px == 0);

    assert(n00b_canvas_remove_plane(canvas, scroll));
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(!state->dragging_vertical_thumb);
    assert(!state->dragging_horizontal_thumb);
    assert(!state->hover_vertical_thumb);
    assert(!state->hover_horizontal_thumb);
    assert(state->drag_anchor_px == 0);
    assert(state->drag_anchor_offset_px == 0);

    offset_after_cancel = n00b_scroll_get_offset_y(scroll);
    route_mouse(canvas,
                press_x,
                press_y + 2,
                N00B_MOUSE_LEFT,
                N00B_MOUSE_DRAG,
                N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_y(scroll) == offset_after_cancel);

    n00b_canvas_add_plane(canvas, root);
    n00b_plane_add_child(root, scroll, 0, 0);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 12,
                           .y      = 4,
                           .width  = 10,
                           .height = 5,
                       });

    press_x = 12 + state->vthumb_rect.x;
    press_y = 4 + state->vthumb_rect.y + state->vthumb_rect.height - 1;
    route_mouse(canvas, press_x, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);
    assert(state->dragging_vertical_thumb);
    assert(state->hover_vertical_thumb);

    assert(n00b_plane_remove_child(root, scroll));
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(!state->dragging_vertical_thumb);
    assert(!state->dragging_horizontal_thumb);
    assert(!state->hover_vertical_thumb);
    assert(!state->hover_horizontal_thumb);
    assert(state->drag_anchor_px == 0);
    assert(state->drag_anchor_offset_px == 0);

    offset_after_cancel = n00b_scroll_get_offset_y(scroll);
    route_mouse(canvas,
                press_x,
                press_y + 2,
                N00B_MOUSE_LEFT,
                N00B_MOUSE_DRAG,
                N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_y(scroll) == offset_after_cancel);

    assert(n00b_canvas_remove_plane(canvas, root));
    destroy_plane_tree(root);
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll detach during thumb drag clears capture\n");
}

static void
test_scroll_horizontal_thumb_drag_and_track_click(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *content = make_dummy_child(20, 5, 20, 5, false);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_HORIZONTAL,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;
    int32_t press_x;
    int32_t press_y;

    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    press_x = state->hthumb_rect.x;
    press_y = state->hthumb_rect.y;

    route_mouse(canvas, press_x, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);

    route_mouse(canvas, press_x + 3, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG, N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_x(scroll) == 6);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll);

    route_mouse(canvas, press_x + 3, press_y, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE, N00B_MOD_NONE);
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);

    n00b_scroll_to_start(scroll);
    route_mouse(canvas,
                state->hscrollbar_rect.x + state->hthumb_rect.width + 2,
                state->hscrollbar_rect.y,
                N00B_MOUSE_LEFT,
                N00B_MOUSE_PRESS,
                N00B_MOD_NONE);
    assert(n00b_scroll_get_offset_x(scroll) == 10);

    assert(n00b_canvas_remove_plane(canvas, scroll));
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll horizontal thumb drag and track click\n");
}

static void
test_scroll_scrolled_content_mouse_routes_to_visible_child(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *clickable = make_dummy_child(4, 2, 4, 2, true);
    n00b_plane_t *content = make_anchored_container(clickable, 8, 20, 2, 14, 4, 2);
    n00b_plane_t *scroll = n00b_scroll_new(content,
                                           .axes = N00B_SCROLL_AXIS_VERTICAL,
                                           .scrollbar_mode = N00B_SCROLLBAR_NEVER,
                                           .canvas = canvas);

    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 10,
                           .height = 5,
                       });

    n00b_scroll_to(scroll, 0, 12);
    route_mouse(canvas, 2, 2, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS, N00B_MOD_NONE);
    assert(dummy_state(clickable)->click_count == 1);

    assert(n00b_canvas_remove_plane(canvas, scroll));
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll scrolled content mouse routes to visible child\n");
}

static void
test_scroll_wrapped_text_uses_viewport_width_for_content_height(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *text = n00b_text_new(
        n00b_string_from_cstr("aa bb cc dd ee ff gg hh ii jj kk ll"),
        .wrap = true,
        .canvas = canvas);
    n00b_plane_t *scroll = n00b_scroll_new(text,
                                           .axes = N00B_SCROLL_AXIS_VERTICAL,
                                           .scrollbar_mode = N00B_SCROLLBAR_AUTO,
                                           .canvas = canvas);
    n00b_scroll_t *state = (n00b_scroll_t *)scroll->widget_data;

    n00b_canvas_add_plane(canvas, scroll);
    n00b_widget_layout(scroll,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 4,
                           .height = 3,
                       });

    assert(n00b_text_get_wrapped_line_count(text) > state->viewport_height);
    assert(state->content_height > state->viewport_height);
    assert(n00b_scroll_can_scroll_down(scroll));

    n00b_scroll_to_bottom(scroll);
    assert(n00b_scroll_get_offset_y(scroll) > 0);

    assert(n00b_canvas_remove_plane(canvas, scroll));
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] scroll wrapped text uses viewport width for content height\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running scroll widget tests...\n");

    test_scroll_create_and_api();
    test_scroll_measure_caps_scroll_axes();
    test_scroll_plain_plane_overflow_preserves_box_size();
    test_scroll_offsets_clamp_and_direction_helpers();
    test_scroll_non_scroll_axis_uses_resolved_viewport();
    test_scroll_auto_scrollbars_and_thumb_rects();
    test_scroll_content_click_focuses_scroll_for_keyboard_input();
    test_scroll_keyboard_scroll_events();
    test_scroll_vertical_thumb_drag_and_track_click();
    test_scroll_detach_during_thumb_drag_clears_capture();
    test_scroll_horizontal_thumb_drag_and_track_click();
    test_scroll_scrolled_content_mouse_routes_to_visible_child();
    test_scroll_wrapped_text_uses_viewport_width_for_content_height();

    printf("All scroll widget tests passed.\n");
    n00b_shutdown();
    return 0;
}
