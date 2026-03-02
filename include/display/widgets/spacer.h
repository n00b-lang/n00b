/**
 * @file spacer.h
 * @brief Spacer widget: invisible empty space for layout.
 *
 * The simplest possible widget — an empty plane that takes up space.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *sp = n00b_spacer_new(.width = 5, .height = 2);
 * ```
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/widget.h"

// ====================================================================
// Spacer data
// ====================================================================

typedef struct n00b_spacer_t {
    n00b_isize_t min_cols;
    n00b_isize_t min_rows;
} n00b_spacer_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_spacer;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new spacer widget.
 *
 * @kw width     Width (default 1).
 * @kw height    Height (default 1).
 * @kw canvas    Canvas to attach for font metrics (nullptr = none).
 * @kw allocator Allocator.
 */
extern n00b_plane_t *
n00b_spacer_new() _kargs {
    int32_t           width     = 1;
    int32_t           height    = 1;
    n00b_canvas_t    *canvas    = nullptr;
    n00b_allocator_t *allocator = nullptr;
};
