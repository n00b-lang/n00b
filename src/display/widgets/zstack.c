/*
 * ZStack widget: overlapping child layers with runtime reordering.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "display/render/box.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/zstack.h"
#include "internal/display/widget_primitives.h"

static void
zstack_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;

    if (data) {
        n00b_free(data);
    }
}

static void
zstack_render(n00b_plane_t *plane, void *data)
{
    (void)data;
    n00b_plane_clear(plane);
}

static void
zstack_measure(n00b_plane_t *plane, void *data,
               int32_t *pref_w, int32_t *pref_h,
               int32_t *min_w, int32_t *min_h)
{
    (void)data;

    int32_t max_pref_w = 0;
    int32_t max_pref_h = 0;
    int32_t max_min_w  = 0;
    int32_t max_min_h  = 0;
    bool    saw_visible = false;

    if (plane && plane->children.data) {
        for (size_t i = 0; i < plane->children.len; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (!child || !(child->flags & N00B_PLANE_VISIBLE)) {
                continue;
            }

            int32_t child_pref_w = 0;
            int32_t child_pref_h = 0;
            int32_t child_min_w  = 0;
            int32_t child_min_h  = 0;

            if (child->widget_vtable && child->widget_vtable->measure) {
                n00b_widget_measure(child,
                                    &child_pref_w,
                                    &child_pref_h,
                                    &child_min_w,
                                    &child_min_h);
            }
            else {
                n00b_widget_measure_plain_plane(child,
                                                &child_pref_w,
                                                &child_pref_h,
                                                &child_min_w,
                                                &child_min_h);
            }

            max_pref_w = n00b_max(max_pref_w, child_pref_w);
            max_pref_h = n00b_max(max_pref_h, child_pref_h);
            max_min_w  = n00b_max(max_min_w, child_min_w);
            max_min_h  = n00b_max(max_min_h, child_min_h);
            saw_visible = true;
        }
    }

    if (!saw_visible) {
        max_pref_w = 1;
        max_pref_h = 1;
        max_min_w  = 1;
        max_min_h  = 1;
    }

    *pref_w = max_pref_w;
    *pref_h = max_pref_h;
    *min_w  = max_min_w;
    *min_h  = max_min_h;
}

static bool
zstack_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    (void)data;
    (void)event;

    return false;
}

static bool
zstack_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    return false;
}

static void
zstack_layout(n00b_plane_t *plane, void *data, n00b_rect_t bounds)
{
    (void)data;

    if (!plane || !plane->children.data) {
        return;
    }

    n00b_rect_t child_bounds = {
        .x      = bounds.x,
        .y      = bounds.y,
        .width  = bounds.width,
        .height = bounds.height,
    };

    for (size_t i = 0; i < plane->children.len; i++) {
        n00b_plane_t *child = plane->children.data[i];
        if (child) {
            n00b_widget_layout(child, child_bounds);
        }
    }
}

const n00b_widget_vtable_t n00b_widget_zstack = {
    .kind         = "zstack",
    .destroy      = zstack_destroy,
    .render       = zstack_render,
    .measure      = zstack_measure,
    .handle_event = zstack_handle_event,
    .can_focus    = zstack_can_focus,
    .layout       = zstack_layout,
};

static n00b_zstack_t *
zstack_data(n00b_plane_t *stack)
{
    return n00b_widget_data_if_kind(stack, &n00b_widget_zstack);
}

n00b_plane_t *
n00b_zstack_new() _kargs {
    n00b_box_props_t *box       = nullptr;
    n00b_canvas_t    *canvas    = nullptr;
    n00b_allocator_t *allocator = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                         .box       = box,
                                         .canvas    = canvas,
                                         .allocator = allocator);

    n00b_zstack_t *stack = n00b_alloc(n00b_zstack_t);
    stack->reserved = 0;

    n00b_widget_attach(plane, &n00b_widget_zstack, stack);
    n00b_plane_mark_dirty(plane);

    return plane;
}

void
n00b_zstack_push(n00b_plane_t *stack, n00b_plane_t *layer)
{
    if (!zstack_data(stack) || !layer) {
        return;
    }

    assert(layer->parent == nullptr);
    n00b_plane_add_child(stack, layer, 0, 0);
}

n00b_plane_t *
n00b_zstack_pop(n00b_plane_t *stack)
{
    if (!zstack_data(stack) || !stack->children.data || stack->children.len == 0) {
        return nullptr;
    }

    n00b_plane_t *top = stack->children.data[stack->children.len - 1];
    if (!n00b_plane_remove_child(stack, top)) {
        return nullptr;
    }

    return top;
}

n00b_isize_t
n00b_zstack_count(n00b_plane_t *stack)
{
    if (!zstack_data(stack)) {
        return 0;
    }

    return (n00b_isize_t)stack->children.len;
}

n00b_plane_t *
n00b_zstack_get(n00b_plane_t *stack, n00b_isize_t index)
{
    if (!zstack_data(stack) || index >= stack->children.len) {
        return nullptr;
    }

    return stack->children.data[index];
}

bool
n00b_zstack_bring_to_front(n00b_plane_t *stack, n00b_plane_t *layer)
{
    if (!zstack_data(stack) || !layer || !stack->children.data) {
        return false;
    }

    for (size_t i = 0; i < stack->children.len; i++) {
        if (stack->children.data[i] == layer) {
            (void)n00b_list_delete(stack->children, i);
            n00b_list_push(stack->children, layer);
            n00b_plane_mark_dirty(stack);
            return true;
        }
    }

    return false;
}

bool
n00b_zstack_send_to_back(n00b_plane_t *stack, n00b_plane_t *layer)
{
    if (!zstack_data(stack) || !layer || !stack->children.data) {
        return false;
    }

    for (size_t i = 0; i < stack->children.len; i++) {
        if (stack->children.data[i] == layer) {
            (void)n00b_list_delete(stack->children, i);
            n00b_list_insert(stack->children, 0, layer);
            n00b_plane_mark_dirty(stack);
            return true;
        }
    }

    return false;
}
