/*
 * Unit tests for the zstack widget.
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/backend_stream_internal.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/box.h"
#include "display/widgets/zstack.h"

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
    .kind         = "zstack_test_dummy",
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
make_dummy_layer(int32_t pref_w, int32_t pref_h,
                 int32_t min_w, int32_t min_h,
                 bool consume_click)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane);
    plane->width  = pref_w > 0 ? pref_w : 1;
    plane->height = pref_h > 0 ? pref_h : 1;

    dummy_widget_t *dummy = n00b_alloc(dummy_widget_t);
    *dummy = (dummy_widget_t){
        .pref_w        = pref_w > 0 ? pref_w : 1,
        .pref_h        = pref_h > 0 ? pref_h : 1,
        .min_w         = min_w > 0 ? min_w : 1,
        .min_h         = min_h > 0 ? min_h : 1,
        .consume_click = consume_click,
    };

    n00b_widget_attach(plane, &dummy_widget, dummy);

    return plane;
}

static n00b_plane_t *
make_plain_layer(int32_t width, int32_t height, int32_t z)
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane, .z = z);
    plane->width = width > 0 ? width : 1;
    plane->height = height > 0 ? height : 1;
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
layout_stack_to_canvas(n00b_canvas_t *canvas, n00b_plane_t *stack)
{
    n00b_widget_layout(stack,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = (int32_t)canvas->frame_cols,
                           .height = (int32_t)canvas->frame_rows,
                       });
}

static void
test_zstack_create_and_api(void)
{
    n00b_plane_t *stack = n00b_zstack_new();

    assert(stack != nullptr);
    assert(stack->widget_vtable == &n00b_widget_zstack);
    assert(!n00b_widget_can_focus(stack));
    assert(n00b_zstack_count(stack) == 0);
    assert(n00b_zstack_get(stack, 0) == nullptr);
    assert(n00b_zstack_pop(stack) == nullptr);

    n00b_widget_detach(stack);
    n00b_plane_destroy(stack);
    printf("  [PASS] zstack create and api\n");
}

static void
test_zstack_measure_visible_max(void)
{
    n00b_plane_t *stack  = n00b_zstack_new();
    n00b_plane_t *small  = make_dummy_layer(7, 3, 2, 1, false);
    n00b_plane_t *large  = make_dummy_layer(20, 9, 11, 5, false);
    n00b_plane_t *hidden = make_dummy_layer(40, 18, 25, 10, false);

    n00b_zstack_push(stack, small);
    n00b_zstack_push(stack, large);
    n00b_zstack_push(stack, hidden);
    n00b_plane_set_visible(hidden, false);

    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w  = 0;
    int32_t min_h  = 0;

    n00b_widget_measure(stack, &pref_w, &pref_h, &min_w, &min_h);

    assert(pref_w == 20);
    assert(pref_h == 9);
    assert(min_w == 11);
    assert(min_h == 5);

    destroy_plane_tree(stack);
    printf("  [PASS] zstack measure visible max\n");
}

static void
test_zstack_measure_plain_plane_layers(void)
{
    n00b_plane_t *stack  = n00b_zstack_new();
    n00b_plane_t *small  = make_plain_layer(7, 3, 0);
    n00b_plane_t *large  = make_plain_layer(20, 9, 0);
    n00b_plane_t *hidden = make_plain_layer(40, 18, 0);

    n00b_zstack_push(stack, small);
    n00b_zstack_push(stack, large);
    n00b_zstack_push(stack, hidden);
    n00b_plane_set_visible(hidden, false);

    int32_t pref_w = 0;
    int32_t pref_h = 0;
    int32_t min_w = 0;
    int32_t min_h = 0;

    n00b_widget_measure(stack, &pref_w, &pref_h, &min_w, &min_h);

    assert(pref_w == 20);
    assert(pref_h == 9);
    assert(min_w == 20);
    assert(min_h == 9);

    destroy_plane_tree(stack);
    printf("  [PASS] zstack measure plain plane layers\n");
}

static void
test_zstack_layout_fills_all_children(void)
{
    n00b_plane_t *stack = n00b_zstack_new();
    n00b_plane_t *a     = make_dummy_layer(4, 2, 2, 1, false);
    n00b_plane_t *b     = make_dummy_layer(6, 3, 2, 1, false);
    n00b_rect_t   bounds = {
        .x      = 4,
        .y      = 6,
        .width  = 33,
        .height = 11,
    };

    n00b_zstack_push(stack, a);
    n00b_zstack_push(stack, b);
    n00b_widget_layout(stack, bounds);

    assert(a->bounds.x == 0);
    assert(a->bounds.y == 0);
    assert(a->bounds.width == bounds.width);
    assert(a->bounds.height == bounds.height);
    assert(dummy_state(a)->last_bounds.x == 0);
    assert(dummy_state(a)->last_bounds.y == 0);
    assert(dummy_state(a)->last_bounds.width == bounds.width);
    assert(dummy_state(a)->last_bounds.height == bounds.height);
    assert(dummy_state(b)->last_bounds.x == 0);
    assert(dummy_state(b)->last_bounds.y == 0);
    assert(dummy_state(b)->last_bounds.width == bounds.width);
    assert(dummy_state(b)->last_bounds.height == bounds.height);

    destroy_plane_tree(stack);
    printf("  [PASS] zstack layout fills all children\n");
}

static void
test_zstack_child_list_order_wins_over_child_z(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 40);
    n00b_plane_t *stack = n00b_zstack_new(.canvas = canvas);
    n00b_plane_t *back = make_plain_layer(5, 5, 10);
    n00b_plane_t *back_leaf = make_plain_layer(5, 5, 5);
    n00b_plane_t *front = make_plain_layer(5, 5, 0);
    n00b_plane_t *front_leaf = make_plain_layer(5, 5, 0);

    n00b_plane_draw_glyph(back_leaf, 0, 0, 'B');
    n00b_plane_draw_glyph(front_leaf, 0, 0, 'F');
    n00b_plane_add_child(back, back_leaf, 0, 0);
    n00b_plane_add_child(front, front_leaf, 0, 0);

    n00b_canvas_add_plane(canvas, stack);
    n00b_zstack_push(stack, back);
    n00b_zstack_push(stack, front);
    layout_stack_to_canvas(canvas, stack);
    n00b_canvas_render(canvas);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(canvas->backend_ctx);
    assert(buf->data[0] == 'F');

    assert(n00b_canvas_remove_plane(canvas, stack));
    destroy_plane_tree(stack);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack child-list order wins over child z\n");
}

static void
test_zstack_mouse_hits_frontmost_layer(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 40);
    n00b_plane_t  *stack  = n00b_zstack_new(.canvas = canvas);
    n00b_plane_t  *back   = make_dummy_layer(5, 5, 1, 1, true);
    n00b_plane_t  *front  = make_dummy_layer(5, 5, 1, 1, true);

    n00b_canvas_add_plane(canvas, stack);
    n00b_zstack_push(stack, back);
    n00b_zstack_push(stack, front);
    layout_stack_to_canvas(canvas, stack);

    send_left_press(canvas, 10, 10);

    assert(dummy_state(back)->click_count == 0);
    assert(dummy_state(front)->click_count == 1);

    assert(n00b_canvas_remove_plane(canvas, stack));
    destroy_plane_tree(stack);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack mouse hits frontmost layer\n");
}

static void
test_zstack_reorder_changes_front_layer(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 40);
    n00b_plane_t  *stack  = n00b_zstack_new(.canvas = canvas);
    n00b_plane_t  *bottom = make_dummy_layer(5, 5, 1, 1, true);
    n00b_plane_t  *top    = make_dummy_layer(5, 5, 1, 1, true);

    n00b_canvas_add_plane(canvas, stack);
    n00b_zstack_push(stack, bottom);
    n00b_zstack_push(stack, top);
    layout_stack_to_canvas(canvas, stack);

    send_left_press(canvas, 8, 8);
    assert(dummy_state(bottom)->click_count == 0);
    assert(dummy_state(top)->click_count == 1);

    assert(n00b_zstack_bring_to_front(stack, bottom));
    assert(n00b_zstack_get(stack, 1) == bottom);
    send_left_press(canvas, 8, 8);
    assert(dummy_state(bottom)->click_count == 1);
    assert(dummy_state(top)->click_count == 1);

    assert(n00b_zstack_send_to_back(stack, bottom));
    assert(n00b_zstack_get(stack, 0) == bottom);
    send_left_press(canvas, 8, 8);
    assert(dummy_state(bottom)->click_count == 1);
    assert(dummy_state(top)->click_count == 2);

    assert(n00b_canvas_remove_plane(canvas, stack));
    destroy_plane_tree(stack);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack reorder changes front layer\n");
}

static void
test_zstack_scene_after_controls_keeps_local_hit_targets(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 60);
    n00b_plane_t *root = n00b_box_new(.canvas = canvas,
                                      .direction = N00B_FLEX_ROW,
                                      .gap = 2);
    root->width = 60;
    root->height = 20;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *controls = n00b_new_kargs(n00b_plane_t, plane);
    controls->width = 12;
    controls->height = 20;
    controls->flex.basis = 12;
    controls->flex.shrink = 0.0f;
    n00b_plane_add_child(root, controls, 0, 0);

    n00b_plane_t *scene = n00b_zstack_new(.canvas = canvas);
    scene->flex.grow = 1.0f;
    scene->flex.shrink = 1.0f;
    scene->flex.basis = 1;
    n00b_plane_add_child(root, scene, 0, 0);

    n00b_plane_t *background = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *overlay = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *back_btn = make_dummy_layer(8, 3, 1, 1, true);
    n00b_plane_t *front_btn = make_dummy_layer(8, 3, 1, 1, true);
    n00b_plane_t *side_btn = make_dummy_layer(8, 3, 1, 1, true);

    n00b_plane_add_child(background, back_btn, 8, 6);
    n00b_plane_add_child(overlay, front_btn, 8, 6);
    n00b_plane_add_child(controls, side_btn, 1, 1);

    n00b_zstack_push(scene, background);
    n00b_zstack_push(scene, overlay);

    n00b_widget_layout(root,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 60,
                           .height = 20,
                       });
    n00b_widget_layout(scene, scene->bounds);

    send_left_press(canvas,
                    controls->x + side_btn->x + 1,
                    controls->y + side_btn->y + 1);
    assert(dummy_state(side_btn)->click_count == 1);
    assert(dummy_state(front_btn)->click_count == 0);
    assert(dummy_state(back_btn)->click_count == 0);

    send_left_press(canvas,
                    scene->x + front_btn->x + 1,
                    scene->y + front_btn->y + 1);
    assert(dummy_state(side_btn)->click_count == 1);
    assert(dummy_state(front_btn)->click_count == 1);
    assert(dummy_state(back_btn)->click_count == 0);

    assert(n00b_canvas_remove_plane(canvas, root));
    destroy_plane_tree(root);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack scene after controls keeps local hit targets\n");
}

static void
test_zstack_pop_detaches_top_layer(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 40);
    n00b_plane_t  *stack  = n00b_zstack_new(.canvas = canvas);
    n00b_plane_t  *base   = make_dummy_layer(5, 5, 1, 1, true);
    n00b_plane_t  *top    = make_dummy_layer(5, 5, 1, 1, true);
    n00b_plane_t  *leaf   = make_dummy_layer(3, 2, 1, 1, false);

    n00b_plane_add_child(top, leaf, 1, 1);
    n00b_canvas_add_plane(canvas, stack);
    n00b_zstack_push(stack, base);
    n00b_zstack_push(stack, top);
    layout_stack_to_canvas(canvas, stack);

    assert(top->canvas == canvas);
    assert(leaf->canvas == canvas);

    n00b_plane_t *popped = n00b_zstack_pop(stack);
    assert(popped == top);
    assert(popped->parent == nullptr);
    assert(popped->canvas == nullptr);
    assert(leaf->canvas == nullptr);
    assert(n00b_zstack_count(stack) == 1);
    assert(n00b_zstack_get(stack, 0) == base);

    assert(n00b_canvas_remove_plane(canvas, stack));
    assert(stack->canvas == nullptr);
    assert(base->canvas == nullptr);

    destroy_plane_tree(popped);
    destroy_plane_tree(stack);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack pop detaches top layer\n");
}

static void
test_zstack_scene_coexists_with_sibling_controls(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 60);
    n00b_plane_t  *root   = n00b_box_new(.canvas = canvas,
                                         .direction = N00B_FLEX_ROW,
                                         .gap = 2);
    root->width  = 60;
    root->height = 20;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *scene = n00b_zstack_new(.canvas = canvas);
    scene->flex.grow   = 1.0f;
    scene->flex.shrink = 1.0f;
    scene->flex.basis  = 1;
    n00b_plane_add_child(root, scene, 0, 0);

    n00b_plane_t *controls = n00b_new_kargs(n00b_plane_t, plane);
    controls->width        = 12;
    controls->height       = 20;
    controls->flex.basis   = 12;
    controls->flex.shrink  = 0.0f;
    n00b_plane_add_child(root, controls, 0, 0);

    n00b_plane_t *background = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *overlay    = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *back_btn   = make_dummy_layer(8, 3, 1, 1, true);
    n00b_plane_t *front_btn  = make_dummy_layer(8, 3, 1, 1, true);
    n00b_plane_t *side_btn   = make_dummy_layer(8, 3, 1, 1, true);

    n00b_plane_add_child(background, back_btn, 8, 6);
    n00b_plane_add_child(overlay, front_btn, 8, 6);
    n00b_plane_add_child(controls, side_btn, 1, 1);

    n00b_zstack_push(scene, background);
    n00b_zstack_push(scene, overlay);

    n00b_widget_layout(root,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = 60,
                           .height = 20,
                       });
    n00b_widget_layout(scene, scene->bounds);

    send_left_press(canvas,
                    controls->x + side_btn->x + 1,
                    controls->y + side_btn->y + 1);
    assert(dummy_state(side_btn)->click_count == 1);
    assert(dummy_state(front_btn)->click_count == 0);
    assert(dummy_state(back_btn)->click_count == 0);

    send_left_press(canvas,
                    scene->x + front_btn->x + 1,
                    scene->y + front_btn->y + 1);
    assert(dummy_state(side_btn)->click_count == 1);
    assert(dummy_state(front_btn)->click_count == 1);
    assert(dummy_state(back_btn)->click_count == 0);

    assert(n00b_canvas_remove_plane(canvas, root));
    destroy_plane_tree(root);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] zstack scene coexists with sibling controls\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running zstack widget tests...\n");

    test_zstack_create_and_api();
    test_zstack_measure_visible_max();
    test_zstack_measure_plain_plane_layers();
    test_zstack_layout_fills_all_children();
    test_zstack_child_list_order_wins_over_child_z();
    test_zstack_mouse_hits_frontmost_layer();
    test_zstack_reorder_changes_front_layer();
    test_zstack_scene_after_controls_keeps_local_hit_targets();
    test_zstack_pop_detaches_top_layer();
    test_zstack_scene_coexists_with_sibling_controls();

    printf("All zstack widget tests passed.\n");
    n00b_shutdown();
    return 0;
}
