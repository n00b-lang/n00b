/*
 * xform_marshal.c - Object graph marshal/unmarshal conduit transforms.
 */

#include "conduit/xform_marshal.h"
#include "core/alloc.h"

// ============================================================================
// Marshal transform: object -> buffer
// ============================================================================

static n00b_option_t(n00b_buffer_t *)
marshal_transform(
    n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *xf,
    n00b_marshal_object_t input)
{
    n00b_conduit_marshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_marshal_object_t, n00b_buffer_t *, xf);

    if (!input) {
        st->status = N00B_MARSHAL_ERR_NULL_ARG;
        st->error  = "marshal transform received a null object";
        return n00b_option_none(n00b_buffer_t *);
    }

    n00b_marshal_ctx_t *ctx = n00b_marshal_ctx_new(
        .flags        = st->flags,
        .base_address = st->base_address);
    n00b_buffer_t *buf = n00b_marshal_incremental(ctx, input);

    st->status = n00b_marshal_ctx_status(ctx);
    st->error  = n00b_marshal_ctx_error(ctx);
    n00b_marshal_ctx_destroy(ctx);

    if (!buf || st->status != N00B_MARSHAL_OK) {
        return n00b_option_none(n00b_buffer_t *);
    }

    return n00b_option_set(n00b_buffer_t *, buf);
}

static n00b_string_t _kind_marshal = {
    .data = "marshal", .u8_bytes = 7, .codepoints = 7, .styling = nullptr
};

static const n00b_conduit_xform_ops_t(n00b_marshal_object_t, n00b_buffer_t *)
    marshal_ops = {
    .transform = marshal_transform,
    .kind      = &_kind_marshal,
};

n00b_result_t(n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *)
n00b_conduit_marshal_new(
    n00b_conduit_t                              *c,
    n00b_conduit_topic_t(n00b_marshal_object_t) *upstream)
    _kargs {
        uint32_t flags        = N00B_MARSHAL_F_NONE;
        uint32_t base_address = 0;
    }
{
    auto r = n00b_conduit_xform_new(
        n00b_marshal_object_t,
        n00b_buffer_t *,
        c,
        upstream,
        &marshal_ops,
        sizeof(n00b_conduit_marshal_state_t));

    if (n00b_result_is_err(r)) {
        return r;
    }

    auto xf = n00b_result_get(r);
    n00b_conduit_marshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_marshal_object_t, n00b_buffer_t *, xf);

    st->flags        = flags;
    st->base_address = base_address;
    st->status       = N00B_MARSHAL_OK;
    st->error        = nullptr;

    return r;
}

n00b_marshal_status_t
n00b_conduit_marshal_status(
    n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *xf)
{
    if (!xf || !xf->cookie) return N00B_MARSHAL_ERR_NULL_ARG;

    n00b_conduit_marshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_marshal_object_t, n00b_buffer_t *, xf);
    return st->status;
}

const char *
n00b_conduit_marshal_error(
    n00b_conduit_xform_t(n00b_marshal_object_t, n00b_buffer_t *) *xf)
{
    if (!xf || !xf->cookie) return "null marshal transform";

    n00b_conduit_marshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_marshal_object_t, n00b_buffer_t *, xf);
    return st->error;
}

// ============================================================================
// Unmarshal transform: buffer -> object(s)
// ============================================================================

static n00b_option_t(n00b_marshal_object_t)
unmarshal_transform(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf,
    n00b_buffer_t *input)
{
    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);

    if (!st->ctx) {
        st->ctx = n00b_unmarshal_ctx_new(.target_arena = st->target_arena);
    }

    if (!input) {
        st->status = N00B_MARSHAL_ERR_NULL_ARG;
        st->error  = "unmarshal transform received a null buffer";
        return n00b_option_none(n00b_marshal_object_t);
    }

    n00b_list_t(void *) roots = n00b_unmarshal_incremental(st->ctx, input);
    st->status               = n00b_unmarshal_ctx_status(st->ctx);
    st->error                = n00b_unmarshal_ctx_error(st->ctx);

    uint64_t n = n00b_list_len(roots);
    for (uint64_t i = 0; i < n; i++) {
        n00b_marshal_object_t root = n00b_list_get(roots, i);
        n00b_conduit_xform_emit(
            n00b_buffer_t *, n00b_marshal_object_t, xf, root);
    }

    return n00b_option_none(n00b_marshal_object_t);
}

static void
unmarshal_flush(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf)
{
    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);

    if (st->status == N00B_MARSHAL_ERR_INCOMPLETE_STREAM && !st->error) {
        st->error = "upstream closed with an incomplete marshal stream";
    }
}

static void
unmarshal_teardown(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf)
{
    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);

    if (st && st->ctx) {
        n00b_unmarshal_ctx_destroy(st->ctx);
        st->ctx = nullptr;
    }
}

static n00b_string_t _kind_unmarshal = {
    .data = "unmarshal", .u8_bytes = 9, .codepoints = 9, .styling = nullptr
};

static const n00b_conduit_xform_ops_t(n00b_buffer_t *, n00b_marshal_object_t)
    unmarshal_ops = {
    .transform = unmarshal_transform,
    .flush     = unmarshal_flush,
    .teardown  = unmarshal_teardown,
    .kind      = &_kind_unmarshal,
};

n00b_result_t(n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *)
n00b_conduit_unmarshal_new(
    n00b_conduit_t                        *c,
    n00b_conduit_topic_t(n00b_buffer_t *) *upstream)
    _kargs {
        n00b_arena_t *target_arena = nullptr;
    }
{
    auto r = n00b_conduit_xform_new(
        n00b_buffer_t *,
        n00b_marshal_object_t,
        c,
        upstream,
        &unmarshal_ops,
        sizeof(n00b_conduit_unmarshal_state_t));

    if (n00b_result_is_err(r)) {
        return r;
    }

    auto xf = n00b_result_get(r);
    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);

    st->ctx          = n00b_unmarshal_ctx_new(.target_arena = target_arena);
    st->target_arena = target_arena;
    st->status       = N00B_MARSHAL_OK;
    st->error        = nullptr;

    return r;
}

n00b_marshal_status_t
n00b_conduit_unmarshal_status(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf)
{
    if (!xf || !xf->cookie) return N00B_MARSHAL_ERR_NULL_ARG;

    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);
    return st->status;
}

const char *
n00b_conduit_unmarshal_error(
    n00b_conduit_xform_t(n00b_buffer_t *, n00b_marshal_object_t) *xf)
{
    if (!xf || !xf->cookie) return "null unmarshal transform";

    n00b_conduit_unmarshal_state_t *st = n00b_conduit_xform_cookie(
        n00b_buffer_t *, n00b_marshal_object_t, xf);
    return st->error;
}
