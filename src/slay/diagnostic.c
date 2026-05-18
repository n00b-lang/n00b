// diagnostic.c — Unified diagnostic accumulator.
//
// Collects diagnostics from all pipeline stages into a single list,
// and renders them with ANSI colors and source context.

#include "slay/diagnostic.h"
#include "typecheck/context.h"
#include "core/alloc.h"
#include "text/strings/string_ops.h"

#include <stdio.h>
#include <string.h>

// ============================================================================
// Lifecycle
// ============================================================================

n00b_diag_ctx_t *
n00b_diag_ctx_new(void)
{
    n00b_diag_ctx_t *ctx = n00b_alloc(n00b_diag_ctx_t);
    ctx->diags         = n00b_list_new_private(n00b_diagnostic_t);
    ctx->error_count   = 0;
    ctx->warning_count = 0;

    return ctx;
}

void
n00b_diag_ctx_free(n00b_diag_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    n00b_list_free(ctx->diags);
    n00b_free(ctx);
}

// ============================================================================
// Push
// ============================================================================

void
n00b_diag_push(n00b_diag_ctx_t     *ctx,
               n00b_diag_severity_t severity,
               n00b_diag_stage_t    stage,
               n00b_string_t       *code,
               n00b_string_t       *message,
               n00b_diag_span_t     span)
{
    if (!ctx) {
        return;
    }

    n00b_diagnostic_t d = {
        .severity    = severity,
        .stage       = stage,
        .code        = code,
        .message     = message,
        .span        = span,
        .has_related = false,
    };

    n00b_list_push(ctx->diags, d);

    if (severity == N00B_DIAG_ERROR) {
        ctx->error_count++;
    }
    else if (severity == N00B_DIAG_WARNING) {
        ctx->warning_count++;
    }
}

void
n00b_diag_push_related(n00b_diag_ctx_t     *ctx,
                       n00b_diag_severity_t severity,
                       n00b_diag_stage_t    stage,
                       n00b_string_t       *code,
                       n00b_string_t       *message,
                       n00b_diag_span_t     span,
                       n00b_diag_span_t     related)
{
    if (!ctx) {
        return;
    }

    n00b_diagnostic_t d = {
        .severity    = severity,
        .stage       = stage,
        .code        = code,
        .message     = message,
        .span        = span,
        .related     = related,
        .has_related = true,
    };

    n00b_list_push(ctx->diags, d);

    if (severity == N00B_DIAG_ERROR) {
        ctx->error_count++;
    }
    else if (severity == N00B_DIAG_WARNING) {
        ctx->warning_count++;
    }
}

// ============================================================================
// Source line extraction
// ============================================================================

// Extract line N (1-based) from source text. Returns pointer into src
// and sets *out_len. Returns NULL if line not found.
static const char *
extract_line(const char *src, uint32_t line_num, size_t *out_len)
{
    if (!src || line_num == 0) {
        *out_len = 0;
        return NULL;
    }

    const char *p     = src;
    uint32_t    cur   = 1;

    while (*p && cur < line_num) {
        if (*p == '\n') {
            cur++;
        }

        p++;
    }

    if (cur != line_num) {
        *out_len = 0;
        return NULL;
    }

    const char *start = p;
    const char *end   = p;

    while (*end && *end != '\n') {
        end++;
    }

    *out_len = (size_t)(end - start);

    return start;
}

// ============================================================================
// Rendering
// ============================================================================

static const char *
severity_label(n00b_diag_severity_t sev)
{
    switch (sev) {
    case N00B_DIAG_ERROR:   return "error";
    case N00B_DIAG_WARNING: return "warning";
    case N00B_DIAG_NOTE:    return "note";
    }

    return "unknown";
}

static const char *
severity_color(n00b_diag_severity_t sev)
{
    switch (sev) {
    case N00B_DIAG_ERROR:   return "\033[1;31m"; // bold red
    case N00B_DIAG_WARNING: return "\033[1;33m"; // bold yellow
    case N00B_DIAG_NOTE:    return "\033[1;36m"; // bold cyan
    }

    return "\033[0m";
}

void
n00b_diag_print_all(n00b_diag_ctx_t *ctx,
                    const char      *source_text,
                    const char      *filename)
{
    if (!ctx) {
        return;
    }

    size_t count = n00b_list_len(ctx->diags);

    for (size_t i = 0; i < count; i++) {
        n00b_diagnostic_t d = n00b_list_get(ctx->diags, i);

        const char *color = severity_color(d.severity);
        const char *label = severity_label(d.severity);
        const char *reset = "\033[0m";

        // Print: severity[code]: message
        fprintf(stderr, "%s%s", color, label);

        if (d.code && d.code->u8_bytes > 0) {
            fprintf(stderr, "[%.*s]", (int)d.code->u8_bytes, d.code->data);
        }

        fprintf(stderr, ":%s ", reset);

        if (d.message && d.message->u8_bytes > 0) {
            fprintf(stderr, "%.*s", (int)d.message->u8_bytes, d.message->data);
        }

        fprintf(stderr, "\n");

        // Print: --> filename:line:col
        const char *fn = filename ? filename : "<input>";

        if (d.span.start_line > 0) {
            fprintf(stderr, "  --> %s:%u:%u\n",
                    fn, d.span.start_line, d.span.start_col);
        }

        // Print source line with caret.
        if (source_text && d.span.start_line > 0) {
            size_t      line_len;
            const char *line = extract_line(source_text, d.span.start_line,
                                            &line_len);

            if (line) {
                fprintf(stderr, "   | %.*s\n", (int)line_len, line);

                // Caret line.
                fprintf(stderr, "   | ");
                uint32_t col = d.span.start_col > 0 ? d.span.start_col - 1 : 0;

                for (uint32_t c = 0; c < col; c++) {
                    fputc(' ', stderr);
                }

                fprintf(stderr, "%s^%s\n", color, reset);
            }
        }

        // Print related location.
        if (d.has_related && d.related.start_line > 0) {
            fprintf(stderr, "  --> also: %s:%u:%u\n",
                    fn, d.related.start_line, d.related.start_col);
        }
    }

    // Summary line.
    if (count > 0) {
        fprintf(stderr, "\n");
    }

    if (ctx->error_count > 0 || ctx->warning_count > 0) {
        fprintf(stderr, "%d error(s), %d warning(s)\n",
                ctx->error_count, ctx->warning_count);
    }
}

// ============================================================================
// Import from type checker
// ============================================================================

// Map tc error kind to a diagnostic code string.
static n00b_string_t *
tc_err_code(n00b_tc_err_kind_t kind)
{
    switch (kind) {
    case N00B_TC_ERR_UNIFY_FAIL:          return r"TC001";
    case N00B_TC_ERR_CONSTRAINT_FAIL:     return r"TC002";
    case N00B_TC_ERR_OCCURS_CHECK:        return r"TC003";
    case N00B_TC_ERR_NON_EXHAUSTIVE:      return r"TC004";
    case N00B_TC_ERR_UNREACHABLE_PATTERN: return r"TC005";
    case N00B_TC_ERR_DUPLICATE_VARIANT:   return r"TC006";
    case N00B_TC_ERR_NO_SUCH_FIELD:       return r"TC007";
    case N00B_TC_ERR_PARAM_MISMATCH:      return r"TC008";
    case N00B_TC_ERR_ARITY_MISMATCH:      return r"TC009";
    case N00B_TC_ERR_MISSING_KEYWORD:     return r"TC010";
    case N00B_TC_ERR_UNKNOWN_KEYWORD:     return r"TC011";
    case N00B_TC_ERR_NO_MATCHING_RULE:    return r"TC012";
    }

    return r"TC000";
}

void
n00b_diag_merge(n00b_diag_ctx_t *dst, n00b_diag_ctx_t *src)
{
    if (!dst || !src) {
        return;
    }

    size_t n = n00b_list_len(src->diags);
    int32_t merged_errors = 0;
    int32_t merged_warnings = 0;

    for (size_t i = 0; i < n; i++) {
        n00b_diagnostic_t d = n00b_list_get(src->diags, i);
        n00b_list_push(dst->diags, d);

        if (d.severity == N00B_DIAG_ERROR) {
            dst->error_count++;
            merged_errors++;
        }
        else if (d.severity == N00B_DIAG_WARNING) {
            dst->warning_count++;
            merged_warnings++;
        }
    }

    if (src->error_count > merged_errors) {
        dst->error_count += src->error_count - merged_errors;
    }

    if (src->warning_count > merged_warnings) {
        dst->warning_count += src->warning_count - merged_warnings;
    }
}

void
n00b_diag_import_parse_error(n00b_diag_ctx_t *ctx,
                             uint32_t         error_line,
                             uint32_t         error_col,
                             const char      *got_text,
                             const char      *expected_msg,
                             const char      *filename)
{
    if (!ctx) {
        return;
    }

    // Build message: "parse error at line N:M: unexpected 'X'"
    // followed by expected tokens if available.
    char buf[512];
    int  off = 0;

    if (got_text) {
        off = snprintf(buf, sizeof(buf), "unexpected token '%s'", got_text);
    }
    else {
        off = snprintf(buf, sizeof(buf), "unexpected end of input");
    }

    if (expected_msg && off < (int)sizeof(buf) - 2) {
        snprintf(buf + off, sizeof(buf) - (size_t)off, "; %s", expected_msg);
    }

    n00b_diag_span_t span = {0};
    span.start_line = error_line;
    span.start_col  = error_col;
    span.end_line   = error_line;
    span.end_col    = error_col;

    if (filename && *filename) {
        span.file = n00b_option_set(n00b_string_t *,
                                    n00b_string_from_cstr(filename));
    }

    n00b_diag_push(ctx,
                   N00B_DIAG_ERROR,
                   N00B_STAGE_PARSE,
                   r"P001",
                   n00b_string_from_cstr(buf),
                   span);
}

void
n00b_diag_import_tc_errors(n00b_diag_ctx_t *ctx, n00b_tc_ctx_t *tc_ctx)
{
    if (!ctx || !tc_ctx || !tc_ctx->errors) {
        return;
    }

    size_t nerrs = n00b_list_len(*tc_ctx->errors);

    for (size_t i = 0; i < nerrs; i++) {
        n00b_tc_error_t err = n00b_list_get(*tc_ctx->errors, i);

        bool has_related = err.related_span.start_line > 0;

        if (has_related) {
            n00b_diag_push_related(ctx,
                                  N00B_DIAG_ERROR,
                                  N00B_STAGE_TYPECHECK,
                                  tc_err_code(err.kind),
                                  err.message,
                                  err.span,
                                  err.related_span);
        }
        else {
            n00b_diag_push(ctx,
                          N00B_DIAG_ERROR,
                          N00B_STAGE_TYPECHECK,
                          tc_err_code(err.kind),
                          err.message,
                          err.span);
        }
    }
}
