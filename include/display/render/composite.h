/**
 * @file composite.h
 * @brief Compositing pipeline for the render layer.
 *
 * Flattens the plane hierarchy into a z-sorted list, composites
 * planes into a frame buffer, resolves state-based styles, and
 * applies graceful degradation based on backend capabilities.
 *
 * ### Content model
 *
 * Planes contain draw commands (pixel coordinates).  Pixel backends
 * (NC, Cocoa) render commands natively.  Cell backends use
 * `n00b_composite_commands_to_grid()` to convert draw commands into
 * a flat cell grid for SGR/cursor-based output.
 *
 * ### Related modules
 *
 * - `render/plane.h` — plane that is composited
 * - `render/draw_cmd.h` — draw commands in planes
 * - `render/canvas.h` — canvas that drives compositing
 * - `render/backend.h` — capabilities used for degradation
 */
#pragma once

#include "n00b.h"
#include "adt/array.h"
#include "display/render/cell.h"
#include "display/render/plane.h"
#include "display/render/backend.h"

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

    // Clip rectangle (in absolute frame coordinates, pixels).
    int32_t       clip_x;
    int32_t       clip_y;
    int32_t       clip_w;
    int32_t       clip_h;
} n00b_composite_entry_t;

// ====================================================================
// Entry info (extracted plane decoration metadata)
// ====================================================================

/**
 * @brief Precomputed layout metadata for a composite entry.
 *
 * Computed by `n00b_composite_entry_info()` from a composite entry's
 * plane and its absolute position.  Backends use this to position
 * borders, fill, and content without re-deriving box insets.
 */
typedef struct {
    int32_t            outer_x, outer_y;       /**< Frame coords of outer box (after margins). */
    n00b_isize_t       outer_rows, outer_cols;  /**< Total size including border+padding. */
    n00b_isize_t       inset_top, inset_bot, inset_left, inset_right;
    int32_t            content_x, content_y;   /**< Frame coords of content area. */
    n00b_text_style_t *border_style;
    n00b_text_style_t *fill_style;
    n00b_text_style_t *text_style;
    const n00b_box_props_t *box;
} n00b_entry_info_t;

// ====================================================================
// Pipeline functions
// ====================================================================

/**
 * @brief Flatten a list of top-level planes into a z-sorted array.
 *
 * Recursively walks each plane's children, computing absolute
 * coordinates in pixels.  The output is sorted by z (stable, low-z first).
 *
 * @param planes     Top-level planes.
 * @param num_planes Number of top-level planes.
 * @param cell_px_w  Pixels per cell column (1 for cell-only backends).
 * @param cell_px_h  Pixels per cell row (1 for cell-only backends).
 * @return           Array of composite entries (caller frees via `n00b_array_free`).
 */
extern n00b_array_t(n00b_composite_entry_t)
    n00b_composite_flatten(n00b_plane_t **planes,
                            n00b_isize_t   num_planes,
                            int32_t        cell_px_w,
                            int32_t        cell_px_h);

// ====================================================================
// Shared helpers for plane-based backends
// ====================================================================

/**
 * @brief Resolve the effective style for a plane in its current state.
 *
 * Checks the plane's `widget_state` and returns the state override
 * style if one exists, otherwise returns `base_style`.
 *
 * @param p           Plane to resolve for.
 * @param base_style  Base style (from box props or plane default).
 * @param style_field Which style: 0=text, 1=border, 2=fill.
 * @return            Resolved style pointer.
 */
extern n00b_text_style_t *
n00b_composite_resolve_style(n00b_plane_t      *p,
                              n00b_text_style_t *base_style,
                              int                style_field);

/**
 * @brief Compute layout metadata for a composite entry.
 *
 * Derives frame-coordinate positions (in pixels), box insets, and
 * resolved styles for the entry's plane.  Backends call this once
 * per entry.
 *
 * @param entry     Composite entry with plane and absolute position.
 * @param out       Output info struct to populate.
 * @param cell_px_w Pixels per cell column.
 * @param cell_px_h Pixels per cell row.
 */
extern void
n00b_composite_entry_info(const n00b_composite_entry_t *entry,
                           n00b_entry_info_t            *out,
                           int32_t                       cell_px_w,
                           int32_t                       cell_px_h);

/**
 * @brief Apply graceful degradation to a single cell.
 *
 * Backends that need degradation call this per-cell.  Pixel backends
 * can skip it entirely.
 *
 * @param cell  Cell to degrade in-place.
 * @param caps  Backend capabilities.
 */
extern void
n00b_composite_degrade_cell(n00b_rcell_t     *cell,
                             n00b_render_cap_t caps);

/**
 * @brief Composite draw commands into a caller-supplied cell grid.
 *
 * Converts pixel-coordinate draw commands from each plane into a
 * flat cell grid for cell-based backends (ANSI, inline, stream, dumb).
 *
 * For each entry (low-z first, painter's algorithm):
 * 1. Stamps box decorations via `n00b_box_stamp()`.
 * 2. Walks `plane->draw_list`, converting pixel positions to cell
 *    positions, and writes graphemes into the cell grid.
 * 3. Applies graceful degradation to all cells.
 *
 * @param entries       Z-sorted composite entries (pixel coords).
 * @param count         Number of entries.
 * @param frame         Target cell grid (row-major, `cell_rows * cell_cols`).
 * @param cell_rows     Frame height in cells.
 * @param cell_cols     Frame width in cells.
 * @param cell_px_w     Pixels per cell column.
 * @param cell_px_h     Pixels per cell row.
 * @param default_style Default style for empty cells.
 * @param caps          Backend capabilities for degradation.
 */
extern void
n00b_composite_commands_to_grid(const n00b_composite_entry_t *entries,
                                 n00b_isize_t                  count,
                                 n00b_rcell_t                 *frame,
                                 n00b_isize_t                  cell_rows,
                                 n00b_isize_t                  cell_cols,
                                 int32_t                       cell_px_w,
                                 int32_t                       cell_px_h,
                                 n00b_text_style_t            *default_style,
                                 n00b_render_cap_t             caps);
