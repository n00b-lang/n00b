/**
 * @file xform_render.h
 * @brief Pipeline transforms connecting conduit to the n00b render system.
 *
 * Provides two transforms:
 *
 * - **render_out**: `n00b_plane_t * -> n00b_buffer_t *`
 *   Renders a plane to a byte buffer using a configurable renderer backend
 *   (defaults to the stream backend for plain text, or ANSI inline for
 *   styled terminal output).
 *
 * - **render_in**: `n00b_buffer_t * -> n00b_plane_t *`
 *   Writes raw bytes into a plane as text content, producing a plane
 *   suitable for compositing with other planes.
 *
 * ### Usage
 *
 * ```c
 * // Render planes to ANSI bytes:
 * auto r = n00b_conduit_render_out_new(conduit, upstream,
 *     .backend = &n00b_renderer_ansi_inline, .width = 80, .height = 25);
 * ```
 */
#pragma once

#include "conduit/xform.h"
#include "conduit/xform_types.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/backend.h"

// ============================================================================
// Type instantiations for n00b_plane_t *
// ============================================================================

N00B_CONDUIT_FULL_IMPL(n00b_plane_t *);

// Heterogeneous: plane -> buffer
N00B_CONDUIT_XFORM_IMPL(n00b_plane_t *, n00b_buffer_t *);

// Heterogeneous: buffer -> plane
N00B_CONDUIT_XFORM_IMPL(n00b_buffer_t *, n00b_plane_t *);

// ============================================================================
// render_out: n00b_plane_t * -> n00b_buffer_t *
// ============================================================================

/**
 * @brief Create a transform that renders planes to byte buffers.
 *
 * Each input `n00b_plane_t *` is rendered through the given backend
 * (or the stream backend by default) and emitted as a `n00b_buffer_t *`.
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_plane_t *` payloads.
 *
 * @kw backend Renderer vtable (default: `&n00b_renderer_stream`).
 * @kw width   Output column width (default: 80).
 * @kw height  Output row height (default: 25).
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_plane_t *, n00b_buffer_t *) *)
n00b_conduit_render_out_new(
    n00b_conduit_t                       *c,
    n00b_conduit_topic_t(n00b_plane_t *) *upstream)
    _kargs {
        const n00b_renderer_vtable_t *backend = nullptr;
        int32_t                       width   = 80;
        int32_t                       height  = 25;
    };

// ============================================================================
// render_in: n00b_buffer_t * -> n00b_plane_t *
// ============================================================================

/**
 * @brief Create a transform that writes bytes into planes.
 *
 * Each input `n00b_buffer_t *` is written as text into a fresh
 * `n00b_plane_t *` and emitted downstream.  Useful for feeding raw
 * text into the render compositor.
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_buffer_t *` payloads.
 *
 * @kw width  Plane column width (default: 80).
 * @kw height Plane row count (default: 25).
 * @kw style  Default text style for the plane (nullable).
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_plane_t *) *)
n00b_conduit_render_in_new(
    n00b_conduit_t                        *c,
    n00b_conduit_topic_t(n00b_buffer_t *) *upstream)
    _kargs {
        int32_t            width  = 80;
        int32_t            height = 25;
        n00b_text_style_t *style  = nullptr;
    };
