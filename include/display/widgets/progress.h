/**
 * @file progress.h
 * @brief Progress bar widget: horizontal or vertical, 0.0–1.0.
 *
 * Renders a progress bar using block characters with sub-cell
 * precision via partial block characters (U+258F..U+2588).
 *
 * ### Usage
 *
 * ```c
 * n00b_plane_t *bar = n00b_progress_new(.width = 30);
 * n00b_progress_set_value(bar, 0.5);
 * ```
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "text/strings/text_style.h"

// ====================================================================
// Progress data
// ====================================================================

typedef struct n00b_progress_t {
    double              value;        /**< 0.0 to 1.0. */
    bool                vertical;
    n00b_text_style_t  *fill_style;
    n00b_text_style_t  *empty_style;
    n00b_codepoint_t    fill_char;    /**< Default: U+2588 (full block). */
    n00b_codepoint_t    empty_char;   /**< Default: U+2591 (light shade). */
} n00b_progress_t;

// ====================================================================
// Vtable
// ====================================================================

extern const n00b_widget_vtable_t n00b_widget_progress;

// ====================================================================
// Public API
// ====================================================================

/**
 * @brief Create a new progress bar widget.
 *
 * @kw value       Initial value (0.0–1.0).
 * @kw vertical    Vertical progress bar (default false).
 * @kw fill_style  Style for filled portion.
 * @kw empty_style Style for empty portion.
 * @kw width       Width (default 20).
 * @kw height      Height (default 1, scaled by line height for horizontal).
 * @kw canvas      Canvas to attach for font metrics (nullptr = none).
 * @kw allocator   Allocator.
 */
extern n00b_plane_t *
n00b_progress_new() _kargs {
    double              value       = 0.0;
    bool                vertical    = false;
    n00b_text_style_t  *fill_style  = nullptr;
    n00b_text_style_t  *empty_style = nullptr;
    int32_t             width       = 20;
    int32_t             height      = 1;
    n00b_canvas_t      *canvas      = nullptr;
    n00b_allocator_t   *allocator   = nullptr;
};

/**
 * @brief Set the progress value.
 * @param plane Progress bar plane.
 * @param value New value (clamped to 0.0–1.0).
 */
extern void n00b_progress_set_value(n00b_plane_t *plane, double value);

/**
 * @brief Get the current progress value.
 */
extern double n00b_progress_get_value(n00b_plane_t *plane);
