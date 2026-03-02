/**
 * @file box.h
 * @brief Box widget: flex container for laying out child planes.
 *
 * A box is a container widget that arranges its children using a
 * CSS-like flexbox algorithm.  Layout operates entirely in pixels.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *root = n00b_box_new(.direction = N00B_FLEX_COLUMN,
 *                                    .gap       = 1);
 * // Add children; set child->flex.grow for stretchy items.
 * n00b_plane_add_child(root, child1, 0, 0);
 * child1->flex.grow = 1.0f;
 * ```
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"

// ====================================================================
// Box data
// ====================================================================

typedef struct n00b_box_data_t {
    n00b_flex_container_t flex;
    n00b_padding_t        padding;
} n00b_box_data_t;

// ====================================================================
// Vtable (defined in box.c)
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_box;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new box container widget.
 *
 * Returns a plane with the box widget attached.
 *
 * @kw direction Flex direction (default COLUMN).
 * @kw justify   Main-axis justification (default START).
 * @kw align     Cross-axis alignment (default STRETCH).
 * @kw gap       Gap between children in pixels (default 0).
 * @kw padding   Padding around content in pixels (default 0).
 * @kw allocator Allocator.
 *
 * @return A new plane with box widget attached.
 */
extern n00b_plane_t *
n00b_box_new() _kargs {
    n00b_flex_direction_t direction  = N00B_FLEX_COLUMN;
    n00b_justify_t        justify    = N00B_JUSTIFY_START;
    n00b_align_items_t    align      = N00B_ALIGN_STRETCH_CROSS;
    int32_t               gap        = 0;
    int32_t               pad_top    = 0;
    int32_t               pad_right  = 0;
    int32_t               pad_bottom = 0;
    int32_t               pad_left   = 0;
    n00b_canvas_t        *canvas     = nullptr;
    n00b_allocator_t     *allocator  = nullptr;
};

/**
 * @brief Run flex layout on children of a container plane.
 *
 * The core 4-phase flex algorithm.  All coordinates are in pixels.
 * Called by `box_layout()` and available for other container widgets.
 *
 * @param container The container plane whose children to layout.
 * @param bounds    Content area in pixels (after padding).
 * @param flex      Flex container parameters.
 */
extern void n00b_flex_layout(n00b_plane_t          *container,
                              n00b_rect_t            bounds,
                              n00b_flex_container_t *flex);
