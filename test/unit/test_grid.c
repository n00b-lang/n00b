/*
 * Unit tests for the grid widget.
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
#include "display/widgets/box.h"
#include "display/widgets/button.h"
#include "display/widgets/grid.h"
#include "display/widgets/label.h"
#include "internal/display/event_dispatch.h"
#include "internal/display/scene_contracts.h"
#include "text/strings/string_ops.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);
extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);

typedef struct {
    int32_t     pref_w;
    int32_t     pref_h;
    int32_t     min_w;
    int32_t     min_h;
    bool        consume_click;
    int         click_count;
    n00b_rect_t last_bounds;
} dummy_widget_t;

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
    .kind         = "grid_test_dummy",
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

static bool
rectangles_overlap(n00b_rect_t a, n00b_rect_t b)
{
    return a.x < (b.x + b.width)
        && b.x < (a.x + a.width)
        && a.y < (b.y + b.height)
        && b.y < (a.y + a.height);
}

static void
test_grid_create_and_api(void)
{
    n00b_plane_t *grid = n00b_grid_new(.columns = 3, .gap = 4);
    n00b_grid_t  *state = (n00b_grid_t *)grid->widget_data;
    n00b_plane_t *child = make_dummy_child(10, 6, 4, 2, false);
    int32_t       col_span = 0;
    int32_t       row_span = 0;

    assert(grid != nullptr);
    assert(grid->widget_vtable == &n00b_widget_grid);
    assert(!n00b_widget_can_focus(grid));
    assert(state != nullptr);
    assert(state->columns == 3);
    assert(state->row_gap == 4);
    assert(state->col_gap == 4);
    assert(state->track_count == 0);

    n00b_plane_add_child(grid, child, 0, 0);
    n00b_grid_get_span(grid, child, &col_span, &row_span);
    assert(col_span == 1);
    assert(row_span == 1);

    destroy_plane_tree(grid);
    printf("  [PASS] grid create and api\n");
}

static void
test_grid_fixed_columns_with_span_layout(void)
{
    n00b_plane_t *grid = n00b_grid_new(.columns = 3,
                                        .row_gap = 3,
                                        .col_gap = 2);
    n00b_plane_t *header = make_dummy_child(60, 30, 30, 10, false);
    n00b_plane_t *a = make_dummy_child(20, 15, 8, 8, false);
    n00b_plane_t *b = make_dummy_child(20, 20, 8, 8, false);
    n00b_plane_t *c = make_dummy_child(20, 18, 8, 8, false);

    n00b_plane_add_child(grid, header, 0, 0);
    n00b_plane_add_child(grid, a, 0, 0);
    n00b_plane_add_child(grid, b, 0, 0);
    n00b_plane_add_child(grid, c, 0, 0);
    n00b_grid_set_span(grid, header, 3, 1);

    n00b_widget_layout(grid,
                       (n00b_rect_t){
                           .x      = 10,
                           .y      = 20,
                           .width  = 302,
                           .height = 160,
                       });

    assert_rect_eq(dummy_state(header)->last_bounds, 10, 20, 302, 30);
    assert_rect_eq(dummy_state(a)->last_bounds, 10, 53, 100, 20);
    assert_rect_eq(dummy_state(b)->last_bounds, 112, 53, 99, 20);
    assert_rect_eq(dummy_state(c)->last_bounds, 213, 53, 99, 20);

    assert(!rectangles_overlap(dummy_state(header)->last_bounds,
                               dummy_state(a)->last_bounds));
    assert(!rectangles_overlap(dummy_state(a)->last_bounds,
                               dummy_state(b)->last_bounds));
    assert(!rectangles_overlap(dummy_state(b)->last_bounds,
                               dummy_state(c)->last_bounds));

    destroy_plane_tree(grid);
    printf("  [PASS] grid fixed columns with span layout\n");
}

static void
test_grid_fr_tracks_with_clamps(void)
{
    n00b_grid_track_t tracks[] = {
        {.type = N00B_GRID_SIZE_FIXED, .value = 80, .min_px = 0, .max_px = 0},
        {.type = N00B_GRID_SIZE_FR, .value = 1, .min_px = 90, .max_px = 0},
        {.type = N00B_GRID_SIZE_FR, .value = 2, .min_px = 0, .max_px = 150},
    };
    n00b_plane_t *grid = n00b_grid_new(.columns = 3, .col_gap = 5);
    n00b_plane_t *a = make_dummy_child(8, 12, 4, 6, false);
    n00b_plane_t *b = make_dummy_child(8, 12, 4, 6, false);
    n00b_plane_t *c = make_dummy_child(8, 12, 4, 6, false);

    n00b_grid_set_tracks(grid, tracks, 3);
    n00b_plane_add_child(grid, a, 0, 0);
    n00b_plane_add_child(grid, b, 0, 0);
    n00b_plane_add_child(grid, c, 0, 0);

    n00b_widget_layout(grid,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 342,
                           .height = 60,
                       });

    assert_rect_eq(dummy_state(a)->last_bounds, 0, 0, 80, 12);
    assert_rect_eq(dummy_state(b)->last_bounds, 85, 0, 90, 12);
    assert_rect_eq(dummy_state(c)->last_bounds, 180, 0, 150, 12);

    destroy_plane_tree(grid);
    printf("  [PASS] grid fr tracks with clamps\n");
}

static void
test_grid_auto_fit_wraps_children(void)
{
    n00b_plane_t *grid = n00b_grid_new(.columns = 1, .gap = 10);
    n00b_plane_t *children[5];

    for (int i = 0; i < 5; i++) {
        children[i] = make_dummy_child(40, 14, 10, 6, false);
        n00b_plane_add_child(grid, children[i], 0, 0);
    }

    n00b_grid_set_auto_fit(grid, 60, 90);
    n00b_widget_layout(grid,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 220,
                           .height = 80,
                       });

    assert_rect_eq(dummy_state(children[0])->last_bounds, 0, 0, 67, 14);
    assert_rect_eq(dummy_state(children[1])->last_bounds, 77, 0, 67, 14);
    assert_rect_eq(dummy_state(children[2])->last_bounds, 154, 0, 66, 14);
    assert_rect_eq(dummy_state(children[3])->last_bounds, 0, 24, 67, 14);
    assert_rect_eq(dummy_state(children[4])->last_bounds, 77, 24, 67, 14);

    destroy_plane_tree(grid);
    printf("  [PASS] grid auto-fit wraps children\n");
}

static void
test_grid_hidden_children_do_not_reserve_slots(void)
{
    n00b_plane_t *grid = n00b_grid_new(.columns = 3, .col_gap = 2);
    n00b_plane_t *a = make_dummy_child(10, 12, 5, 6, false);
    n00b_plane_t *hidden = make_dummy_child(10, 12, 5, 6, false);
    n00b_plane_t *c = make_dummy_child(10, 12, 5, 6, false);
    n00b_plane_t *d = make_dummy_child(10, 12, 5, 6, false);

    n00b_plane_add_child(grid, a, 0, 0);
    n00b_plane_add_child(grid, hidden, 0, 0);
    n00b_plane_add_child(grid, c, 0, 0);
    n00b_plane_add_child(grid, d, 0, 0);
    n00b_plane_set_visible(hidden, false);

    n00b_widget_layout(grid,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 302,
                           .height = 80,
                       });

    assert_rect_eq(dummy_state(a)->last_bounds, 0, 0, 100, 12);
    assert_rect_eq(dummy_state(c)->last_bounds, 102, 0, 99, 12);
    assert_rect_eq(dummy_state(d)->last_bounds, 203, 0, 99, 12);

    destroy_plane_tree(grid);
    printf("  [PASS] grid hidden children do not reserve slots\n");
}

static void
test_grid_mouse_routes_to_laid_out_child(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(10, 40);
    n00b_plane_t  *grid = n00b_grid_new(.canvas = canvas, .columns = 2);
    n00b_plane_t  *left = make_dummy_child(10, 4, 5, 2, true);
    n00b_plane_t  *right = make_dummy_child(10, 4, 5, 2, true);

    n00b_plane_add_child(grid, left, 0, 0);
    n00b_plane_add_child(grid, right, 0, 0);
    n00b_canvas_add_plane(canvas, grid);

    n00b_widget_layout(grid,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 40,
                           .height = 10,
                       });

    send_left_press(canvas, 5, 1);
    send_left_press(canvas, 25, 1);

    assert(dummy_state(left)->click_count == 1);
    assert(dummy_state(right)->click_count == 1);

    n00b_canvas_remove_plane(canvas, grid);
    destroy_plane_tree(grid);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] grid mouse routes to laid-out child\n");
}

static int g_nested_grid_left_clicks = 0;
static int g_nested_grid_right_clicks = 0;

static void
on_nested_grid_left_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    g_nested_grid_left_clicks++;
}

static void
on_nested_grid_right_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    g_nested_grid_right_clicks++;
}

static void
test_grid_nested_card_children_render_and_interact(void)
{
    g_nested_grid_left_clicks = 0;
    g_nested_grid_right_clicks = 0;

    n00b_canvas_t *canvas = make_stream_canvas(12, 60);
    n00b_plane_t  *grid = n00b_grid_new(.canvas = canvas,
                                        .columns = 2,
                                        .col_gap = 2,
                                        .pad_top = 1,
                                        .pad_left = 4);
    n00b_plane_t  *left_card = n00b_box_new(.canvas    = canvas,
                                            .direction = N00B_FLEX_COLUMN,
                                            .gap       = 1);
    n00b_plane_t  *right_card = n00b_box_new(.canvas    = canvas,
                                             .direction = N00B_FLEX_COLUMN,
                                             .gap       = 1);
    n00b_plane_t  *left_title = n00b_label_new(
        n00b_string_from_cstr("Left Card"),
        .canvas = canvas);
    n00b_plane_t  *right_title = n00b_label_new(
        n00b_string_from_cstr("Right Card"),
        .canvas = canvas);
    n00b_plane_t  *left_button = n00b_button_new(
        n00b_string_from_cstr("Toggle Filters"),
        .canvas   = canvas,
        .on_click = on_nested_grid_left_click);
    n00b_plane_t  *right_button = n00b_button_new(
        n00b_string_from_cstr("Refresh Preview"),
        .canvas   = canvas,
        .on_click = on_nested_grid_right_click);

    n00b_plane_add_child(left_card, left_title, 0, 0);
    n00b_plane_add_child(left_card, left_button, 0, 0);
    n00b_plane_add_child(right_card, right_title, 0, 0);
    n00b_plane_add_child(right_card, right_button, 0, 0);
    n00b_plane_add_child(grid, left_card, 0, 0);
    n00b_plane_add_child(grid, right_card, 0, 0);
    n00b_canvas_add_plane(canvas, grid);

    n00b_display_scene_run_layout(canvas);
    n00b_display_scene_mark_all_dirty(canvas);
    n00b_display_scene_rerender_dirty(canvas);
    n00b_canvas_render(canvas);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    assert(buf != nullptr);
    assert(n00b_unicode_str_contains(buf, r"Toggle Filters"));
    assert(n00b_unicode_str_contains(buf, r"Refresh Preview"));

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    assert(n00b_focus_mgr_current(fm) == left_button);

    n00b_event_t tab = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_TAB, .mods = N00B_MOD_NONE},
    };
    n00b_display_dispatch_result_t dispatch =
        n00b_display_dispatch_event(canvas, fm, &tab);
    assert(dispatch.handled);
    assert(n00b_focus_mgr_current(fm) == right_button);

    n00b_event_t enter = {
        .type = N00B_EVENT_KEY,
        .key = {.key = N00B_KEY_ENTER, .mods = N00B_MOD_NONE},
    };
    dispatch = n00b_display_dispatch_event(canvas, fm, &enter);
    assert(dispatch.handled);
    assert(g_nested_grid_right_clicks == 1);

    send_left_press(canvas,
                    right_button->bounds.x + right_button->bounds.width / 2,
                    right_button->bounds.y + right_button->bounds.height / 2);
    assert(g_nested_grid_right_clicks == 2);
    assert(g_nested_grid_left_clicks == 0);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, grid);
    destroy_plane_tree(grid);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] grid nested card children render and interact\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running grid widget tests...\n");

    test_grid_create_and_api();
    test_grid_fixed_columns_with_span_layout();
    test_grid_fr_tracks_with_clamps();
    test_grid_auto_fit_wraps_children();
    test_grid_hidden_children_do_not_reserve_slots();
    test_grid_mouse_routes_to_laid_out_child();
    test_grid_nested_card_children_render_and_interact();

    printf("All grid widget tests passed.\n");
    n00b_shutdown();
    return 0;
}
