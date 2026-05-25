/**
 * @file xform_marshal.h
 * @brief Marshal conduit transforms for object graph byte streams.
 *
 * Two transforms:
 * - **marshal**: `n00b_marshal_object_t -> n00b_buffer_t *`
 *   Marshals each input object graph into one byte-stream buffer.
 * - **unmarshal**: `n00b_buffer_t * -> n00b_marshal_object_t`
 *   Accepts incremental byte chunks and emits reconstructed roots.
 */
#pragma once

#include "conduit/xform_types.h"
#include "util/marshal.h"

// ============================================================================
// Type instantiations
// ============================================================================

typedef void *n00b_marshal_object_t;

N00B_CONDUIT_FULL_IMPL(n00b_marshal_object_t);
N00B_CONDUIT_XFORM_IMPL(n00b_marshal_object_t, n00b_buffer_t *);
N00B_CONDUIT_XFORM_IMPL(n00b_buffer_t *, n00b_marshal_object_t);

// ============================================================================
// Transform state
// ============================================================================

typedef struct {
    uint32_t              flags;
    uint32_t              base_address;
    n00b_marshal_status_t status;
    n00b_string_t        *error;
} n00b_conduit_marshal_state_t;

typedef struct {
    n00b_unmarshal_ctx_t *ctx;
    n00b_arena_t         *target_arena;
    n00b_marshal_status_t status;
    n00b_string_t        *error;
} n00b_conduit_unmarshal_state_t;

// ============================================================================
// API
// ============================================================================

extern n00b_result_t(n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *)
n00b_conduit_marshal_new(
    n00b_conduit_t                                  *c,
    n00b_conduit_topic_t(n00b_marshal_object_t)     *upstream)
    _kargs {
        uint32_t flags        = N00B_MARSHAL_F_NONE;
        uint32_t base_address = 0;
    };

extern n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *)
n00b_conduit_unmarshal_new(
    n00b_conduit_t                              *c,
    n00b_conduit_topic_t(n00b_buffer_t *)       *upstream)
    _kargs {
        n00b_arena_t *target_arena = nullptr;
    };

extern n00b_marshal_status_t
n00b_conduit_marshal_status(
    n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *xf);

extern n00b_string_t *
n00b_conduit_marshal_error(
    n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *xf);

extern n00b_marshal_status_t
n00b_conduit_unmarshal_status(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf);

extern n00b_string_t *
n00b_conduit_unmarshal_error(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf);

