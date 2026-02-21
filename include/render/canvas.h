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
#include "render/cell.h"
#include "render/plane.h"
#include "render/backend.h"

// ====================================================================
// Canvas structure
// ====================================================================

typedef struct n00b_canvas_t {
    const n00b_renderer_vtable_t *vtable;
    void                         *backend_ctx;
    n00b_render_cap_t             caps;

    n00b_rcell_t                 *frame;
    n00b_rcell_t                 *prev_frame;
    n00b_isize_t                  frame_rows;
    n00b_isize_t                  frame_cols;

    n00b_list_t(n00b_plane_ptr_t) planes;

    n00b_text_style_t            *default_style;
    bool                          needs_full_redraw;
    bool                          size_set;
    n00b_rwlock_t                *lock;
    n00b_allocator_t             *allocator;
} n00b_canvas_t;

// ====================================================================
// Lifecycle
// ====================================================================

/**
 * @brief Initialize a pre-allocated canvas with the given backend.
 * @param c Canvas to initialize.
 *
 * @kw vtable    Renderer vtable (must not be nullptr).
 * @kw allocator Allocator for internal allocations (nullptr = runtime default).
 * @kw output    Output topic for the backend (nullptr = none).
 *
 * @post Canvas is ready; backend is initialized.
 */
extern void
n00b_canvas_init(n00b_canvas_t *c) _kargs
{
    const n00b_renderer_vtable_t           *vtable    = nullptr;
    n00b_allocator_t                       *allocator = nullptr;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output    = nullptr;
};

/**
 * @brief Destroy a canvas, its backend, and free resources.
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
 * @param rows New row count.
 * @param cols New column count.
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
 * @brief Enter alternate screen (if supported).
 * @param c Canvas.
 */
extern void n00b_canvas_alt_screen_enter(n00b_canvas_t *c);

/**
 * @brief Leave alternate screen (if supported).
 * @param c Canvas.
 */
extern void n00b_canvas_alt_screen_leave(n00b_canvas_t *c);
