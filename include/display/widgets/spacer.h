/**
 * @file spacer.h
 * @brief Spacer widget: invisible empty space for layout.
 *
 * The simplest possible widget — an empty plane that takes up space.
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *sp = n00b_spacer_new(.cols = 5, .rows = 2);
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
 * @kw cols      Width (default 1).
 * @kw rows      Height (default 1).
 * @kw allocator Allocator.
 */
extern n00b_plane_t *
n00b_spacer_new() _kargs {
    n00b_isize_t      cols      = 1;
    n00b_isize_t      rows      = 1;
    n00b_allocator_t *allocator = nullptr;
};
