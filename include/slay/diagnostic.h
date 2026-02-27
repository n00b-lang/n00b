#pragma once

/**
 * @file diagnostic.h
 * @brief Unified diagnostic accumulator for the analysis pipeline.
 *
 * Collects diagnostics from all pipeline stages (parse, annotation walk,
 * type checking, CFG construction, static analysis) into a single list
 * with severity levels, source spans, and rendering.
 *
 * Reuses `n00b_tc_span_t` from `typecheck/types.h` for source locations.
 */

#include "typecheck/types.h"
#include "slay/parse_tree.h"
#include "core/list.h"
#include "core/string.h"

// ============================================================================
// Span alias
// ============================================================================

/** @brief Source location span (aliased from type checker). */
typedef n00b_tc_span_t n00b_diag_span_t;

// ============================================================================
// Severity
// ============================================================================

typedef enum {
    N00B_DIAG_ERROR,
    N00B_DIAG_WARNING,
    N00B_DIAG_NOTE,
} n00b_diag_severity_t;

// ============================================================================
// Pipeline stage
// ============================================================================

typedef enum {
    N00B_STAGE_PARSE,
    N00B_STAGE_ANNOT,
    N00B_STAGE_TYPECHECK,
    N00B_STAGE_CFG,
    N00B_STAGE_ANALYSIS,
    N00B_STAGE_CODEGEN,
} n00b_diag_stage_t;

// ============================================================================
// Diagnostic entry
// ============================================================================

typedef struct {
    n00b_diag_severity_t severity;
    n00b_diag_stage_t    stage;
    n00b_string_t        code;       /**< "W001", "E042", etc. */
    n00b_string_t        message;
    n00b_diag_span_t     span;
    n00b_diag_span_t     related;    /**< Secondary location (e.g., original decl). */
    bool                 has_related;
} n00b_diagnostic_t;

n00b_list_decl(n00b_diagnostic_t);

// ============================================================================
// Diagnostic context (accumulator)
// ============================================================================

typedef struct {
    n00b_list_t(n00b_diagnostic_t) diags;
    int32_t                         error_count;
    int32_t                         warning_count;
} n00b_diag_ctx_t;

// ============================================================================
// Lifecycle
// ============================================================================

n00b_diag_ctx_t *n00b_diag_ctx_new(void);
void n00b_diag_ctx_free(n00b_diag_ctx_t *ctx);

// ============================================================================
// Push diagnostics
// ============================================================================

void n00b_diag_push(n00b_diag_ctx_t     *ctx,
                    n00b_diag_severity_t  severity,
                    n00b_diag_stage_t     stage,
                    n00b_string_t         code,
                    n00b_string_t         message,
                    n00b_diag_span_t      span);

void n00b_diag_push_related(n00b_diag_ctx_t     *ctx,
                            n00b_diag_severity_t  severity,
                            n00b_diag_stage_t     stage,
                            n00b_string_t         code,
                            n00b_string_t         message,
                            n00b_diag_span_t      span,
                            n00b_diag_span_t      related);

// ============================================================================
// Query
// ============================================================================

static inline bool
n00b_diag_has_errors(n00b_diag_ctx_t *ctx)
{
    return ctx && ctx->error_count > 0;
}

static inline int32_t
n00b_diag_count(n00b_diag_ctx_t *ctx)
{
    return ctx ? (int32_t)n00b_list_len(ctx->diags) : 0;
}

// ============================================================================
// Rendering
// ============================================================================

/**
 * @brief Print all diagnostics to stderr with source context.
 *
 * @param ctx          Diagnostic context.
 * @param source_text  Full source text (for line extraction). May be NULL.
 * @param filename     Source filename for display. May be NULL.
 */
void n00b_diag_print_all(n00b_diag_ctx_t *ctx,
                         const char      *source_text,
                         const char      *filename);

// ============================================================================
// Import from type checker
// ============================================================================

/**
 * @brief Import type-check errors into the diagnostic context.
 *
 * Converts each `n00b_tc_error_t` from the type-check context's error
 * list into a `n00b_diagnostic_t` and pushes it.
 *
 * @param ctx     Diagnostic context to push into.
 * @param tc_ctx  Type-check context with accumulated errors.
 */
void n00b_diag_import_tc_errors(n00b_diag_ctx_t *ctx,
                                n00b_tc_ctx_t   *tc_ctx);

// ============================================================================
// Span helpers
// ============================================================================

/**
 * @brief Build a span from a token.
 */
static inline n00b_diag_span_t
n00b_diag_span_from_token(n00b_token_info_t *tok)
{
    n00b_diag_span_t span = {0};

    if (tok) {
        span.file      = tok->file;
        span.start_line = tok->line;
        span.start_col  = tok->column;
        span.end_line   = tok->line;
        span.end_col    = tok->endcol;
    }

    return span;
}

/**
 * @brief Build a span from a parse tree node (leftmost token).
 */
static inline n00b_diag_span_t
n00b_diag_span_from_node(n00b_parse_tree_t *node)
{
    if (!node) {
        return (n00b_diag_span_t){0};
    }

    n00b_parse_tree_t *first = n00b_pt_first_token(node);

    if (!first) {
        return (n00b_diag_span_t){0};
    }

    n00b_token_info_t *tok = n00b_parse_node_token(first);

    return n00b_diag_span_from_token(tok);
}
