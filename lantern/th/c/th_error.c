#include <string.h>

#include "th_internal.h"

static const char *const kind_names[TH_ERR_KIND_COUNT] = {
    [TH_ERR_UNIFY]               = "unify",
    [TH_ERR_CONSTRAINT]          = "constraint",
    [TH_ERR_OCCURS]              = "occurs",
    [TH_ERR_NO_SUCH_FIELD]       = "no-such-field",
    [TH_ERR_PARAM_MISMATCH]      = "param-mismatch",
    [TH_ERR_ARITY_MISMATCH]      = "arity-mismatch",
    [TH_ERR_MISSING_KEYWORD]     = "missing-keyword",
    [TH_ERR_UNKNOWN_KEYWORD]     = "unknown-keyword",
    [TH_ERR_NON_EXHAUSTIVE]      = "non-exhaustive",
    [TH_ERR_UNREACHABLE_PATTERN] = "unreachable-pattern",
    [TH_ERR_DUPLICATE_VARIANT]   = "duplicate-variant",
    [TH_ERR_NO_MATCHING_RULE]    = "no-matching-rule",
    [TH_ERR_BAD_NAME]            = "bad-name",
    [TH_ERR_FREE_VAR]            = "free-var",
};

const char *th_err_kind_name(th_err_kind kind)
{
    return kind < TH_ERR_KIND_COUNT && kind_names[kind] ? kind_names[kind]
                                                        : "unknown";
}

void th_err(th_ctx *ctx, th_err_kind kind, const char *constraint,
            th_type *expected, th_type *got, const char *message)
{
    if (!ctx) {
        return;
    }

    th_error_t e = {
        .kind        = kind,
        .message     = message,
        .constraint  = constraint,
        .expected    = expected,
        .got         = got,
        .span        = ctx->span,
        .related     = ctx->related,
        .has_related = ctx->has_related,
    };

    ncc_list_push(ctx->errors, e);

    ctx->err = message;
}

void th_ctx_set_span(th_ctx *ctx, th_span_t span)
{
    if (ctx) {
        ctx->span = span;
    }
}

void th_ctx_set_related(th_ctx *ctx, th_span_t span)
{
    if (ctx) {
        ctx->related     = span;
        ctx->has_related = true;
    }
}

void th_ctx_clear_related(th_ctx *ctx)
{
    if (ctx) {
        ctx->related     = (th_span_t){0};
        ctx->has_related = false;
    }
}

th_span_t th_ctx_span(const th_ctx *ctx)
{
    return ctx ? ctx->span : (th_span_t){0};
}

size_t th_error_count(const th_ctx *ctx)
{
    return ctx ? ctx->errors.len : 0;
}

bool th_error_at(const th_ctx *ctx, size_t i, th_error_t *out)
{
    if (!ctx || i >= ctx->errors.len) {
        return false;
    }

    if (out) {
        *out = ctx->errors.data[i];
    }

    return true;
}

void th_errors_clear(th_ctx *ctx)
{
    if (!ctx) {
        return;
    }

    ctx->errors.len = 0;
    ctx->err        = nullptr;
}
