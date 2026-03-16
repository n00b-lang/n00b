/**
 * @file scroll.h
 * @brief Scroll widget: single-child viewport with theme-aware scrollbars.
 *
 * A scroll widget hosts one content subtree, clips it to a viewport,
 * and exposes pixel-native scrolling through API calls, keys, wheel
 * input, and scrollbar thumb dragging.
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/widget.h"

typedef enum {
    N00B_SCROLL_AXIS_NONE       = 0,
    N00B_SCROLL_AXIS_VERTICAL   = 1 << 0,
    N00B_SCROLL_AXIS_HORIZONTAL = 1 << 1,
    N00B_SCROLL_AXIS_BOTH       = 3,
} n00b_scroll_axis_t;

typedef enum {
    N00B_SCROLLBAR_AUTO,
    N00B_SCROLLBAR_ALWAYS,
    N00B_SCROLLBAR_NEVER,
} n00b_scrollbar_mode_t;

typedef struct n00b_scroll_t {
    n00b_plane_t          *content;
    int32_t                offset_x;
    int32_t                offset_y;
    int32_t                content_width;
    int32_t                content_height;
    int32_t                viewport_width;
    int32_t                viewport_height;
    n00b_scroll_axis_t     axes;
    n00b_scrollbar_mode_t  scrollbar_mode;
    int32_t                scroll_step_lines;
    int32_t                scrollbar_thickness_px;
    n00b_rect_t            vscrollbar_rect;
    n00b_rect_t            hscrollbar_rect;
    n00b_rect_t            vthumb_rect;
    n00b_rect_t            hthumb_rect;
    bool                   show_vscrollbar;
    bool                   show_hscrollbar;
    bool                   dragging_vertical_thumb;
    bool                   dragging_horizontal_thumb;
    bool                   hover_vertical_thumb;
    bool                   hover_horizontal_thumb;
    int32_t                drag_anchor_px;
    int32_t                drag_anchor_offset_px;
    n00b_text_style_t     *track_style;
    n00b_text_style_t     *thumb_style;
    n00b_text_style_t     *thumb_hover_style;
} n00b_scroll_t;

extern const n00b_widget_vtable_t n00b_widget_scroll;

extern n00b_plane_t *
n00b_scroll_new(n00b_plane_t *content) _kargs {
    n00b_scroll_axis_t    axes                   = N00B_SCROLL_AXIS_VERTICAL;
    n00b_scrollbar_mode_t scrollbar_mode         = N00B_SCROLLBAR_AUTO;
    int32_t               scroll_step_lines      = 3;
    int32_t               scrollbar_thickness_px = 1;
    n00b_canvas_t        *canvas                 = nullptr;
    n00b_allocator_t     *allocator              = nullptr;
};

extern void         n00b_scroll_set_content(n00b_plane_t *scroll, n00b_plane_t *content);
extern n00b_plane_t *n00b_scroll_get_content(n00b_plane_t *scroll);
extern void         n00b_scroll_to(n00b_plane_t *scroll, int32_t x_px, int32_t y_px);
extern void         n00b_scroll_by(n00b_plane_t *scroll, int32_t dx_px, int32_t dy_px);
extern void         n00b_scroll_to_top(n00b_plane_t *scroll);
extern void         n00b_scroll_to_bottom(n00b_plane_t *scroll);
extern void         n00b_scroll_to_start(n00b_plane_t *scroll);
extern void         n00b_scroll_to_end(n00b_plane_t *scroll);
extern void         n00b_scroll_ensure_visible(n00b_plane_t *scroll, n00b_rect_t rect_px);
extern int32_t      n00b_scroll_get_offset_x(n00b_plane_t *scroll);
extern int32_t      n00b_scroll_get_offset_y(n00b_plane_t *scroll);
extern bool         n00b_scroll_can_scroll_up(n00b_plane_t *scroll);
extern bool         n00b_scroll_can_scroll_down(n00b_plane_t *scroll);
extern bool         n00b_scroll_can_scroll_left(n00b_plane_t *scroll);
extern bool         n00b_scroll_can_scroll_right(n00b_plane_t *scroll);
