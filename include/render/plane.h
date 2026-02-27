/**
 * @file plane.h
 * @brief Render plane: a compositing surface with a cell grid.
 *
 * A plane (`n00b_plane_t`) is the primary content container in the
 * rendering system.  It owns a row-major grid of `n00b_rcell_t` cells,
 * a write cursor, optional box decorations, and viewport/scroll state.
 *
 * Planes form a hierarchy (parent/child) for nested composition.
 * Each plane has a position (x, y) in its parent's coordinate space
 * and a z-order for compositing.
 *
 * ### Content model
 *
 * The grid stores pure content.  Box decorations (borders, padding)
 * are overlaid during compositing, not stored in the grid.  All
 * `put_*` APIs operate in content coordinates (0,0 = first usable
 * cell inside border+padding).
 *
 * ### Related modules
 *
 * - `render/cell.h` — cell type stored in the grid
 * - `render/types.h` — box props, alignment, scroll mode
 * - `render/canvas.h` — canvas composites planes into frames
 * - `render/box.h` — box stamping during compositing
 */
#pragma once

#include "n00b.h"
#include "core/option.h"
#include "core/string.h"
#include "core/list.h"
#include "render/cell.h"
#include "render/types.h"
#include "strings/text_style.h"

// n00b_option_t(n00b_string_t) is declared in core/string.h.

// Type-safe option for cell pointer lookups.
typedef const n00b_rcell_t *n00b_const_rcell_ptr_t;
n00b_option_decl(n00b_const_rcell_ptr_t);

// Type-safe list for child planes.
typedef struct n00b_plane_t *n00b_plane_ptr_t;
n00b_list_decl(n00b_plane_ptr_t);

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
    n00b_string_t        name;
    struct n00b_plane_t              *parent;
    n00b_list_t(n00b_plane_ptr_t)    children;

    // Grid storage (row-major)
    n00b_rcell_t        *grid;
    n00b_isize_t         total_rows;
    n00b_isize_t         total_cols;

    // Viewport (visible region of the grid)
    n00b_isize_t         vp_row;
    n00b_isize_t         vp_col;
    n00b_isize_t         vp_rows;
    n00b_isize_t         vp_cols;

    // Position in parent's coordinate space
    int32_t              x;
    int32_t              y;
    int32_t              z;

    // Write cursor (in content coordinates)
    n00b_isize_t         cursor_row;
    n00b_isize_t         cursor_col;

    // Ring buffer for auto-scroll
    n00b_isize_t         ring_base;
    n00b_isize_t         ring_len;

    // Decoration
    n00b_box_props_t    *box;
    n00b_text_style_t   *default_style;

    // Behavior
    n00b_scroll_mode_t   scroll_mode;
    n00b_widget_state_t  widget_state;
    uint16_t             flags;

    n00b_rwlock_t       *lock;
    n00b_allocator_t    *allocator;
} n00b_plane_t;

// ====================================================================
// Lifecycle
// ====================================================================

/**
 * @brief Initialize a pre-allocated plane with the given dimensions.
 *
 * @param p Plane to initialize.
 *
 * @kw cols     Total columns in the content grid (default 80).
 * @kw rows     Total rows in the content grid (default 25).
 * @kw vp_cols  Viewport width (0 = same as cols).
 * @kw vp_rows  Viewport height (0 = same as rows).
 * @kw name     Human-readable name for debugging.
 * @kw scroll   Scroll mode.
 * @kw z        Z-order for compositing.
 * @kw box      Box decoration properties.
 * @kw style    Default text style for this plane.
 * @kw allocator Allocator for internal allocations.
 *
 * @post Plane is visible, grid is zero-filled, cursor at (0,0).
 */
extern void
n00b_plane_init(n00b_plane_t *p) _kargs
{
    n00b_isize_t       cols      = 80;
    n00b_isize_t       rows      = 25;
    n00b_isize_t       vp_cols   = 0;
    n00b_isize_t       vp_rows   = 0;
    n00b_option_t(n00b_string_t) name = n00b_option_none(n00b_string_t);
    n00b_scroll_mode_t scroll    = N00B_SCROLL_NONE;
    int32_t            z         = 0;
    n00b_box_props_t  *box       = nullptr;
    n00b_text_style_t *style     = nullptr;
    n00b_allocator_t  *allocator = nullptr;
};

/**
 * @brief Destroy a plane and free its grid.
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
 * @param x      X offset in parent's content coordinates.
 * @param y      Y offset in parent's content coordinates.
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
// Content writing
// ====================================================================

/**
 * @brief Write a styled string at the cursor, advancing the cursor.
 * @param p Plane to write to.
 * @param s String to write (with embedded style info).
 *
 * @kw wrap If true, wrap at plane boundary (default true).
 *
 * @post Cursor advances past the written content.
 */
extern void
n00b_plane_put_str(n00b_plane_t *p, n00b_string_t s) _kargs
{
    bool wrap = true;
};

/**
 * @brief Write a styled string at a specific position.
 * @param p   Plane to write to.
 * @param row Row in content coordinates.
 * @param col Column in content coordinates.
 * @param s   String to write.
 */
extern void n00b_plane_put_str_at(n00b_plane_t *p,
                                   n00b_isize_t  row,
                                   n00b_isize_t  col,
                                   n00b_string_t s);

/**
 * @brief Write a single codepoint at the cursor.
 * @param p  Plane to write to.
 * @param cp Unicode codepoint to write.
 *
 * @kw style Style override (nullptr = use plane default).
 */
extern void
n00b_plane_put_cp(n00b_plane_t *p, n00b_codepoint_t cp) _kargs
{
    n00b_text_style_t *style = nullptr;
};

/**
 * @brief Advance the cursor to the next line.
 * @param p Plane.
 */
extern void n00b_plane_newline(n00b_plane_t *p);

// ====================================================================
// Cell access
// ====================================================================

/**
 * @brief Get a read-only pointer to a cell.
 * @param p   Plane.
 * @param row Row in content coordinates.
 * @param col Column in content coordinates.
 * @return    Option containing cell pointer, or none if out of bounds.
 */
extern n00b_option_t(n00b_const_rcell_ptr_t)
    n00b_plane_get_cell(n00b_plane_t *p,
                         n00b_isize_t  row,
                         n00b_isize_t  col);

// ====================================================================
// Clear / fill
// ====================================================================

/**
 * @brief Clear all cells in the plane.
 * @param p Plane to clear.
 * @post  All cells are empty, cursor reset to (0,0).
 */
extern void n00b_plane_clear(n00b_plane_t *p);

/**
 * @brief Fill a rectangular region with a character and style.
 * @param p    Plane.
 * @param row  Start row.
 * @param col  Start column.
 * @param rows Number of rows.
 * @param cols Number of columns.
 *
 * @kw cp    Fill codepoint (default: space).
 * @kw style Fill style (nullptr = plane default).
 */
extern void
n00b_plane_fill_rect(n00b_plane_t *p,
                      n00b_isize_t  row,
                      n00b_isize_t  col,
                      n00b_isize_t  rows,
                      n00b_isize_t  cols) _kargs
{
    n00b_codepoint_t   cp    = ' ';
    n00b_text_style_t *style = nullptr;
};

// ====================================================================
// Cursor
// ====================================================================

/**
 * @brief Move the write cursor.
 * @param p   Plane.
 * @param row Target row.
 * @param col Target column.
 */
extern void n00b_plane_cursor_move(n00b_plane_t *p,
                                    n00b_isize_t  row,
                                    n00b_isize_t  col);

// ====================================================================
// Viewport / scrolling
// ====================================================================

/**
 * @brief Scroll the viewport by a relative offset.
 * @param p    Plane.
 * @param drow Row delta (positive = down).
 * @param dcol Column delta (positive = right).
 */
extern void n00b_plane_scroll(n00b_plane_t *p, int32_t drow, int32_t dcol);

/**
 * @brief Scroll the viewport to an absolute position.
 * @param p   Plane.
 * @param row Viewport origin row.
 * @param col Viewport origin column.
 */
extern void n00b_plane_scroll_to(n00b_plane_t *p,
                                  n00b_isize_t  row,
                                  n00b_isize_t  col);

// ====================================================================
// Geometry
// ====================================================================

/**
 * @brief Move the plane to a new position in its parent.
 * @param p Plane.
 * @param x New x offset.
 * @param y New y offset.
 */
extern void n00b_plane_move(n00b_plane_t *p, int32_t x, int32_t y);

/**
 * @brief Change the plane's z-order.
 * @param p Plane.
 * @param z New z value.
 */
extern void n00b_plane_set_z(n00b_plane_t *p, int32_t z);

/**
 * @brief Resize the content grid.
 * @param p    Plane.
 * @param rows New row count.
 * @param cols New column count.
 *
 * @post Existing content is preserved where it fits; new cells are empty.
 */
extern void n00b_plane_resize(n00b_plane_t *p,
                               n00b_isize_t  rows,
                               n00b_isize_t  cols);

/**
 * @brief Show or hide the plane.
 * @param p       Plane.
 * @param visible true to show, false to hide.
 */
extern void n00b_plane_set_visible(n00b_plane_t *p, bool visible);

// ====================================================================
// Box decoration
// ====================================================================

/**
 * @brief Set or replace the box decoration.
 * @param p   Plane.
 * @param box New box properties (nullptr to remove).
 */
extern void n00b_plane_set_box(n00b_plane_t *p, n00b_box_props_t *box);

/**
 * @brief Query the usable content area (inside border + padding).
 * @param p        Plane.
 * @param out_rows Output: available content rows.
 * @param out_cols Output: available content columns.
 */
extern void n00b_plane_content_size(n00b_plane_t *p,
                                     n00b_isize_t *out_rows,
                                     n00b_isize_t *out_cols);

// ====================================================================
// Widget state
// ====================================================================

/**
 * @brief Set the widget state for style resolution.
 * @param p     Plane.
 * @param state New widget state.
 */
extern void n00b_plane_set_state(n00b_plane_t    *p,
                                  n00b_widget_state_t state);

/**
 * @brief Query the current widget state.
 * @param p Plane.
 * @return  Current widget state.
 */
extern n00b_widget_state_t n00b_plane_get_state(n00b_plane_t *p);
