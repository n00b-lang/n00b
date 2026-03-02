/**
 * @file divider.h
 * @brief Divider widget: horizontal or vertical separator line.
 *
 * Renders a line using box-drawing characters with an optional
 * centered label.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *div = n00b_divider_new();
 * n00b_plane_t *div = n00b_divider_new(.vertical = true,
 *                                       .label = my_str);
 * ```
 */
#pragma once

#include "n00b.h"
#include "core/string.h"
#include "display/render/types.h"
#include "display/render/plane.h"
#include "display/widget.h"

// ====================================================================
// Divider data
// ====================================================================

typedef struct n00b_divider_t {
    n00b_string_t             *label;
    const n00b_border_theme_t *theme;
    n00b_codepoint_t           line_char;  /**< 0 = use theme default. */
    bool                       vertical;
} n00b_divider_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_divider;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new divider widget.
 *
 * @kw vertical   Vertical divider (default false = horizontal).
 * @kw label      Optional centered label text.
 * @kw theme      Border theme for line characters.
 * @kw line_char  Override line character (0 = use theme).
 * @kw width      Width (0 = auto).
 * @kw height     Height (0 = auto: line_height for h, full for v).
 * @kw style      Text style for the line.
 * @kw canvas     Canvas to attach for font metrics (nullptr = none).
 * @kw allocator  Allocator.
 */
extern n00b_plane_t *
n00b_divider_new() _kargs {
    bool                       vertical   = false;
    n00b_string_t             *label      = nullptr;
    const n00b_border_theme_t *theme      = nullptr;
    n00b_codepoint_t           line_char  = 0;
    int32_t                    width      = 0;
    int32_t                    height     = 0;
    n00b_text_style_t         *style      = nullptr;
    n00b_canvas_t             *canvas     = nullptr;
    n00b_allocator_t          *allocator  = nullptr;
};
