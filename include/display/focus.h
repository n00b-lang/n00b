/**
 * @file focus.h
 * @brief Focus manager for the widget system.
 *
 * Tracks focusable planes in tab order (depth-first tree walk).
 * Tab/Shift+Tab cycle focus.  FOCUS/BLUR events fire on transitions,
 * setting `widget_state` to FOCUSED/NORMAL.
 *
 * ### Usage
 *
 * ```c
 * n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
 * (void)n00b_focus_mgr_next(fm);   // Tab
 * (void)n00b_focus_mgr_prev(fm);   // Shift+Tab
 * n00b_option_t(n00b_plane_t *) cur = n00b_focus_mgr_current(fm);
 * ```
 */
#pragma once

#include "n00b.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/canvas.h"
#include "display/widget.h"

// ====================================================================
// Focus manager
// ====================================================================

/**
 * @brief Focus manager state.
 *
 * Holds a flat list of focusable planes (built by depth-first walk)
 * and a current focus index.
 */
typedef struct n00b_focus_mgr_t {
    n00b_canvas_t  *canvas;
    n00b_plane_t  **focusable;   /**< Array of focusable planes. */
    n00b_isize_t    count;       /**< Number of focusable planes. */
    n00b_isize_t    current;     /**< Index of currently focused plane (== count means none). */
    n00b_isize_t    capacity;    /**< Allocated capacity of focusable array. */
} n00b_focus_mgr_t;

// ====================================================================
// Lifecycle
// ====================================================================

/**
 * @brief Create a new focus manager for a canvas.
 *
 * Performs an initial rebuild of the focusable plane list.
 *
 * @param canvas The canvas to manage focus for.
 * @return       A heap-allocated focus manager.
 */
extern n00b_focus_mgr_t *n00b_focus_mgr_new(n00b_canvas_t *canvas);

/**
 * @brief Destroy a focus manager and free its resources.
 */
extern void n00b_focus_mgr_destroy(n00b_focus_mgr_t *fm);

// ====================================================================
// Navigation
// ====================================================================

/**
 * @brief Rebuild the focusable plane list from the canvas tree.
 *
 * Call after adding/removing planes or changing can_focus results.
 */
extern void n00b_focus_mgr_rebuild(n00b_focus_mgr_t *fm);

/**
 * @brief Move focus to the next focusable widget (Tab).
 * @return Some(newly focused plane), or None if no focusable widgets.
 */
extern n00b_option_t(n00b_plane_t *) n00b_focus_mgr_next(n00b_focus_mgr_t *fm);

/**
 * @brief Move focus to the previous focusable widget (Shift+Tab).
 * @return Some(newly focused plane), or None if no focusable widgets.
 */
extern n00b_option_t(n00b_plane_t *) n00b_focus_mgr_prev(n00b_focus_mgr_t *fm);

/**
 * @brief Set focus to a specific plane.
 * @return true if the plane was found and focused.
 */
extern bool n00b_focus_mgr_set(n00b_focus_mgr_t *fm, n00b_plane_t *plane);

/**
 * @brief Get the currently focused plane.
 * @return Some(focused plane), or None if no plane is focused.
 */
extern n00b_option_t(n00b_plane_t *) n00b_focus_mgr_current(n00b_focus_mgr_t *fm);
