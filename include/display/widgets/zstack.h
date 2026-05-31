/**
 * @file zstack.h
 * @brief ZStack widget: layers child planes in shared bounds.
 *
 * A zstack is a container widget that places every child in the same
 * content rectangle. Child-list order determines inter-layer stacking:
 * earlier children are behind later children, while `z` still orders
 * content inside a single layer subtree.
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef struct n00b_zstack_t {
    uint8_t reserved;
} n00b_zstack_t;

extern const n00b_widget_vtable_t n00b_widget_zstack;

extern n00b_plane_t *
n00b_zstack_new() _kargs {
    n00b_box_props_t *box       = nullptr;
    n00b_canvas_t    *canvas    = nullptr;
    n00b_allocator_t *allocator = nullptr;
};

extern void         n00b_zstack_push(n00b_plane_t *stack, n00b_plane_t *layer);
extern n00b_plane_t *n00b_zstack_pop(n00b_plane_t *stack);
extern n00b_isize_t n00b_zstack_count(n00b_plane_t *stack);
extern n00b_plane_t *n00b_zstack_get(n00b_plane_t *stack, n00b_isize_t index);
extern bool         n00b_zstack_bring_to_front(n00b_plane_t *stack, n00b_plane_t *layer);
extern bool         n00b_zstack_send_to_back(n00b_plane_t *stack, n00b_plane_t *layer);
