/*
 * Unit tests for the tabs widget.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/scroll.h"
#include "display/widgets/tabs.h"
#include "internal/display/widget_primitives.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                         n00b_isize_t rows,
                                         n00b_isize_t cols);

typedef struct {
    int32_t     pref_w;
    int32_t     pref_h;
    int32_t     min_w;
    int32_t     min_h;
    bool        consume_click;
    bool        can_focus;
    int         click_count;
    int         layout_calls;
    int         persistent_value;
    n00b_rect_t last_bounds;
} dummy_widget_t;

typedef struct {
    int count;
    int last_new_index;
    int last_old_index;
} tabs_callback_state_t;

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
        dummy->persistent_value++;
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
    if (!dummy) {
        return;
    }

    dummy->layout_calls++;
    dummy->last_bounds = bounds;
}

static const n00b_widget_vtable_t dummy_widget = {
    .kind         = "tabs_test_dummy",
    .destroy      = dummy_destroy,
    .render       = dummy_render,
    .measure      = dummy_measure,
    .handle_event = dummy_handle_event,
    .can_focus    = dummy_can_focus,
    .layout       = dummy_layout,
};

static dummy_widget_t *
dummy_state(n00b_plane_t *plane)
{
    assert(plane != nullptr);
    assert(plane->widget_vtable == &dummy_widget);
    return (dummy_widget_t *)plane->widget_data;
}

static n00b_tabs_t *
tabs_state(n00b_plane_t *plane)
{
    assert(plane != nullptr);
    assert(plane->widget_vtable == &n00b_widget_tabs);
    return (n00b_tabs_t *)plane->widget_data;
}

static n00b_plane_t *
make_dummy_widget(int32_t pref_w, int32_t pref_h,
                  int32_t min_w, int32_t min_h,
                  bool consume_click,
                  bool can_focus)
{
    n00b_plane_t   *plane = n00b_new_kargs(n00b_plane_t, plane);
    dummy_widget_t *dummy = n00b_alloc(dummy_widget_t);

    *dummy = (dummy_widget_t){
        .pref_w        = pref_w > 0 ? pref_w : 1,
        .pref_h        = pref_h > 0 ? pref_h : 1,
        .min_w         = min_w > 0 ? min_w : 1,
        .min_h         = min_h > 0 ? min_h : 1,
        .consume_click = consume_click,
        .can_focus     = can_focus,
    };

    plane->width  = dummy->pref_w;
    plane->height = dummy->pref_h;
    n00b_widget_attach(plane, &dummy_widget, dummy);

    return plane;
}

static n00b_plane_t *
make_plain_plane(int32_t width, int32_t height)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);

    plane->width = width > 0 ? width : 1;
    plane->height = height > 0 ? height : 1;

    return plane;
}

static void
on_tabs_select(n00b_plane_t *tabs, int new_index, int old_index, void *data)
{
    (void)tabs;

    tabs_callback_state_t *state = data;
    if (!state) {
        return;
    }

    state->count++;
    state->last_new_index = new_index;
    state->last_old_index = old_index;
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
layout_to_canvas(n00b_canvas_t *canvas, n00b_plane_t *plane)
{
    n00b_widget_layout(plane,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = (int32_t)canvas->frame_cols,
                           .height = (int32_t)canvas->frame_rows,
                       });
}

static void
send_left_press(n00b_canvas_t *canvas, int32_t x, int32_t y)
{
    n00b_event_t event = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = x,
            .y      = y,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods   = N00B_MOD_NONE,
        },
    };

    n00b_mouse_route_event(canvas, nullptr, &event);
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
test_tabs_create_and_api(void)
{
    n00b_plane_t *tabs   = n00b_tabs_new();
    n00b_plane_t *wrong  = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *page_a = make_dummy_widget(8, 3, 3, 1, true, false);
    n00b_plane_t *page_b = make_dummy_widget(10, 4, 4, 2, false, false);
    n00b_tab_entry_t *entry;

    assert(tabs != nullptr);
    assert(tabs->widget_vtable == &n00b_widget_tabs);
    assert(n00b_widget_can_focus(tabs));
    assert(n00b_tabs_selected_index(tabs) == -1);
    assert(n00b_tabs_count(tabs) == 0);
    assert(n00b_tabs_get(tabs, 0) == nullptr);
    assert(!n00b_tabs_remove(tabs, 0));
    assert(!n00b_tabs_next(tabs));
    assert(!n00b_tabs_prev(tabs));
    assert(n00b_tabs_add(tabs, nullptr, page_a) == -1);
    assert(page_a->parent == nullptr);
    assert(n00b_tabs_selected_index(wrong) == -1);

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Overview"), page_a) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Details"), page_b) == 1);
    assert(n00b_tabs_count(tabs) == 2);
    assert(n00b_tabs_selected_index(tabs) == 0);
    assert(page_a->parent == tabs);
    assert(page_b->parent == tabs);
    assert((page_a->flags & N00B_PLANE_VISIBLE) != 0);
    assert((page_b->flags & N00B_PLANE_VISIBLE) == 0);

    entry = n00b_tabs_get(tabs, 0);
    assert(entry != nullptr);
    assert(entry->content == page_a);
    assert(entry->name != nullptr);

    n00b_plane_destroy(wrong);
    destroy_plane_tree(tabs);
    printf("  [PASS] tabs create and api\n");
}

static void
test_tabs_measure_uses_header_and_max_content(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 60);
    n00b_plane_t  *tabs   = n00b_tabs_new(.canvas = canvas);
    n00b_plane_t  *small  = make_dummy_widget(6, 2, 2, 1, false, false);
    n00b_plane_t  *large  = make_dummy_widget(18, 7, 9, 4, false, false);
    int32_t        pref_w = 0;
    int32_t        pref_h = 0;
    int32_t        min_w  = 0;
    int32_t        min_h  = 0;
    int32_t        header_h;
    int32_t        sep_w;
    int32_t        header_total_w;

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("One"), small) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Mega"), large) == 1);

    n00b_widget_measure(tabs, &pref_w, &pref_h, &min_w, &min_h);

    header_h = n00b_widget_line_px_height(tabs);
    sep_w = n00b_plane_text_width(tabs, n00b_string_from_cstr(" | "), nullptr);
    header_total_w = n00b_plane_text_width(tabs, n00b_string_from_cstr("One"), nullptr)
                   + n00b_plane_text_width(tabs, n00b_string_from_cstr("Mega"), nullptr)
                   + sep_w;

    assert(pref_w == n00b_max(header_total_w, 18));
    assert(pref_h == header_h + 7);
    assert(min_w == 9);
    assert(min_h == header_h + 4);
    assert(pref_h > header_h + 2);

    destroy_plane_tree(tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs measure uses header and max content\n");
}

static void
test_tabs_measure_plain_plane_pages(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 60);
    n00b_plane_t  *tabs   = n00b_tabs_new(.canvas = canvas);
    n00b_plane_t  *plain  = make_plain_plane(14, 6);
    int32_t        pref_w = 0;
    int32_t        pref_h = 0;
    int32_t        min_w  = 0;
    int32_t        min_h  = 0;
    int32_t        header_h;
    int32_t        header_w;

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Plain"), plain) == 0);

    n00b_widget_measure(tabs, &pref_w, &pref_h, &min_w, &min_h);

    header_h = n00b_widget_line_px_height(tabs);
    header_w = n00b_plane_text_width(tabs, n00b_string_from_cstr("Plain"), nullptr);

    assert(pref_w == n00b_max(header_w, 14));
    assert(pref_h == header_h + 6);
    assert(min_w == 14);
    assert(min_h == header_h + 6);

    destroy_plane_tree(tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs measure plain plane pages\n");
}

static void
test_tabs_layout_top_and_bottom_headers(void)
{
    n00b_canvas_t *canvas      = make_stream_canvas(24, 80);
    n00b_plane_t  *top_tabs    = n00b_tabs_new(.canvas = canvas, .position = N00B_TABS_TOP);
    n00b_plane_t  *bottom_tabs = n00b_tabs_new(.canvas = canvas, .position = N00B_TABS_BOTTOM);
    n00b_plane_t  *top_a       = make_dummy_widget(10, 4, 2, 1, false, false);
    n00b_plane_t  *top_b       = make_dummy_widget(8, 3, 2, 1, false, false);
    n00b_plane_t  *bottom_a    = make_dummy_widget(7, 5, 2, 1, false, false);
    n00b_plane_t  *bottom_b    = make_dummy_widget(9, 2, 2, 1, false, false);
    n00b_rect_t    bounds      = {
        .x      = 5,
        .y      = 7,
        .width  = 40,
        .height = 12,
    };
    int32_t        header_h;

    assert(n00b_tabs_add(top_tabs, n00b_string_from_cstr("Alpha"), top_a) == 0);
    assert(n00b_tabs_add(top_tabs, n00b_string_from_cstr("Beta"), top_b) == 1);
    assert(n00b_tabs_add(bottom_tabs, n00b_string_from_cstr("Alpha"), bottom_a) == 0);
    assert(n00b_tabs_add(bottom_tabs, n00b_string_from_cstr("Beta"), bottom_b) == 1);

    n00b_widget_layout(top_tabs, bounds);
    n00b_widget_layout(bottom_tabs, bounds);

    header_h = n00b_widget_line_px_height(top_tabs);

    assert_rect_eq(dummy_state(top_a)->last_bounds,
                   bounds.x,
                   bounds.y + header_h,
                   bounds.width,
                   bounds.height - header_h);
    assert_rect_eq(dummy_state(top_b)->last_bounds,
                   bounds.x,
                   bounds.y + header_h,
                   bounds.width,
                   bounds.height - header_h);
    assert((top_a->flags & N00B_PLANE_VISIBLE) != 0);
    assert((top_b->flags & N00B_PLANE_VISIBLE) == 0);
    assert(tabs_state(top_tabs)->header_rects[0].y == 0);
    assert(tabs_state(top_tabs)->header_rects[1].y == 0);

    assert_rect_eq(dummy_state(bottom_a)->last_bounds,
                   bounds.x,
                   bounds.y,
                   bounds.width,
                   bounds.height - header_h);
    assert_rect_eq(dummy_state(bottom_b)->last_bounds,
                   bounds.x,
                   bounds.y,
                   bounds.width,
                   bounds.height - header_h);
    assert((bottom_a->flags & N00B_PLANE_VISIBLE) != 0);
    assert((bottom_b->flags & N00B_PLANE_VISIBLE) == 0);
    assert(tabs_state(bottom_tabs)->header_rects[0].y == bounds.height - header_h);
    assert(tabs_state(bottom_tabs)->header_rects[1].y == bounds.height - header_h);

    destroy_plane_tree(top_tabs);
    destroy_plane_tree(bottom_tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs layout top and bottom headers\n");
}

static void
test_tabs_keyboard_navigation_wraps_and_fires_callback(void)
{
    tabs_callback_state_t callback_state = { 0 };
    n00b_plane_t         *tabs = n00b_tabs_new(.on_select = on_tabs_select,
                                               .on_select_data = &callback_state);
    n00b_event_t          right = {
        .type = N00B_EVENT_KEY,
        .key  = {
            .key  = N00B_KEY_RIGHT,
            .mods = N00B_MOD_NONE,
        },
    };
    n00b_event_t          left = {
        .type = N00B_EVENT_KEY,
        .key  = {
            .key  = N00B_KEY_LEFT,
            .mods = N00B_MOD_NONE,
        },
    };

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("One"), make_dummy_widget(4, 2, 1, 1, false, false)) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Two"), make_dummy_widget(5, 2, 1, 1, false, false)) == 1);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Three"), make_dummy_widget(6, 2, 1, 1, false, false)) == 2);

    assert(n00b_widget_handle_event(tabs, &right));
    assert(n00b_tabs_selected_index(tabs) == 1);
    assert(callback_state.count == 1);
    assert(callback_state.last_old_index == 0);
    assert(callback_state.last_new_index == 1);
    assert((tabs_state(tabs)->entries[1].content->flags & N00B_PLANE_VISIBLE) != 0);
    assert((tabs_state(tabs)->entries[0].content->flags & N00B_PLANE_VISIBLE) == 0);

    assert(n00b_widget_handle_event(tabs, &right));
    assert(n00b_tabs_selected_index(tabs) == 2);
    assert(callback_state.count == 2);

    assert(n00b_widget_handle_event(tabs, &right));
    assert(n00b_tabs_selected_index(tabs) == 0);
    assert(callback_state.count == 3);
    assert(callback_state.last_old_index == 2);
    assert(callback_state.last_new_index == 0);

    assert(n00b_widget_handle_event(tabs, &left));
    assert(n00b_tabs_selected_index(tabs) == 2);
    assert(callback_state.count == 4);
    assert(callback_state.last_old_index == 0);
    assert(callback_state.last_new_index == 2);

    destroy_plane_tree(tabs);
    printf("  [PASS] tabs keyboard navigation wraps and fires callback\n");
}

static void
test_tabs_mouse_header_click_selects_page(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(18, 60);
    n00b_plane_t  *tabs   = n00b_tabs_new(.canvas = canvas);
    n00b_tabs_t   *state;
    int32_t        separator_x;

    n00b_canvas_add_plane(canvas, tabs);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Alpha"), make_dummy_widget(7, 2, 1, 1, false, false)) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Beta"), make_dummy_widget(8, 2, 1, 1, false, false)) == 1);

    layout_to_canvas(canvas, tabs);
    state = tabs_state(tabs);

    separator_x = state->header_rects[0].x + state->header_rects[0].width;
    if (state->header_rects[1].x > separator_x) {
        send_left_press(canvas, separator_x, state->header_rects[0].y);
        assert(n00b_tabs_selected_index(tabs) == 0);
    }

    send_left_press(canvas,
                    state->header_rects[1].x,
                    state->header_rects[1].y);
    assert(n00b_tabs_selected_index(tabs) == 1);

    assert(n00b_canvas_remove_plane(canvas, tabs));
    destroy_plane_tree(tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs mouse header click selects page\n");
}

static void
test_tabs_switch_preserves_content_planes_and_state(void)
{
    n00b_plane_t *tabs   = n00b_tabs_new();
    n00b_plane_t *page_a = make_dummy_widget(8, 3, 2, 1, true, false);
    n00b_plane_t *page_b = make_dummy_widget(8, 3, 2, 1, true, false);
    n00b_event_t  click  = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = 1,
            .y      = 1,
            .button = N00B_MOUSE_LEFT,
            .action = N00B_MOUSE_PRESS,
            .mods   = N00B_MOD_NONE,
        },
    };

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("First"), page_a) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Second"), page_b) == 1);

    assert(n00b_widget_handle_event(page_a, &click));
    assert(dummy_state(page_a)->click_count == 1);
    assert(dummy_state(page_a)->persistent_value == 1);

    for (int i = 0; i < 5; i++) {
        assert(n00b_tabs_select_index(tabs, 1));
        assert(n00b_tabs_select_index(tabs, 0));
    }

    assert(n00b_tabs_get(tabs, 0)->content == page_a);
    assert(n00b_tabs_get(tabs, 1)->content == page_b);
    assert(page_a->parent == tabs);
    assert(page_b->parent == tabs);
    assert(dummy_state(page_a)->click_count == 1);
    assert(dummy_state(page_a)->persistent_value == 1);
    assert((page_a->flags & N00B_PLANE_VISIBLE) != 0);
    assert((page_b->flags & N00B_PLANE_VISIBLE) == 0);

    destroy_plane_tree(tabs);
    printf("  [PASS] tabs switch preserves content planes and state\n");
}

static void
test_tabs_focus_returns_to_header_when_hidden_page_was_focused(void)
{
    n00b_canvas_t     *canvas      = make_stream_canvas(20, 60);
    n00b_focus_mgr_t  *fm;
    n00b_plane_t      *tabs        = n00b_tabs_new(.canvas = canvas);
    n00b_plane_t      *page_a      = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t      *page_b      = make_dummy_widget(10, 4, 2, 1, false, false);
    n00b_plane_t      *focused_box = make_dummy_widget(5, 2, 1, 1, false, true);

    n00b_plane_add_child(page_a, focused_box, 1, 1);

    n00b_canvas_add_plane(canvas, tabs);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Page A"), page_a) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Page B"), page_b) == 1);
    layout_to_canvas(canvas, tabs);

    fm = n00b_focus_mgr_new(canvas);
    assert(canvas->focus == fm);
    assert(n00b_focus_mgr_set(fm, focused_box));
    assert(n00b_focus_mgr_current(fm) == focused_box);

    assert(n00b_tabs_select_index(tabs, 1));
    assert(n00b_focus_mgr_current(fm) == tabs);
    assert(n00b_plane_get_state(focused_box) == N00B_WSTATE_NORMAL);

    n00b_focus_mgr_destroy(fm);
    assert(n00b_canvas_remove_plane(canvas, tabs));
    destroy_plane_tree(tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs focus returns to header when hidden page was focused\n");
}

static void
test_tabs_switch_clears_hidden_page_capture(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 80);
    n00b_plane_t *scroll_content = make_dummy_widget(12, 24, 12, 24, false, false);
    n00b_plane_t *scroll_page = n00b_scroll_new(scroll_content,
                                                .axes = N00B_SCROLL_AXIS_VERTICAL,
                                                .canvas = canvas);
    n00b_plane_t *page_b = make_dummy_widget(10, 4, 2, 1, false, false);
    n00b_plane_t *tabs = n00b_tabs_new(.canvas = canvas);
    n00b_scroll_t *scroll_state = (n00b_scroll_t *)scroll_page->widget_data;
    int32_t press_x;
    int32_t press_y;

    n00b_canvas_add_plane(canvas, tabs);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Scroll"), scroll_page) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("Other"), page_b) == 1);

    layout_to_canvas(canvas, tabs);

    assert(scroll_state->vthumb_rect.width > 0);
    assert(scroll_state->vthumb_rect.height > 0);
    press_x = scroll_page->bounds.x + scroll_state->vthumb_rect.x;
    press_y = scroll_page->bounds.y + scroll_state->vthumb_rect.y;

    send_left_press(canvas, press_x, press_y);
    assert(n00b_canvas_get_mouse_capture(canvas) == scroll_page);
    assert(scroll_state->dragging_vertical_thumb);

    assert(n00b_tabs_select_index(tabs, 1));
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(!scroll_state->dragging_vertical_thumb);
    assert((scroll_page->flags & N00B_PLANE_VISIBLE) == 0);

    assert(n00b_canvas_remove_plane(canvas, tabs));
    destroy_plane_tree(tabs);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] tabs switch clears hidden page capture\n");
}

static void
test_tabs_remove_selected_uses_same_slot_then_previous(void)
{
    n00b_plane_t *tabs   = n00b_tabs_new();
    n00b_plane_t *page_a = make_dummy_widget(4, 2, 1, 1, false, false);
    n00b_plane_t *page_b = make_dummy_widget(4, 2, 1, 1, false, false);
    n00b_plane_t *page_c = make_dummy_widget(4, 2, 1, 1, false, false);
    n00b_plane_t *page_d = make_dummy_widget(4, 2, 1, 1, false, false);

    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("A"), page_a) == 0);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("B"), page_b) == 1);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("C"), page_c) == 2);
    assert(n00b_tabs_add(tabs, n00b_string_from_cstr("D"), page_d) == 3);

    assert(n00b_tabs_select_index(tabs, 1));
    assert(n00b_tabs_remove(tabs, 1));
    assert(n00b_tabs_selected_index(tabs) == 1);
    assert(n00b_tabs_count(tabs) == 3);
    assert(n00b_tabs_get(tabs, 1)->content == page_c);
    assert(page_b->parent == nullptr);

    assert(n00b_tabs_select_index(tabs, 2));
    assert(n00b_tabs_remove(tabs, 2));
    assert(n00b_tabs_selected_index(tabs) == 1);
    assert(n00b_tabs_count(tabs) == 2);
    assert(n00b_tabs_get(tabs, 1)->content == page_c);
    assert(page_d->parent == nullptr);

    destroy_plane_tree(tabs);
    destroy_plane_tree(page_b);
    destroy_plane_tree(page_d);
    printf("  [PASS] tabs remove selected uses same slot then previous\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running tabs widget tests...\n");

    test_tabs_create_and_api();
    test_tabs_measure_uses_header_and_max_content();
    test_tabs_measure_plain_plane_pages();
    test_tabs_layout_top_and_bottom_headers();
    test_tabs_keyboard_navigation_wraps_and_fires_callback();
    test_tabs_mouse_header_click_selects_page();
    test_tabs_switch_preserves_content_planes_and_state();
    test_tabs_focus_returns_to_header_when_hidden_page_was_focused();
    test_tabs_switch_clears_hidden_page_capture();
    test_tabs_remove_selected_uses_same_slot_then_previous();

    printf("All tabs widget tests passed.\n");
    n00b_shutdown();
    return 0;
}
