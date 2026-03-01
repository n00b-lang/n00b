/**
 * @file event_loop.h
 * @brief Event loop for interactive canvas applications.
 *
 * `n00b_canvas_run()` enters a blocking event loop that polls the
 * backend for input, dispatches events to widgets via the focus
 * manager, and re-renders the canvas when dirty.
 *
 * ### Loop behavior
 *
 * 1. Poll backend for events (with timeout).
 * 2. Tab → focus next, Shift+Tab → focus prev.
 * 3. Ctrl+C → quit.
 * 4. Other events → dispatch to focused widget.
 * 5. If any plane is dirty → re-render canvas.
 */
#pragma once

#include "n00b.h"
#include "display/render/canvas.h"
#include "display/focus.h"
#include "display/event.h"

// ====================================================================
// Event loop
// ====================================================================

/**
 * @brief Resize callback signature.
 *
 * Called by the event loop when a resize event is processed, after
 * the canvas dimensions have been updated.
 *
 * @param canvas The resized canvas.
 * @param data   User-provided context pointer.
 */
typedef void (*n00b_resize_cb_t)(n00b_canvas_t *canvas, void *data);

/**
 * @brief Run the interactive event loop.
 *
 * Blocks until Ctrl+C is pressed or `n00b_canvas_run_stop()` is
 * called from an event handler.
 *
 * @param canvas The canvas to run.
 *
 * @kw tick_ms     Poll timeout in milliseconds (default 16 ~= 60fps).
 * @kw on_resize   Callback invoked after a resize event is processed.
 * @kw resize_data User context passed to on_resize.
 */
extern void
n00b_canvas_run(n00b_canvas_t *canvas) _kargs
{
    int32_t          tick_ms     = 16;
    n00b_resize_cb_t on_resize   = nullptr;
    void            *resize_data = nullptr;
};

/**
 * @brief Signal the event loop to stop.
 *
 * Safe to call from any event handler.  The loop will exit on the
 * next iteration.
 */
extern void n00b_canvas_run_stop(void);
