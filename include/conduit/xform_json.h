/**
 * @file xform_json.h
 * @brief JSON conduit transforms: parse and encode.
 *
 * Three transforms:
 * - **json_parse**: `n00b_buffer_t * -> n00b_json_node_t *`
 *   Accumulates input bytes, parses complete JSON values, emits value trees.
 * - **json_encode**: `n00b_json_node_t * -> n00b_buffer_t *`
 *   Encodes value trees to JSON text buffers.
 *
 * ### Usage
 *
 * ```c
 * auto r = n00b_conduit_json_parse_new(c, upstream);
 * auto xf = n00b_result_get(r);
 * auto out = n00b_conduit_xform_topic(n00b_buffer_t *, n00b_json_node_t *, xf);
 * ```
 */
#pragma once

#include "conduit/xform_types.h"
#include "parsers/json.h"

// ============================================================================
// Type instantiations
// ============================================================================

n00b_option_decl(n00b_json_node_t *);
N00B_CONDUIT_FULL_IMPL(n00b_json_node_t *);
N00B_CONDUIT_XFORM_IMPL(n00b_buffer_t *, n00b_json_node_t *);
N00B_CONDUIT_XFORM_IMPL(n00b_json_node_t *, n00b_buffer_t *);

// ============================================================================
// JSON parse transform state
// ============================================================================

typedef struct {
    uint8_t *buf;       /**< Accumulated input buffer. */
    size_t   buf_len;   /**< Current data length. */
    size_t   buf_cap;   /**< Buffer capacity. */
    size_t   max_depth; /**< Nesting limit (default 256). */
    size_t   max_size;  /**< Max input size (0 = unlimited). */
} n00b_json_parse_state_t;

// ============================================================================
// JSON encode transform state
// ============================================================================

typedef struct {
    bool pretty; /**< Pretty-print output. */
    int  indent; /**< Indent width (spaces). */
} n00b_json_encode_state_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Create a JSON parse transform (buffer -> node).
 *
 * Accumulates input bytes and emits complete `n00b_json_node_t *`
 * value trees.  Supports multiple JSON values per stream.
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_buffer_t *` payloads.
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_json_node_t *) *)
n00b_conduit_json_parse_new(
    n00b_conduit_t                        *c,
    n00b_conduit_topic_t(n00b_buffer_t *) *upstream);

/**
 * @brief Create a JSON encode transform (node -> buffer).
 *
 * Each input `n00b_json_node_t *` is encoded to a `n00b_buffer_t *`
 * containing the JSON text.
 *
 * @param c        Conduit instance.
 * @param upstream Upstream topic producing `n00b_json_node_t *` payloads.
 * @kw pretty     Enable indented output (default false).
 * @kw indent     Indent width in spaces (default 2).
 *
 * @return Result with xform pointer on success.
 */
extern n00b_result_t(n00b_conduit_xform_t(n00b_json_node_t *, n00b_buffer_t *) *)
n00b_conduit_json_encode_new(
    n00b_conduit_t                             *c,
    n00b_conduit_topic_t(n00b_json_node_t *)  *upstream)
    _kargs {
        bool pretty = false;
        int  indent = 2;
    };
