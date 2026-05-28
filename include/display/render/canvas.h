/**
 * @file canvas.h
 * @brief Canvas: compositing surface with pluggable renderer backend.
 *
 * The canvas owns a frame buffer, a list of top-level planes, and a
 * reference to a renderer backend.  On each `n00b_canvas_render()`
 * call, it flattens the plane hierarchy, composites into the frame,
 * optionally diffs against the previous frame, and dispatches to the
 * backend for output.
 *
 * ### Related modules
 *
 * - `render/plane.h` — planes composited by the canvas
 * - `render/composite.h` — compositing pipeline
 * - `render/backend.h` — renderer vtable
 */
#pragma once

#include "n00b.h"
#include "display/render/cell.h"
#include "display/render/plane.h"
#include "display/render/font_metrics.h"
#include "display/render/backend.h"

// ====================================================================
// Canvas structure
// ====================================================================

typedef struct n00b_canvas_t {
    const n00b_renderer_vtable_t *vtable;
    void                         *backend_ctx;
    int                           backend_error;
    n00b_render_cap_t             caps;

    n00b_isize_t                  frame_rows;  /**< Frame height in pixels. */
    n00b_isize_t                  frame_cols;  /**< Frame width in pixels. */

    n00b_list_t(n00b_plane_ptr_t) planes;

    n00b_text_style_t            *default_style;

    n00b_isize_t                  cell_px_w;  /**< Pixels per cell column (1 for cell-only backends). */
    n00b_isize_t                  cell_px_h;  /**< Pixels per cell row (1 for cell-only backends). */

    n00b_font_metrics_provider_t  metrics;    /**< Font metrics (from backend or fallback). */

    bool                          needs_full_redraw;
    bool                          size_set;
    n00b_rwlock_t                *lock;
    n00b_allocator_t             *allocator;

    // Focus manager attached to this canvas, or nullptr when none exists.
    struct n00b_focus_mgr_t      *focus;

    // Mouse capture (plane receiving all mouse events during drag, nullptr = hit-test).
    n00b_plane_t                 *mouse_capture;
} n00b_canvas_t;

// ====================================================================
// Lifecycle
// ====================================================================

/**
 * @brief Initialize a pre-allocated canvas with the given backend.
 * @param c Canvas to initialize.
 *
 * @kw vtable    Renderer vtable (optional direct backend path).
 * @kw backend_name Requested backend name (defaults to `auto` when no vtable is given).
 * @kw backend_allow_fallback     Append fallback candidates after explicit request.
 * @kw backend_allow_dynamic_load Allow dynamic plugin loading during backend resolve (default false).
 * @kw backend_allow_env_override Honor `$N00B_RENDERER_BACKEND` override in selection (default false).
 * @kw allocator Allocator for internal allocations (nullptr = runtime default).
 * @kw output    Output topic for the backend (nullptr = none).
 *
 * @post On success, `n00b_canvas_backend_ready(c)` returns true.
 */
extern void
n00b_canvas_init(n00b_canvas_t *c) _kargs
{
    const n00b_renderer_vtable_t           *vtable    = nullptr;
    n00b_string_t                          *backend_name = nullptr;
    bool                                    backend_allow_fallback = true;
    bool                                    backend_allow_dynamic_load = false;
    bool                                    backend_allow_env_override = false;
    n00b_allocator_t                       *allocator = nullptr;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output    = nullptr;
};

/**
 * @brief Report whether canvas startup successfully bound a backend.
 * @param c Canvas.
 * @return  true when the canvas has a usable backend binding.
 */
extern bool n00b_canvas_backend_ready(const n00b_canvas_t *c);

/**
 * @brief Return the last backend startup error recorded by the canvas.
 * @param c Canvas.
 * @return  0 when ready, otherwise the most recent startup error code.
 */
extern int n00b_canvas_backend_error(const n00b_canvas_t *c);

/**
 * @brief Tear down canvas backend/list resources without freeing @p c.
 * @param c Canvas to deinitialize.
 *
 * Safe to call on stack/embedded canvases, and safe to call more than once.
 * After return, the canvas may be reinitialized with `n00b_canvas_init()`.
 */
extern void n00b_canvas_deinit(n00b_canvas_t *c);

/**
 * @brief Destroy a heap-allocated canvas (`deinit` + free).
 * @param c Canvas to destroy.
 * @pre  All planes have been removed or are owned by the canvas.
 */
extern void n00b_canvas_destroy(n00b_canvas_t *c);

// ====================================================================
// Plane management
// ====================================================================

/**
 * @brief Add a top-level plane to the canvas.
 * @param c Canvas.
 * @param p Plane to add.
 */
extern void n00b_canvas_add_plane(n00b_canvas_t *c, n00b_plane_t *p);

/**
 * @brief Remove a top-level plane from the canvas.
 * @param c Canvas.
 * @param p Plane to remove.
 * @return  true if the plane was found and removed.
 */
extern bool n00b_canvas_remove_plane(n00b_canvas_t *c, n00b_plane_t *p);

// ====================================================================
// Rendering
// ====================================================================

/**
 * @brief Composite and render all planes to the backend.
 *
 * Pipeline: flatten → clear frame → composite → degrade → diff → render → swap.
 *
 * @param c Canvas.
 */
extern void n00b_canvas_render(n00b_canvas_t *c);

/**
 * @brief Mark the entire canvas as needing a full redraw.
 * @param c Canvas.
 */
extern void n00b_canvas_invalidate(n00b_canvas_t *c);

/**
 * @brief Resize the canvas frame buffer.
 * @param c    Canvas.
 * @param rows New height in pixels.
 * @param cols New width in pixels.
 */
extern void n00b_canvas_resize(n00b_canvas_t *c,
                                n00b_isize_t   rows,
                                n00b_isize_t   cols);

/**
 * @brief Flush backend output.
 * @param c Canvas.
 */
extern void n00b_canvas_flush(n00b_canvas_t *c);

/**
 * @brief Copy UTF-8 text through the active backend clipboard path.
 * @param c    Canvas.
 * @param text Text to copy.
 * @return     true on successful backend copy.
 */
extern bool n00b_canvas_clipboard_copy(n00b_canvas_t *c, n00b_string_t *text);

/**
 * @brief Enter alternate screen (if supported).
 * @param c Canvas.
 */
extern void n00b_canvas_alt_screen_enter(n00b_canvas_t *c);

/**
 * @brief Leave alternate screen (if supported).
 * @param c Canvas.
 */
extern void n00b_canvas_alt_screen_leave(n00b_canvas_t *c);
