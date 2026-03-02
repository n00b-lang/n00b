/**
 * @file plane.h
 * @brief Render plane: a compositing surface with pixel-native draw commands.
 *
 * A plane (`n00b_plane_t`) is the primary content container in the
 * rendering system.  It owns a list of draw commands in pixel
 * coordinates, optional box decorations, and viewport/scroll state.
 *
 * Planes form a hierarchy (parent/child) for nested composition.
 * Each plane has a position (x, y) in its parent's coordinate space
 * and a z-order for compositing.
 *
 * ### Content model
 *
 * Widgets issue draw commands (text, glyphs, fill rects) at pixel
 * coordinates relative to the plane's content origin (0,0 = top-left
 * inside border+padding).  Pixel backends render natively; cell
 * backends convert commands to a cell grid via the compositor.
 *
 * Box decorations (borders, padding) are NOT draw commands — they
 * are handled per-backend using `n00b_entry_info_t` metadata.
 *
 * ### Related modules
 *
 * - `render/draw_cmd.h` — draw command types
 * - `render/font_metrics.h` — text measurement
 * - `render/types.h` — box props, alignment, scroll mode
 * - `render/canvas.h` — canvas composites planes into frames
 * - `render/box.h` — box stamping during compositing
 */
#pragma once

#include "n00b.h"
#include "adt/option.h"
#include "core/string.h"
#include "adt/list.h"
#include "display/render/draw_cmd.h"
#include "display/render/types.h"
#include "text/strings/text_style.h"

// Type-safe list for child planes.
typedef struct n00b_plane_t *n00b_plane_ptr_t;

// Forward declaration — canvas owns the metrics provider.
typedef struct n00b_canvas_t n00b_canvas_t;

// ====================================================================
// Plane flags
// ====================================================================

#define N00B_PLANE_VISIBLE 0x0001
#define N00B_PLANE_DIRTY   0x0002

// ====================================================================
// Plane structure
// ====================================================================

typedef struct n00b_plane_t {
    // Identity / hierarchy
    n00b_string_t                    *name;
    struct n00b_plane_t              *parent;
    n00b_list_t(n00b_plane_ptr_t)    children;

    // Draw command storage (pixel coordinates, relative to content origin).
    n00b_draw_list_t     draw_list;

    // Back-pointer to owning canvas (set when added, propagated to children).
    // Used for font metrics access.
    n00b_canvas_t       *canvas;

    // Pixel scroll offset within content.
    int32_t              scroll_x;
    int32_t              scroll_y;

    // Content area size in pixels (set by layout or widget constructor).
    int32_t              width;
    int32_t              height;

    // Position in parent's coordinate space (pixels).
    int32_t              x;
    int32_t              y;
    int32_t              z;

    // Decoration
    n00b_box_props_t    *box;
    n00b_text_style_t   *default_style;

    // Behavior
    n00b_scroll_mode_t   scroll_mode;
    n00b_widget_state_t  widget_state;
    uint16_t             flags;

    // Monotonic counter incremented on every draw-list mutation.
    // Backends use this to skip re-rendering unchanged planes.
    uint32_t             render_gen;

    // Widget behavior (nullptr = plain plane, no widget attached)
    const struct n00b_widget_vtable_t *widget_vtable;
    void                              *widget_data;

    // Layout (pixel-based flex)
    n00b_flex_props_t    flex;    /**< Flex item properties (when in a flex container). */
    n00b_rect_t          bounds;  /**< Assigned pixel bounds (set by layout engine). */

    n00b_rwlock_t       *lock;
    n00b_allocator_t    *allocator;
} n00b_plane_t;

// ====================================================================
// Lifecycle
// ====================================================================

/**
 * @brief Initialize a pre-allocated plane.
 *
 * @param p Plane to initialize.
 *
 * @kw name      Human-readable name for debugging.
 * @kw scroll    Scroll mode.
 * @kw z         Z-order for compositing.
 * @kw box       Box decoration properties.
 * @kw style     Default text style for this plane.
 * @kw allocator Allocator for internal allocations.
 * @kw canvas    Canvas to attach for font metrics (nullptr = none).
 *
 * @post Plane is visible, draw list is empty.
 */
extern void
n00b_plane_init(n00b_plane_t *p) _kargs
{
    n00b_option_t(n00b_string_t *) name = n00b_option_none(n00b_string_t *);
    n00b_scroll_mode_t scroll    = N00B_SCROLL_NONE;
    int32_t            z         = 0;
    n00b_box_props_t  *box       = nullptr;
    n00b_text_style_t *style     = nullptr;
    n00b_allocator_t  *allocator = nullptr;
    n00b_canvas_t     *canvas    = nullptr;
};

/**
 * @brief Destroy a plane and free its draw list.
 * @param p Plane to destroy.
 * @pre  Plane has been removed from any canvas/parent.
 */
extern void n00b_plane_destroy(n00b_plane_t *p);

// ====================================================================
// Hierarchy
// ====================================================================

/**
 * @brief Add a child plane at the given position.
 * @param parent Parent plane.
 * @param child  Child plane.
 * @param x      X offset in parent's content coordinates (pixels).
 * @param y      Y offset in parent's content coordinates (pixels).
 *
 * @pre  `child->parent` is nullptr (not yet parented).
 * @post `child->parent == parent`.
 */
extern void n00b_plane_add_child(n00b_plane_t *parent,
                                  n00b_plane_t *child,
                                  int32_t       x,
                                  int32_t       y);

/**
 * @brief Remove a child plane from its parent.
 * @param parent Parent plane.
 * @param child  Child plane to remove.
 * @return       true if the child was found and removed.
 *
 * @post On success, `child->parent` is nullptr.
 */
extern bool n00b_plane_remove_child(n00b_plane_t *parent,
                                     n00b_plane_t *child);

// ====================================================================
// Draw commands (all coordinates in pixels, relative to content origin)
// ====================================================================

/**
 * @brief Clear all draw commands.
 * @param p Plane to clear.
 */
extern void n00b_plane_clear(n00b_plane_t *p);

/**
 * @brief Query the content area size in pixels.
 * @param p   Plane.
 * @param out_w Output: content width in pixels.
 * @param out_h Output: content height in pixels.
 */
extern void n00b_plane_content_size(n00b_plane_t *p,
                                     int32_t *out_w,
                                     int32_t *out_h);

/**
 * @brief Draw styled text at a pixel position.
 * @param p    Plane.
 * @param x    X offset in pixels.
 * @param y    Y offset in pixels.
 * @param text String to draw.
 *
 * @kw style Style override (nullptr = use plane default).
 */
extern void
n00b_plane_draw_text(n00b_plane_t *p, int32_t x, int32_t y,
                      n00b_string_t *text) _kargs
{
    n00b_text_style_t *style = nullptr;
};

/**
 * @brief Draw a single codepoint at a pixel position.
 * @param p  Plane.
 * @param x  X offset in pixels.
 * @param y  Y offset in pixels.
 * @param cp Unicode codepoint.
 *
 * @kw style Style override (nullptr = use plane default).
 */
extern void
n00b_plane_draw_glyph(n00b_plane_t *p, int32_t x, int32_t y,
                       n00b_codepoint_t cp) _kargs
{
    n00b_text_style_t *style = nullptr;
};

/**
 * @brief Fill a rectangle with a codepoint and style.
 * @param p Plane.
 * @param x X offset in pixels.
 * @param y Y offset in pixels.
 * @param w Width in pixels.
 * @param h Height in pixels.
 *
 * @kw cp    Fill codepoint (default: space).
 * @kw style Fill style (nullptr = use plane default).
 */
extern void
n00b_plane_fill_rect(n00b_plane_t *p, int32_t x, int32_t y,
                      int32_t w, int32_t h) _kargs
{
    n00b_codepoint_t   cp    = ' ';
    n00b_text_style_t *style = nullptr;
};

// ====================================================================
// Text measurement (convenience wrappers using canvas metrics)
// ====================================================================

/**
 * @brief Measure the pixel width of styled text.
 * @param p     Plane (must be attached to a canvas).
 * @param text  String to measure.
 * @param style Style affecting font (nullptr = default).
 * @return Pixel width, or 0 if no metrics provider.
 */
extern int32_t n00b_plane_text_width(n00b_plane_t    *p,
                                      n00b_string_t   *text,
                                      n00b_text_style_t *style);

/**
 * @brief Get the line height for the given style.
 * @param p     Plane (must be attached to a canvas).
 * @param style Style affecting font (nullptr = default).
 * @return Pixel height of one line, or 0 if no metrics provider.
 */
extern int32_t n00b_plane_line_height(n00b_plane_t      *p,
                                       n00b_text_style_t *style);

/**
 * @brief Convert a pixel width to approximate character columns.
 *
 * Divides pixel width by the average character cell width from the
 * font metrics provider.  Used by widgets that need column counts
 * for Unicode line-breaking algorithms.
 *
 * @param p     Plane (must be attached to a canvas for accurate results).
 * @param px_w  Pixel width to convert.
 * @param style Style affecting font (nullptr = default).
 * @return Approximate character column count (minimum 1 if px_w > 0).
 */
extern int32_t n00b_plane_text_columns(n00b_plane_t      *p,
                                        int32_t            px_w,
                                        n00b_text_style_t *style);

// ====================================================================
// Viewport / scrolling
// ====================================================================

/**
 * @brief Scroll the viewport by a relative pixel offset.
 * @param p  Plane.
 * @param dx Horizontal delta (positive = right).
 * @param dy Vertical delta (positive = down).
 */
extern void n00b_plane_scroll(n00b_plane_t *p, int32_t dx, int32_t dy);

/**
 * @brief Scroll the viewport to an absolute pixel position.
 * @param p Plane.
 * @param x Viewport origin x.
 * @param y Viewport origin y.
 */
extern void n00b_plane_scroll_to(n00b_plane_t *p, int32_t x, int32_t y);

// ====================================================================
// Geometry
// ====================================================================

/**
 * @brief Move the plane to a new position in its parent (pixels).
 * @param p Plane.
 * @param x New x offset in pixels.
 * @param y New y offset in pixels.
 */
extern void n00b_plane_move(n00b_plane_t *p, int32_t x, int32_t y);

/**
 * @brief Change the plane's z-order.
 * @param p Plane.
 * @param z New z value.
 */
extern void n00b_plane_set_z(n00b_plane_t *p, int32_t z);

/**
 * @brief Show or hide the plane.
 * @param p       Plane.
 * @param visible true to show, false to hide.
 */
extern void n00b_plane_set_visible(n00b_plane_t *p, bool visible);

/**
 * @brief Mark the plane as needing a re-render.
 * @param p Plane.
 */
static inline void
n00b_plane_mark_dirty(n00b_plane_t *p)
{
    if (p) {
        p->flags |= N00B_PLANE_DIRTY;
        p->render_gen++;
    }
}

// ====================================================================
// Box decoration
// ====================================================================

/**
 * @brief Set or replace the box decoration.
 * @param p   Plane.
 * @param box New box properties (nullptr to remove).
 */
extern void n00b_plane_set_box(n00b_plane_t *p, n00b_box_props_t *box);

// ====================================================================
// Widget state
// ====================================================================

/**
 * @brief Set the widget state for style resolution.
 * @param p     Plane.
 * @param state New widget state.
 */
extern void n00b_plane_set_state(n00b_plane_t        *p,
                                  n00b_widget_state_t  state);

/**
 * @brief Query the current widget state.
 * @param p Plane.
 * @return  Current widget state.
 */
extern n00b_widget_state_t n00b_plane_get_state(n00b_plane_t *p);
