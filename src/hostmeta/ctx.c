/** @file src/hostmeta/ctx.c — the collection context's write surface.
 *
 *  Collectors never touch `ctx->result` directly; they go through the
 *  helpers here. That keeps chalk's three-way "is this key wanted,
 *  is it already set, is the value non-empty" gate in one place, so a
 *  collector can enumerate every key it knows about unconditionally
 *  and let the gate decide.
 */

#define N00B_USE_INTERNAL_API

#include "internal/hostmeta/hostmeta_internal.h"

#include "adt/dict.h"
#include "text/strings/string_ops.h"

n00b_string_t *
n00b_hostmeta_err_str(n00b_hostmeta_err_t err)
{
    switch (err) {
    case N00B_HOSTMETA_OK:
        return r"ok";
    case N00B_HOSTMETA_ERR_ARG:
        return r"invalid argument";
    case N00B_HOSTMETA_ERR_NOT_INIT:
        return r"hostmeta registry not initialized";
    case N00B_HOSTMETA_ERR_NO_SUCH_NAME:
        return r"no collector registered under that name";
    case N00B_HOSTMETA_ERR_DUPLICATE:
        return r"a collector is already registered under that name";
    case N00B_HOSTMETA_ERR_FULL:
        return r"collector registry is full";
    }
    return r"unknown hostmeta error";
}

n00b_string_t *
n00b_hostmeta_scoped_key(n00b_hostmeta_ctx_t *ctx, const char *base)
{
    if (base == nullptr) {
        return nullptr;
    }
    n00b_string_t *k = n00b_string_from_cstr(base);
    if (ctx == nullptr || ctx->phase != N00B_HOSTMETA_PHASE_RUN_TIME) {
        return k;
    }
    return n00b_unicode_str_cat(r"_", k);
}

n00b_json_node_t *
n00b_hostmeta_get(n00b_hostmeta_ctx_t *ctx, n00b_string_t *key)
{
    if (ctx == nullptr || ctx->result == nullptr || key == nullptr) {
        return nullptr;
    }
    return n00b_json_object_get(ctx->result->keys, key);
}

/**
 * Whether the caller asked for @p key at all — the filter only, with
 * no regard for whether the key is already populated.
 *
 * Kept separate from `n00b_hostmeta_subscribed` because failures need
 * the filter without the already-set half: a key being filled in is a
 * reason not to overwrite it, but it is not a reason to throw away the
 * diagnostic explaining why some *other* part of the same collector
 * came up short.
 */
static bool
key_wanted(n00b_hostmeta_ctx_t *ctx, n00b_string_t *key)
{
    if (ctx == nullptr || key == nullptr || key->u8_bytes == 0) {
        return false;
    }
    if (ctx->subscribed == nullptr) {
        return true;
    }
    return ctx->subscribed(ctx->subscribed_user, key);
}

bool
n00b_hostmeta_subscribed(n00b_hostmeta_ctx_t *ctx, n00b_string_t *key)
{
    if (!key_wanted(ctx, key)) {
        return false;
    }
    // First writer wins: collectors are ordered most-specific-first, so
    // `aws_ecs` gets to name the region before the generic IMDS probe.
    return n00b_hostmeta_get(ctx, key) == nullptr;
}

void
n00b_hostmeta_put(n00b_hostmeta_ctx_t *ctx,
                  n00b_string_t       *key,
                  n00b_json_node_t    *value)
{
    if (value == nullptr || !n00b_hostmeta_subscribed(ctx, key)) {
        return;
    }
    n00b_json_object_put_n00b(ctx->result->keys, key, value);
}

void
n00b_hostmeta_put_string(n00b_hostmeta_ctx_t *ctx,
                         n00b_string_t       *key,
                         n00b_string_t       *value)
{
    if (value == nullptr || value->u8_bytes == 0) {
        return;
    }
    n00b_hostmeta_put(ctx, key, n00b_json_string_new_from_n00b(value));
}

void
n00b_hostmeta_put_cstr(n00b_hostmeta_ctx_t *ctx,
                       n00b_string_t       *key,
                       const char          *value)
{
    n00b_hostmeta_put_string(ctx, key, n00b_hostmeta_str(value));
}

void
n00b_hostmeta_put_string_list(n00b_hostmeta_ctx_t *ctx,
                              n00b_string_t       *key,
                              n00b_string_t       *value)
{
    if (value == nullptr || value->u8_bytes == 0
        || !n00b_hostmeta_subscribed(ctx, key)) {
        return;
    }
    n00b_json_node_t *arr = n00b_json_array_new();
    n00b_json_array_push(arr, n00b_json_string_new_from_n00b(value));
    n00b_json_object_put_n00b(ctx->result->keys, key, arr);
}

void
n00b_hostmeta_put_env(n00b_hostmeta_ctx_t *ctx,
                      n00b_string_t       *key,
                      const char          *env_name)
{
    n00b_hostmeta_put_string(ctx, key, n00b_hostmeta_env(env_name));
}

void
n00b_hostmeta_add_failure(n00b_hostmeta_ctx_t *ctx,
                          n00b_string_t       *key,
                          const char          *code,
                          n00b_string_t       *error,
                          const char          *description)
{
    if (ctx == nullptr || ctx->result == nullptr || key == nullptr) {
        return;
    }
    // Filter only. A failure whose key another collector already filled
    // in is still worth reporting — the key it names is the subject of
    // the diagnostic, not necessarily the thing that went missing.
    if (!key_wanted(ctx, key)) {
        return;
    }

    n00b_hostmeta_failure_t *f = n00b_alloc(n00b_hostmeta_failure_t);
    f->key                     = key;
    f->code                    = n00b_hostmeta_str(code);
    f->error                   = error ? error : n00b_string_empty();
    f->description             = n00b_hostmeta_str(description);

    n00b_list_push(ctx->result->failures, f);
}
