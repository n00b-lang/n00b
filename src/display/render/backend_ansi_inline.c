/*
 * Inline ANSI terminal renderer backend.
 *
 * Outputs styled text line-by-line with SGR escape sequences but
 * **no cursor positioning**.  Each row is written sequentially,
 * terminated by a newline; trailing whitespace is trimmed.
 *
 * Use this backend for CLI tools that print styled output and exit.
 * The full-screen `n00b_renderer_ansi` should be used for TUI apps
 * that own the alternate screen.
 */

#include <stdio.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "display/render/backend.h"
#include "conduit/write.h"
#include "text/strings/text_style.h"
#include "display/render/composite.h"
#include "internal/display/ansi_sgr.h"

// -------------------------------------------------------------------
// Context
// -------------------------------------------------------------------

typedef struct {
    char                                   *buf;
    size_t                                  buf_size;
    size_t                                  buf_used;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output;
    n00b_isize_t                            rows;
    n00b_isize_t                            cols;

    // Persistent compositing grid.
    n00b_rcell_t                           *comp_grid;
    n00b_isize_t                            comp_grid_rows;
    n00b_isize_t                            comp_grid_cols;
    n00b_composite_style_pool_t             style_pool;
} ansi_inline_ctx_t;

#define INLINE_INITIAL_BUF 16384

static const char inline_base64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// -------------------------------------------------------------------
// Output buffer helpers
// -------------------------------------------------------------------

static void
inline_ensure(ansi_inline_ctx_t *ctx, size_t needed)
{
    if (ctx->buf_used + needed < ctx->buf_size) {
        return;
    }

    while (ctx->buf_used + needed >= ctx->buf_size) {
        ctx->buf_size *= 2;
    }

    char *new_buf = n00b_alloc_array_with_opts(char, ctx->buf_size, &(n00b_alloc_opts_t){.no_scan = true});
    memcpy(new_buf, ctx->buf, ctx->buf_used);
    n00b_free(ctx->buf);
    ctx->buf = new_buf;
}

static void
inline_emit(ansi_inline_ctx_t *ctx, const char *data, size_t len)
{
    inline_ensure(ctx, len);
    memcpy(ctx->buf + ctx->buf_used, data, len);
    ctx->buf_used += len;
}

static void
inline_emit_str(ansi_inline_ctx_t *ctx, const char *str)
{
    inline_emit(ctx, str, strlen(str));
}

static void
inline_emit_base64(ansi_inline_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;

    for (i = 0; i + 3 <= len; i += 3) {
        char chunk[4];
        uint32_t block = ((uint32_t)data[i] << 16)
                       | ((uint32_t)data[i + 1] << 8)
                       | (uint32_t)data[i + 2];

        chunk[0] = inline_base64[(block >> 18) & 0x3f];
        chunk[1] = inline_base64[(block >> 12) & 0x3f];
        chunk[2] = inline_base64[(block >> 6) & 0x3f];
        chunk[3] = inline_base64[block & 0x3f];
        inline_emit(ctx, chunk, sizeof(chunk));
    }

    if (i < len) {
        char chunk[4];
        uint32_t block = (uint32_t)data[i] << 16;

        if (i + 1 < len) {
            block |= (uint32_t)data[i + 1] << 8;
        }

        chunk[0] = inline_base64[(block >> 18) & 0x3f];
        chunk[1] = inline_base64[(block >> 12) & 0x3f];
        chunk[2] = (i + 1 < len) ? inline_base64[(block >> 6) & 0x3f] : '=';
        chunk[3] = '=';
        inline_emit(ctx, chunk, sizeof(chunk));
    }
}

static void
inline_sgr_emit_adapter(void *vctx, const char *data, size_t len)
{
    ansi_inline_ctx_t *ctx = vctx;
    inline_emit(ctx, data, len);
}

// -------------------------------------------------------------------
// SGR escape generation (same logic as the full-screen backend)
// -------------------------------------------------------------------


// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
ansi_inline_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    ansi_inline_ctx_t *ctx = n00b_alloc(ansi_inline_ctx_t);

    ctx->output   = output;
    ctx->buf_size = INLINE_INITIAL_BUF;
    ctx->buf      = n00b_alloc_array_with_opts(char, INLINE_INITIAL_BUF, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->buf_used = 0;
    ctx->cols     = 80;

    // Inline backend does not own the screen — leave rows = 0 so the
    // canvas knows it must be sized explicitly via canvas_resize().
    ctx->rows = 0;

    return ctx;
}

static void
ansi_inline_destroy(void *vctx)
{
    ansi_inline_ctx_t *ctx = vctx;

    if (ctx) {
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        n00b_composite_style_pool_destroy(&ctx->style_pool);
        n00b_free(ctx->buf);
        n00b_free(ctx);
    }
}

static n00b_render_cap_t
ansi_inline_capabilities(void *vctx)
{
    (void)vctx;

    // Same styling capabilities as the full-screen backend, but no
    // cursor movement, alt screen, or diff rendering.
    return N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_COLOR_256
         | N00B_RCAP_COLOR_24BIT
         | N00B_RCAP_BOLD
         | N00B_RCAP_ITALIC
         | N00B_RCAP_UNDERLINE
         | N00B_RCAP_STRIKETHROUGH
         | N00B_RCAP_DIM
         | N00B_RCAP_UNICODE
         | N00B_RCAP_WIDE_CHARS;
}

static n00b_render_size_t
ansi_inline_get_size(void *vctx)
{
    ansi_inline_ctx_t *ctx = vctx;

    // Return stored dimensions. Caller sets them via canvas_resize().
    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
    };
}

static void
ansi_inline_render_frame(void         *vctx,
                         n00b_rcell_t *cells,
                         n00b_isize_t  rows,
                         n00b_isize_t  cols,
                         n00b_rcell_t *prev_cells)
{
    (void)prev_cells;
    ansi_inline_ctx_t *ctx = vctx;

    ctx->buf_used = 0;

    const n00b_text_style_t *last_style = nullptr;

    for (n00b_isize_t r = 0; r < rows; r++) {
        // Find last non-space column to trim trailing whitespace.
        n00b_isize_t last_content = 0;

        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if ((cell->flags & N00B_CELL_OCCUPIED)
                && !(cell->flags & N00B_CELL_WIDE_CONT)
                && !(cell->grapheme_len == 1 && cell->grapheme[0] == ' ')) {
                last_content = c + 1;
            }
        }

        for (n00b_isize_t c = 0; c < last_content; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            // Emit style change.
            if (cell->style != last_style) {
                n00b_display_ansi_emit_style(cell->style, inline_sgr_emit_adapter, ctx);
                last_style = cell->style;
            }

            // Emit grapheme.
            if (cell->flags & N00B_CELL_OCCUPIED) {
                inline_emit(ctx, cell->grapheme, cell->grapheme_len);
            }
            else {
                inline_emit(ctx, " ", 1);
            }
        }

        // Reset style at end of each line so newlines don't carry
        // background color, then emit newline.
        if (last_style != nullptr) {
            n00b_display_ansi_emit_reset(inline_sgr_emit_adapter, ctx);
            last_style = nullptr;
        }

        inline_emit(ctx, "\n", 1);
    }
}

static void
ansi_inline_flush(void *vctx)
{
    ansi_inline_ctx_t *ctx = vctx;

    if (ctx->buf_used == 0) {
        return;
    }

    if (ctx->output) {
        n00b_buffer_t *buf = n00b_buffer_from_bytes(ctx->buf, (int64_t)ctx->buf_used);
        n00b_write(n00b_buffer_t *, ctx->output, buf);
    }

    ctx->buf_used = 0;
}

static bool
ansi_inline_clipboard_copy(void *vctx, const char *utf8, size_t len)
{
    ansi_inline_ctx_t *ctx = vctx;

    if (!ctx || !utf8) {
        return false;
    }

    inline_emit_str(ctx, "\033]52;c;");
    inline_emit_base64(ctx, (const uint8_t *)utf8, len);
    inline_emit_str(ctx, "\a");
    return true;
}

// -------------------------------------------------------------------
// Plane-based rendering
// -------------------------------------------------------------------

static void
ansi_inline_render_planes(void                         *vctx,
                           const n00b_composite_entry_t *entries,
                           n00b_isize_t                  count,
                           n00b_isize_t                  total_rows,
                           n00b_isize_t                  total_cols,
                           n00b_text_style_t            *default_style,
                           n00b_render_cap_t             caps)
{
    ansi_inline_ctx_t *ctx = vctx;

    if (total_rows != ctx->comp_grid_rows
        || total_cols != ctx->comp_grid_cols) {
        if (ctx->comp_grid) {
            n00b_free(ctx->comp_grid);
        }
        size_t total = (size_t)total_rows * total_cols;
        ctx->comp_grid = n00b_alloc_array(n00b_rcell_t, total);
        ctx->comp_grid_rows = total_rows;
        ctx->comp_grid_cols = total_cols;
    }

    n00b_composite_style_pool_clear(&ctx->style_pool);
    n00b_composite_commands_to_grid(entries, count, ctx->comp_grid,
                                     total_rows, total_cols,
                                     1, 1,
                                     default_style, caps,
                                     &ctx->style_pool);

    ansi_inline_render_frame(vctx, ctx->comp_grid, total_rows, total_cols,
                              nullptr);
}

// -------------------------------------------------------------------
// Inline-specific helpers
// -------------------------------------------------------------------

/*
 * Set the virtual terminal size for the inline backend.
 * Only valid when the backend is ansi_inline.
 */
void
n00b_ansi_inline_set_size(void *ctx, n00b_isize_t rows, n00b_isize_t cols)
{
    ansi_inline_ctx_t *ictx = ctx;
    ictx->rows = rows;
    ictx->cols = cols;
}

// -------------------------------------------------------------------
// Public vtable
// -------------------------------------------------------------------

const n00b_renderer_vtable_t n00b_renderer_ansi_inline = {
    .name          = "ansi_inline",
    .version       = N00B_RENDERER_ABI_VERSION,
    .init          = ansi_inline_init,
    .destroy       = ansi_inline_destroy,
    .capabilities  = ansi_inline_capabilities,
    .get_size      = ansi_inline_get_size,
    .render_frame  = ansi_inline_render_frame,
    .flush         = ansi_inline_flush,
    .render_planes = ansi_inline_render_planes,
    .clipboard_copy = ansi_inline_clipboard_copy,
};
