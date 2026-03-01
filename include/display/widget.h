/**
 * @file widget.h
 * @brief Widget behavior vtable for planes.
 *
 * A widget is a plane with attached behavior.  Rather than a separate
 * object hierarchy, we store a vtable pointer and an opaque data
 * pointer directly on `n00b_plane_t`.  This reuses the plane's grid,
 * box model, styles, and hierarchy for free.
 *
 * ### Lifecycle
 *
 * 1. Create a plane (or let a widget constructor do it).
 * 2. Call `n00b_widget_attach()` to bind a vtable + data.
 * 3. Call `n00b_widget_render()` whenever the widget needs to repaint.
 * 4. Call `n00b_widget_detach()` to unbind (calls `destroy` if set).
 *
 * A plane with `widget_vtable == nullptr` is a plain plane with no
 * widget behavior.
 */
#pragma once

#include "n00b.h"
#include "display/render/plane.h"
#include "display/event.h"

// ====================================================================
// Widget vtable
// ====================================================================

/**
 * @brief Behavior vtable for widget-ified planes.
 *
 * All function pointers are optional (nullptr = no-op).
 */
typedef struct n00b_widget_vtable_t {
    const char *kind;  /**< Widget type name ("label", "button", …). */

    /**
     * @brief Tear down widget-specific data.
     *
     * Called by `n00b_widget_detach()`.  The plane itself is
     * **not** freed — only the widget data should be cleaned up.
     */
    void (*destroy)(n00b_plane_t *plane, void *data);

    /**
     * @brief Render widget content into the plane's cell grid.
     *
     * Implementations should clear the grid (or the relevant region)
     * and write cells via the plane's `put_*` APIs.
     */
    void (*render)(n00b_plane_t *plane, void *data);

    /**
     * @brief Report preferred and minimum sizes for layout.
     *
     * @param plane     The widget's plane.
     * @param data      Widget-specific data.
     * @param pref_cols Output: preferred column count.
     * @param pref_rows Output: preferred row count.
     * @param min_cols  Output: minimum usable column count.
     * @param min_rows  Output: minimum usable row count.
     */
    void (*measure)(n00b_plane_t *plane, void *data,
                    n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
                    n00b_isize_t *min_cols,  n00b_isize_t *min_rows);

    /**
     * @brief Handle an input event.
     *
     * @param plane The widget's plane.
     * @param data  Widget-specific data.
     * @param event The event to handle.
     * @return      true if the event was consumed, false to propagate.
     */
    bool (*handle_event)(n00b_plane_t *plane, void *data,
                         const n00b_event_t *event);

    /**
     * @brief Report whether this widget can receive focus.
     *
     * @param plane The widget's plane.
     * @param data  Widget-specific data.
     * @return      true if the widget is focusable.
     */
    bool (*can_focus)(n00b_plane_t *plane, void *data);
} n00b_widget_vtable_t;

// ====================================================================
// Attach / detach / render
// ====================================================================

/**
 * @brief Attach a widget vtable and data to a plane.
 *
 * @pre `plane->widget_vtable` is nullptr (not already attached).
 */
extern void n00b_widget_attach(n00b_plane_t                    *plane,
                                const n00b_widget_vtable_t      *vtable,
                                void                            *data);

/**
 * @brief Detach widget behavior from a plane.
 *
 * Calls `vtable->destroy` if non-null, then clears both fields.
 * Safe to call on a plane with no widget attached (no-op).
 */
extern void n00b_widget_detach(n00b_plane_t *plane);

/**
 * @brief Invoke the widget's render callback.
 *
 * No-op if no widget is attached.  Marks the plane dirty after render.
 */
extern void n00b_widget_render(n00b_plane_t *plane);

/**
 * @brief Invoke the widget's measure callback.
 *
 * No-op (zeros all outputs) if no widget is attached or if the vtable
 * has no measure function.
 */
extern void n00b_widget_measure(n00b_plane_t *plane,
                                 n00b_isize_t *pref_cols,
                                 n00b_isize_t *pref_rows,
                                 n00b_isize_t *min_cols,
                                 n00b_isize_t *min_rows);

/**
 * @brief Dispatch an event to a widget.
 *
 * No-op (returns false) if no widget is attached or if the vtable
 * has no handle_event function.
 */
extern bool n00b_widget_handle_event(n00b_plane_t       *plane,
                                      const n00b_event_t *event);

/**
 * @brief Query whether a widget can receive focus.
 *
 * Returns false if no widget is attached or if the vtable has no
 * can_focus function.
 */
extern bool n00b_widget_can_focus(n00b_plane_t *plane);
