/**
 * @file mouse.h
 * @brief Mouse hit-testing, event routing, and capture API.
 *
 * Provides the plumbing between raw mouse events (produced by backends)
 * and the widget system.  The event loop calls `n00b_mouse_route_event()`
 * which performs hit-testing on the plane tree, optionally focuses the
 * target, and dispatches through `n00b_widget_handle_event()` with
 * bubbling to parent planes.
 *
 * ### Mouse capture
 *
 * During drag operations a widget can capture the mouse so that all
 * subsequent events route to it regardless of pointer position.  Call
 * `n00b_canvas_capture_mouse()` on PRESS and
 * `n00b_canvas_release_mouse()` on RELEASE.
 */
#pragma once

#include "n00b.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/event.h"

// ====================================================================
// Hit testing
// ====================================================================

/**
 * @brief Find the deepest visible plane containing pixel (x, y).
 *
 * Flattens `plane` using the compositor's ordering rules, then walks the
 * resulting entries from front to back. Skips planes without
 * `N00B_PLANE_VISIBLE`.
 *
 * @param plane     Root plane to test.
 * @param x         Absolute pixel column in the frame.
 * @param y         Absolute pixel row in the frame.
 * @param cell_px_w Pixels per cell column.
 * @param cell_px_h Pixels per cell row.
 * @return          Deepest hit plane, or nullptr if no hit.
 */
extern n00b_plane_t *n00b_mouse_hit_test(n00b_plane_t *plane,
                                          int32_t       x,
                                          int32_t       y,
                                          int32_t       cell_px_w,
                                          int32_t       cell_px_h);

// ====================================================================
// Event routing
// ====================================================================

/**
 * @brief Route a mouse event through the canvas plane tree.
 *
 * 1. If `canvas->mouse_capture` is set, route directly to that plane.
 * 2. Otherwise, flatten the top-level plane tree and hit-test the same
 *    ordered entries the renderer used.
 * 3. If the hit plane is focusable and action is PRESS, focus it.
 * 4. Dispatch via `n00b_widget_handle_event()`.
 * 5. If not consumed, bubble to `target->parent` until consumed or root.
 *
 * @param canvas Canvas owning the plane tree.
 * @param fm     Focus manager (for click-to-focus).
 * @param event  Mouse event to route.
 */
extern void n00b_mouse_route_event(n00b_canvas_t          *canvas,
                                    struct n00b_focus_mgr_t *fm,
                                    const n00b_event_t      *event);

// ====================================================================
// Mouse capture
// ====================================================================

/**
 * @brief Capture all mouse events to a specific plane (for drag).
 * @param c     Canvas.
 * @param plane Plane to receive all mouse events.
 */
extern void n00b_canvas_capture_mouse(n00b_canvas_t *c, n00b_plane_t *plane);

/**
 * @brief Release the mouse capture.
 * @param c Canvas.
 */
extern void n00b_canvas_release_mouse(n00b_canvas_t *c);

/**
 * @brief Get the plane currently capturing mouse events.
 * @param c Canvas.
 * @return  Capturing plane, or nullptr.
 */
extern n00b_plane_t *n00b_canvas_get_mouse_capture(n00b_canvas_t *c);
