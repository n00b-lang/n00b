/*
 * Fallback (cell-based) font metrics provider.
 *
 * Used by ANSI and other cell-only backends.  Maps text measurement
 * to Unicode display width * cell pixel size.
 */

#include "n00b.h"
#include "text/unicode/properties.h"
#include "display/render/font_metrics.h"

// -------------------------------------------------------------------
// Fallback context: just stores cell pixel dimensions.
// -------------------------------------------------------------------

typedef struct {
    int32_t cell_px_w;
    int32_t cell_px_h;
} fallback_ctx_t;

static int32_t
fallback_text_width(void *vctx, n00b_string_t *text,
                     n00b_text_style_t *style)
{
    (void)style;
    fallback_ctx_t *ctx = vctx;

    if (!text) {
        return 0;
    }

    int32_t cols = (int32_t)n00b_unicode_display_width(text);
    return cols * ctx->cell_px_w;
}

static int32_t
fallback_line_height(void *vctx, n00b_text_style_t *style)
{
    (void)style;
    fallback_ctx_t *ctx = vctx;
    return ctx->cell_px_h;
}

static int32_t
fallback_ascent(void *vctx, n00b_text_style_t *style)
{
    (void)style;
    fallback_ctx_t *ctx = vctx;
    return ctx->cell_px_h;
}

// -------------------------------------------------------------------
// Public: create fallback provider
// -------------------------------------------------------------------

// Static context — one per program is fine since cell dimensions
// are uniform within a process.
static fallback_ctx_t _fallback_ctx;

n00b_font_metrics_provider_t
n00b_font_metrics_fallback(int32_t cell_px_w, int32_t cell_px_h)
{
    _fallback_ctx.cell_px_w = cell_px_w;
    _fallback_ctx.cell_px_h = cell_px_h;

    return (n00b_font_metrics_provider_t){
        .text_width  = fallback_text_width,
        .line_height = fallback_line_height,
        .ascent      = fallback_ascent,
        .ctx         = &_fallback_ctx,
    };
}
