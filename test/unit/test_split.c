/*
 * Unit tests for the split widget.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/box.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/split.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

typedef struct {
    int32_t     pref_w;
    int32_t     pref_h;
    int32_t     min_w;
    int32_t     min_h;
    bool        consume_click;
    int         click_count;
    n00b_rect_t last_bounds;
} dummy_widget_t;

typedef struct {
    int   calls;
    float last_ratio;
} split_change_state_t;

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
    (void)data;

    return false;
}

static void
dummy_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    (void)plane;

    dummy_widget_t *dummy = data;
    if (dummy) {
        dummy->last_bounds = bounds;
    }
}

static const n00b_widget_vtable_t dummy_widget = {
    .kind         = "split_test_dummy",
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
        .consume_click = consume_click,
    };

    plane->width  = dummy->pref_w;
    plane->height = dummy->pref_h;
    n00b_widget_attach(plane, &dummy_widget, dummy);

    return plane;
}

static void
plain_child_set_box(n00b_plane_t *plane)
{
    assert(plane != nullptr);
    plane->box = n00b_box_props_new(.borders = N00B_BORDER_ALL);
}

static n00b_plane_t *
make_plain_child(int32_t width, int32_t height, bool with_box)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);

    plane->width  = width > 0 ? width : 1;
    plane->height = height > 0 ? height : 1;

    if (with_box) {
        plain_child_set_box(plane);
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
            n00b_mouse_action_t    action)
{
    n00b_event_t event = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = x,
            .y      = y,
            .button = button,
            .action = action,
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
assert_ratio_near(float actual, float expected)
{
    if (!(actual > expected - 0.0001f && actual < expected + 0.0001f)) {
        fprintf(stderr, "ratio mismatch: actual=%0.6f expected=%0.6f\n",
                (double)actual,
                (double)expected);
        assert(false);
    }
}

static void
on_split_change(n00b_plane_t *split, float ratio, void *data)
{
    (void)split;

    split_change_state_t *state = data;
    assert(state != nullptr);
    state->calls++;
    state->last_ratio = ratio;
}

static void
test_split_create_and_api(void)
{
    split_change_state_t change = {};
    n00b_plane_t *first = make_dummy_child(18, 6, 5, 3, false);
    n00b_plane_t *second = make_dummy_child(24, 8, 7, 4, false);
    n00b_plane_t *replacement = make_dummy_child(30, 9, 8, 4, false);
    n00b_plane_t *split = n00b_split_new(first,
                                         second,
                                         .on_change = on_split_change,
                                         .on_change_data = &change);
    n00b_split_t *state = (n00b_split_t *)split->widget_data;

    assert(split != nullptr);
    assert(split->widget_vtable == &n00b_widget_split);
    assert(!n00b_widget_can_focus(split));
    assert(state != nullptr);
    assert(state->first == first);
    assert(state->second == second);
    assert(first->parent == split);
    assert(second->parent == split);
    assert(split->children.len == 2);
    assert(split->children.data[0] == first);
    assert(split->children.data[1] == second);
    assert_ratio_near(n00b_split_get_ratio(split), 0.5f);
    assert(state->divider_px == 1);

    n00b_split_set_ratio(split, 0.7f);
    assert_ratio_near(n00b_split_get_ratio(split), 0.7f);
    assert(change.calls == 1);
    assert_ratio_near(change.last_ratio, 0.7f);

    n00b_split_set_divider_size(split, 9);
    assert(state->divider_px == 9);

    n00b_split_set_first(split, replacement);
    assert(first->parent == nullptr);
    assert(first->widget_vtable == &dummy_widget);
    assert(state->first == replacement);
    assert(split->children.len == 2);
    assert(split->children.data[0] == replacement);
    assert(split->children.data[1] == second);

    n00b_split_set_second(split, nullptr);
    assert(second->parent == nullptr);
    assert(state->second == nullptr);
    assert(split->children.len == 1);
    assert(split->children.data[0] == replacement);

    destroy_plane_tree(first);
    destroy_plane_tree(second);
    destroy_plane_tree(split);
    printf("  [PASS] split create and api\n");
}

static void
test_split_measure_by_orientation(void)
{
    n00b_plane_t *h_first = make_dummy_child(30, 12, 10, 5, false);
    n00b_plane_t *h_second = make_dummy_child(20, 18, 8, 7, false);
    n00b_plane_t *h_split = n00b_split_new(h_first,
                                           h_second,
                                           .min_first_px = 0,
                                           .min_second_px = 0,
                                           .divider_px = 4);
    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w = 0;
    int32_t min_h = 0;

    n00b_widget_measure(h_split, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 54);
    assert(pref_h == 18);
    assert(min_w == 22);
    assert(min_h == 7);

    n00b_plane_set_visible(h_second, false);
    n00b_widget_measure(h_split, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 30);
    assert(pref_h == 12);
    assert(min_w == 10);
    assert(min_h == 5);

    destroy_plane_tree(h_split);

    n00b_plane_t *v_first = make_dummy_child(30, 12, 10, 5, false);
    n00b_plane_t *v_second = make_dummy_child(20, 18, 8, 7, false);
    n00b_plane_t *v_split = n00b_split_new(v_first,
                                           v_second,
                                           .min_first_px = 0,
                                           .min_second_px = 0,
                                           .orientation = N00B_SPLIT_VERTICAL,
                                           .divider_px = 6);

    n00b_widget_measure(v_split, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 30);
    assert(pref_h == 36);
    assert(min_w == 10);
    assert(min_h == 18);

    destroy_plane_tree(v_split);

    n00b_plane_t *empty = n00b_split_new(nullptr, nullptr);
    n00b_widget_measure(empty, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 1);
    assert(pref_h == 1);
    assert(min_w == 1);
    assert(min_h == 1);

    destroy_plane_tree(empty);
    printf("  [PASS] split measure by orientation\n");
}

static void
test_split_measure_honors_configured_minimums(void)
{
    n00b_plane_t *first = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t *second = make_dummy_child(22, 9, 8, 5, false);
    n00b_plane_t *split = n00b_split_new(first,
                                         second,
                                         .min_first_px = 40,
                                         .min_second_px = 30,
                                         .divider_px = 6);
    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w = 0;
    int32_t min_h = 0;

    n00b_widget_measure(split, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 76);
    assert(pref_h == 9);
    assert(min_w == 76);
    assert(min_h == 5);

    destroy_plane_tree(split);
    printf("  [PASS] split measure honors configured minimums\n");
}

static void
test_split_measure_plain_planes(void)
{
    n00b_plane_t *first = make_plain_child(15, 6, true);
    n00b_plane_t *second = make_plain_child(9, 12, false);
    n00b_plane_t *split = n00b_split_new(first,
                                         second,
                                         .min_first_px = 0,
                                         .min_second_px = 0,
                                         .divider_px = 4);
    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w = 0;
    int32_t min_h = 0;

    n00b_widget_measure(split, &pref_w, &pref_h, &min_w, &min_h);
    assert(pref_w == 30);
    assert(pref_h == 12);
    assert(min_w == 30);
    assert(min_h == 12);

    destroy_plane_tree(split);
    printf("  [PASS] split measure plain planes\n");
}

static void
test_split_layout_horizontal_clamps_minimums(void)
{
    n00b_plane_t *first = make_dummy_child(20, 8, 5, 3, false);
    n00b_plane_t *second = make_dummy_child(20, 8, 5, 3, false);
    n00b_plane_t *split = n00b_split_new(first,
                                         second,
                                         .ratio = 0.8f,
                                         .min_first_px = 40,
                                         .min_second_px = 30,
                                         .divider_px = 10);
    n00b_split_t *state = (n00b_split_t *)split->widget_data;

    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 5,
                           .y      = 7,
                           .width  = 100,
                           .height = 24,
                       });

    assert_rect_eq(dummy_state(first)->last_bounds, 5, 7, 60, 24);
    assert_rect_eq(dummy_state(second)->last_bounds, 75, 7, 30, 24);
    assert_rect_eq(state->divider_rect, 60, 0, 10, 24);

    destroy_plane_tree(split);
    printf("  [PASS] split layout horizontal clamps minimums\n");
}

static void
test_split_single_visible_pane_uses_full_bounds(void)
{
    n00b_plane_t *first = make_dummy_child(20, 8, 5, 3, false);
    n00b_plane_t *second = make_dummy_child(18, 10, 5, 3, false);
    n00b_plane_t *split = n00b_split_new(first, second, .divider_px = 8);
    n00b_split_t *state = (n00b_split_t *)split->widget_data;

    n00b_plane_set_visible(first, false);
    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 2,
                           .y      = 3,
                           .width  = 50,
                           .height = 12,
                       });

    assert_rect_eq(dummy_state(second)->last_bounds, 2, 3, 50, 12);
    assert_rect_eq(state->divider_rect, 0, 0, 0, 0);

    destroy_plane_tree(split);
    printf("  [PASS] split single visible pane uses full bounds\n");
}

static void
test_split_drag_updates_ratio_and_capture(void)
{
    split_change_state_t change = {};
    n00b_canvas_t *canvas = make_stream_canvas(40, 120);
    n00b_plane_t  *first = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *second = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *split = n00b_split_new(first,
                                          second,
                                          .ratio = 0.5f,
                                          .min_first_px = 10,
                                          .min_second_px = 10,
                                          .divider_px = 6,
                                          .on_change = on_split_change,
                                          .on_change_data = &change,
                                          .canvas = canvas);
    n00b_split_t  *state = (n00b_split_t *)split->widget_data;
    int32_t        release_x;

    n00b_canvas_add_plane(canvas, split);
    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 120,
                           .height = 40,
                       });

    route_mouse(canvas, 59, 10, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_ACTIVE);

    route_mouse(canvas, 86, 10, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(change.calls == 1);
    assert_ratio_near(change.last_ratio, 84.0f / 114.0f);
    assert_ratio_near(n00b_split_get_ratio(split), 84.0f / 114.0f);
    assert(state->divider_rect.x == 84);

    release_x = state->divider_rect.x + (state->divider_rect.width / 2);
    route_mouse(canvas, release_x, 10, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE);
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_HOVER);

    assert(n00b_canvas_remove_plane(canvas, split));
    destroy_plane_tree(split);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] split drag updates ratio and capture\n");
}

static void
test_split_vertical_drag_updates_ratio_and_capture(void)
{
    split_change_state_t change = {};
    n00b_canvas_t *canvas = make_stream_canvas(80, 100);
    n00b_plane_t  *first = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *second = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *split = n00b_split_new(first,
                                          second,
                                          .orientation = N00B_SPLIT_VERTICAL,
                                          .ratio = 0.5f,
                                          .min_first_px = 10,
                                          .min_second_px = 10,
                                          .divider_px = 6,
                                          .on_change = on_split_change,
                                          .on_change_data = &change,
                                          .canvas = canvas);
    n00b_split_t  *state = (n00b_split_t *)split->widget_data;
    int32_t        release_y;

    n00b_canvas_add_plane(canvas, split);
    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 100,
                           .height = 80,
                       });

    route_mouse(canvas, 20, 39, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_ACTIVE);

    route_mouse(canvas, 20, 61, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(change.calls == 1);
    assert_ratio_near(change.last_ratio, 59.0f / 74.0f);
    assert_ratio_near(n00b_split_get_ratio(split), 59.0f / 74.0f);
    assert(state->divider_rect.y == 59);

    release_y = state->divider_rect.y + (state->divider_rect.height / 2);
    route_mouse(canvas, 20, release_y, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE);
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_HOVER);

    assert(n00b_canvas_remove_plane(canvas, split));
    destroy_plane_tree(split);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] split vertical drag updates ratio and capture\n");
}

static void
test_split_detach_during_drag_clears_capture(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(40, 160);
    n00b_plane_t  *first = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *second = make_dummy_child(20, 8, 10, 4, false);
    n00b_plane_t  *split = n00b_split_new(first,
                                          second,
                                          .ratio = 0.5f,
                                          .min_first_px = 10,
                                          .min_second_px = 10,
                                          .divider_px = 6,
                                          .canvas = canvas);
    n00b_split_t  *split_state = (n00b_split_t *)split->widget_data;
    n00b_plane_t  *root = n00b_new_kargs(n00b_plane_t, plane, .canvas = canvas);
    n00b_plane_t  *host = n00b_new_kargs(n00b_plane_t, plane, .canvas = canvas);

    root->width  = 160;
    root->height = 80;
    host->width  = 160;
    host->height = 80;

    n00b_canvas_add_plane(canvas, split);
    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 120,
                           .height = 40,
                       });

    route_mouse(canvas, 59, 10, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(split_state->dragging);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_ACTIVE);

    assert(n00b_canvas_remove_plane(canvas, split));
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(!split_state->dragging);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_NORMAL);

    n00b_canvas_add_plane(canvas, root);
    n00b_plane_add_child(root, host, 0, 0);
    n00b_plane_add_child(host, split, 0, 0);
    n00b_widget_layout(split,
                       (n00b_rect_t){
                           .x      = 12,
                           .y      = 6,
                           .width  = 120,
                           .height = 40,
                       });

    route_mouse(canvas, 71, 16, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    assert(n00b_canvas_get_mouse_capture(canvas) == split);
    assert(split_state->dragging);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_ACTIVE);

    assert(n00b_plane_remove_child(root, host));
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);
    assert(!split_state->dragging);
    assert(n00b_plane_get_state(split) == N00B_WSTATE_NORMAL);

    destroy_plane_tree(host);
    assert(n00b_canvas_remove_plane(canvas, root));
    destroy_plane_tree(root);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] split detach during drag clears capture\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running split widget tests...\n");

    test_split_create_and_api();
    test_split_measure_by_orientation();
    test_split_measure_honors_configured_minimums();
    test_split_measure_plain_planes();
    test_split_layout_horizontal_clamps_minimums();
    test_split_single_visible_pane_uses_full_bounds();
    test_split_drag_updates_ratio_and_capture();
    test_split_vertical_drag_updates_ratio_and_capture();
    test_split_detach_during_drag_clears_capture();

    printf("All split widget tests passed.\n");
    n00b_shutdown();
    return 0;
}
