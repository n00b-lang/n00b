/**
 * @file widget.h
 * @brief Widget behavior vtable for planes.
 *
 * A widget is a plane with attached behavior.  Rather than a separate
 * object hierarchy, we store a vtable pointer and an opaque data
 * pointer directly on `n00b_plane_t`.  This reuses the plane's draw
 * list, box model, styles, and hierarchy for free.
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
     * @brief Render widget content into the plane's draw list.
     *
     * Implementations should clear the draw list and issue draw
     * commands via `n00b_plane_draw_text()`, `n00b_plane_draw_glyph()`,
     * and `n00b_plane_fill_rect()`.
     */
    void (*render)(n00b_plane_t *plane, void *data);

    /**
     * @brief Report preferred and minimum sizes in pixels.
     *
     * @param plane  The widget's plane.
     * @param data   Widget-specific data.
     * @param pref_w Output: preferred width in pixels.
     * @param pref_h Output: preferred height in pixels.
     * @param min_w  Output: minimum width in pixels.
     * @param min_h  Output: minimum height in pixels.
     */
    void (*measure)(n00b_plane_t *plane, void *data,
                    int32_t *pref_w, int32_t *pref_h,
                    int32_t *min_w,  int32_t *min_h);

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

    /**
     * @brief Position and size the widget within the given pixel bounds.
     *
     * Container widgets override this to recursively lay out children.
     * Leaf widgets can leave this nullptr — the default
     * `n00b_widget_layout()` handles move + viewport sizing.
     *
     * @param plane  The widget's plane.
     * @param data   Widget-specific data.
     * @param bounds Pixel bounding rectangle assigned by the parent.
     */
    void (*layout)(n00b_plane_t *plane, void *data, n00b_rect_t bounds);
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
 * No-op if no widget is attached.  Marks the plane dirty so the
 * compositor picks up the new content.
 */
extern void n00b_widget_render(n00b_plane_t *plane);

/**
 * @brief Invoke the widget's measure callback (pixels).
 *
 * No-op (zeros all outputs) if no widget is attached or if the vtable
 * has no measure function.
 */
extern void n00b_widget_measure(n00b_plane_t *plane,
                                 int32_t *pref_w, int32_t *pref_h,
                                 int32_t *min_w,  int32_t *min_h);

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

// ====================================================================
// Layout
// ====================================================================

/**
 * @brief Lay out a widget within pixel bounds.
 *
 * Stores bounds on the plane, computes viewport size (bounds minus
 * box insets), calls `n00b_plane_move()`, marks dirty, and invokes
 * the widget's custom `layout` vtable slot if present.
 *
 * @param plane  The widget's plane.
 * @param bounds Pixel bounding rectangle.
 */
extern void n00b_widget_layout(n00b_plane_t *plane,
                                n00b_rect_t   bounds);
