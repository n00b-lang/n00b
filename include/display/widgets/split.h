/**
 * @file split.h
 * @brief Split widget: two-pane container with a draggable divider.
 *
 * A split lays out up to two child panes either left/right or
 * top/bottom. The divider size is pixel-native, sizing is ratio-based,
 * minimum sizes are enforced when possible, and mouse dragging updates
 * the ratio live.
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef enum {
    N00B_SPLIT_HORIZONTAL,
    N00B_SPLIT_VERTICAL,
} n00b_split_orientation_t;

typedef void (*n00b_split_change_cb_t)(n00b_plane_t *split,
                                       float         ratio,
                                       void         *data);

typedef struct n00b_split_t {
    n00b_plane_t              *first;
    n00b_plane_t              *second;
    n00b_split_orientation_t   orientation;
    float                      ratio;
    int32_t                    min_first_px;
    int32_t                    min_second_px;
    int32_t                    divider_px;
    n00b_split_change_cb_t     on_change;
    void                      *on_change_data;
    n00b_rect_t                divider_rect;
    bool                       divider_hovered;
    bool                       dragging;
    int32_t                    drag_pointer_offset_px;
} n00b_split_t;

extern const n00b_widget_vtable_t n00b_widget_split;

extern n00b_plane_t *
n00b_split_new(n00b_plane_t *first, n00b_plane_t *second) _kargs {
    n00b_split_orientation_t orientation   = N00B_SPLIT_HORIZONTAL;
    float                    ratio         = 0.5f;
    int32_t                  min_first_px  = 64;
    int32_t                  min_second_px = 64;
    int32_t                  divider_px    = 1;
    n00b_split_change_cb_t   on_change     = nullptr;
    void                    *on_change_data = nullptr;
    n00b_canvas_t           *canvas        = nullptr;
    n00b_allocator_t        *allocator     = nullptr;
};

extern void  n00b_split_set_ratio(n00b_plane_t *split, float ratio);
extern float n00b_split_get_ratio(n00b_plane_t *split);
extern void  n00b_split_set_first(n00b_plane_t *split, n00b_plane_t *first);
extern void  n00b_split_set_second(n00b_plane_t *split, n00b_plane_t *second);
extern void  n00b_split_set_min_sizes(n00b_plane_t *split,
                                      int32_t       min_first_px,
                                      int32_t       min_second_px);
extern void  n00b_split_set_divider_size(n00b_plane_t *split,
                                         int32_t       divider_px);
