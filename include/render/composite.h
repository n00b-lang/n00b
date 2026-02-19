/**
 * @file composite.h
 * @brief Compositing pipeline for the render layer.
 *
 * Flattens the plane hierarchy into a z-sorted list, composites
 * planes into a frame buffer, resolves state-based styles, and
 * applies graceful degradation based on backend capabilities.
 *
 * ### Related modules
 *
 * - `render/plane.h` — plane that is composited
 * - `render/canvas.h` — canvas that drives compositing
 * - `render/backend.h` — capabilities used for degradation
 */
#pragma once

#include "n00b.h"
#include "core/array.h"
#include "render/cell.h"
#include "render/plane.h"
#include "render/backend.h"

// ====================================================================
// Composite entry (flattened plane)
// ====================================================================

/**
 * @brief A plane with absolute coordinates, ready for compositing.
 *
 * The clip rectangle defines the region where this plane's content
 * is visible, based on ancestor bounds.  Content outside the clip
 * rectangle is not rendered.
 */
typedef struct n00b_composite_entry_t {
    n00b_plane_t *plane;
    int32_t       abs_x;
    int32_t       abs_y;
    int32_t       abs_z;

    // Clip rectangle (in absolute frame coordinates).
    // Set to frame bounds for top-level planes; intersected with
    // parent bounds for child planes.
    int32_t       clip_x;
    int32_t       clip_y;
    int32_t       clip_w;
    int32_t       clip_h;
} n00b_composite_entry_t;

n00b_array_decl(n00b_composite_entry_t);

// ====================================================================
// Pipeline functions
// ====================================================================

/**
 * @brief Flatten a list of top-level planes into a z-sorted array.
 *
 * Recursively walks each plane's children, computing absolute
 * coordinates.  The output is sorted by z (stable, low-z first).
 *
 * @param planes     Top-level planes.
 * @param num_planes Number of top-level planes.
 * @return           Array of composite entries (caller frees via `n00b_array_free`).
 */
extern n00b_array_t(n00b_composite_entry_t)
    n00b_composite_flatten(n00b_plane_t **planes,
                            n00b_isize_t   num_planes);

/**
 * @brief Composite flattened planes into a frame buffer.
 *
 * Iterates the z-sorted entries, copying visible viewport cells into
 * the frame buffer.  Applies state-based style resolution.
 *
 * @param entries    Sorted composite entries (low-z first).
 * @param count      Number of entries.
 * @param frame      Target frame buffer (row-major).
 * @param frame_rows Frame height.
 * @param frame_cols Frame width.
 * @param default_style Default style for empty frame cells.
 */
extern void
n00b_composite_render(const n00b_composite_entry_t *entries,
                       n00b_isize_t                  count,
                       n00b_rcell_t                 *frame,
                       n00b_isize_t                  frame_rows,
                       n00b_isize_t                  frame_cols,
                       n00b_text_style_t            *default_style);

/**
 * @brief Apply graceful degradation to a frame based on capabilities.
 *
 * Replaces Unicode box-drawing with ASCII if no `N00B_RCAP_UNICODE`,
 * quantizes colors, strips unsupported decorations.
 *
 * @param frame      Frame buffer to degrade in-place.
 * @param frame_rows Frame height.
 * @param frame_cols Frame width.
 * @param caps       Backend capabilities.
 */
extern void
n00b_composite_degrade(n00b_rcell_t     *frame,
                        n00b_isize_t      frame_rows,
                        n00b_isize_t      frame_cols,
                        n00b_render_cap_t caps);
