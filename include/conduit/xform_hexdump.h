/**
 * @file xform_hexdump.h
 * @brief Hexdump conduit transform: byte stream to render plane.
 *
 * Transforms `n00b_buffer_t *` input into `n00b_plane_t *` output,
 * showing an incrementally-growing hex dump.  Partial lines are
 * displayed immediately and overwritten when the line completes.
 *
 * The output plane grows as data arrives.  Each line is styled with
 * deferred tags: `hexdump.offset` for the offset column and
 * `hexdump.ascii` for the ASCII sidebar.
 *
 * ### Usage
 *
 * ```c
 * auto r = n00b_conduit_hexdump_new(conduit, upstream, .width = 80);
 * auto xf = n00b_result_get(r);
 * auto out = n00b_conduit_xform_topic(n00b_buffer_t *, n00b_plane_t *, xf);
 * ```
 */
#pragma once

#include "conduit/xform_types.h"
#include "conduit/xform_render.h"   /* N00B_CONDUIT_XFORM_IMPL(buf, plane) */
#include "display/hexdump.h"
#include "display/render/plane.h"

// ============================================================================
// Hexdump xform state (stored in cookie)
// ============================================================================

/**
 * @brief Internal state for the hexdump transform.
 */
typedef struct {
    n00b_hexdump_t *hd;           /**< Hexdump formatting engine. */
    n00b_plane_t   *plane;        /**< Output render plane. */
    n00b_isize_t    current_row;  /**< Next row for a complete line. */
    n00b_isize_t    plane_cols;   /**< Column width of the plane. */
    n00b_isize_t    plane_cap;    /**< Current row capacity of the plane. */
    bool            has_partial;  /**< True if current_row holds a partial. */
} n00b_hexdump_xform_state_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Create a hexdump transform (buffer -> plane).
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_buffer_t *` payloads.
 *
 * @kw width         Terminal width in columns (0 = default 138).
 * @kw start_offset  Starting display address.
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_plane_t *) *)
n00b_conduit_hexdump_new(
    n00b_conduit_t                        *c,
    n00b_conduit_topic_t(n00b_buffer_t *) *upstream)
    _kargs {
        uint32_t width        = 0;
        int64_t  start_offset = 0;
    };

// ============================================================================
// Chain spec
// ============================================================================

/**
 * @brief Chain specification for the hexdump transform.
 */
typedef struct {
    n00b_conduit_xform_spec_base_t base;
    uint32_t                       width;
    int64_t                        start_offset;
} n00b_conduit_hexdump_spec_t;

extern const n00b_conduit_hexdump_spec_t n00b_conduit_hexdump_default_spec;
