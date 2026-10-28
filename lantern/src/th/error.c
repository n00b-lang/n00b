#include "private/th/internal.h"

static const core_string_t kind_names[TH_ERR_KIND_COUNT] = {
    [TH_ERR_UNIFY]               = CORE_STRING_STATIC("unify"),
    [TH_ERR_CONSTRAINT]          = CORE_STRING_STATIC("constraint"),
    [TH_ERR_OCCURS]              = CORE_STRING_STATIC("occurs"),
    [TH_ERR_NO_SUCH_FIELD]       = CORE_STRING_STATIC("no-such-field"),
    [TH_ERR_PARAM_MISMATCH]      = CORE_STRING_STATIC("param-mismatch"),
    [TH_ERR_ARITY_MISMATCH]      = CORE_STRING_STATIC("arity-mismatch"),
    [TH_ERR_MISSING_KEYWORD]     = CORE_STRING_STATIC("missing-keyword"),
    [TH_ERR_UNKNOWN_KEYWORD]     = CORE_STRING_STATIC("unknown-keyword"),
    [TH_ERR_NON_EXHAUSTIVE]      = CORE_STRING_STATIC("non-exhaustive"),
    [TH_ERR_UNREACHABLE_PATTERN] = CORE_STRING_STATIC("unreachable-pattern"),
    [TH_ERR_DUPLICATE_VARIANT]   = CORE_STRING_STATIC("duplicate-variant"),
    [TH_ERR_NO_MATCHING_RULE]    = CORE_STRING_STATIC("no-matching-rule"),
    [TH_ERR_BAD_NAME]            = CORE_STRING_STATIC("bad-name"),
    [TH_ERR_FREE_VAR]            = CORE_STRING_STATIC("free-var"),
};

core_string_t th_err_kind_name(th_err_kind kind)
{
    return kind < TH_ERR_KIND_COUNT && kind_names[kind].data
               ? kind_names[kind]
               : CORE_STRING_STATIC("unknown");
}

void th_err(th_ctx *ctx, th_err_kind kind, core_string_t constraint,
            th_type *expected, th_type *got, core_string_t message)
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

    core_list_push(ctx->errors, e);

    ctx->err = message;
}

void th_ctx_set_span(th_ctx *ctx, th_span_t span)
{
    if (ctx) {
        if (!span.file.data && span.start_line) span.file = ctx->source_file;
        ctx->span = span;
    }
}

void th_ctx_set_source_file(th_ctx *ctx, core_string_t file)
{
    if (ctx && (!ctx->source_file.data
                || !core_string_eq(ctx->source_file, file)))
        ctx->source_file = th_strdup(ctx, file);
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
    ctx->err        = (core_string_t){};
}

void th_ctx_set_probe_errors_observable(th_ctx *ctx, bool observable)
{
    if (ctx) {
        ctx->probe_errors_discarded = !observable;
    }
}
