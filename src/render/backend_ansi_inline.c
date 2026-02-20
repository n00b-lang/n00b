/**
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
#include <unistd.h>
#include <sys/ioctl.h>
#include "n00b.h"
#include "core/alloc.h"
#include "render/backend.h"
#include "strings/text_style.h"
#include "strings/theme.h"

// -------------------------------------------------------------------
// Context
// -------------------------------------------------------------------

typedef struct {
    char        *buf;
    size_t       buf_size;
    size_t       buf_used;
    int          fd;
    n00b_isize_t rows;
    n00b_isize_t cols;
} ansi_inline_ctx_t;

#define INLINE_INITIAL_BUF 16384

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

    char *new_buf = n00b_alloc_size(1, ctx->buf_size, .no_scan = true);
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

// -------------------------------------------------------------------
// SGR escape generation (same logic as the full-screen backend)
// -------------------------------------------------------------------

static void
inline_emit_sgr_reset(ansi_inline_ctx_t *ctx)
{
    inline_emit_str(ctx, "\033[0m");
}

static void
inline_emit_style(ansi_inline_ctx_t *ctx, const n00b_text_style_t *style)
{
    if (!style) {
        inline_emit_sgr_reset(ctx);
        return;
    }

    inline_emit_str(ctx, "\033[0");

    if (style->bold == N00B_TRI_YES) {
        inline_emit_str(ctx, ";1");
    }
    if (style->dim == N00B_TRI_YES) {
        inline_emit_str(ctx, ";2");
    }
    if (style->italic == N00B_TRI_YES) {
        inline_emit_str(ctx, ";3");
    }
    if (style->underline == N00B_TRI_YES) {
        inline_emit_str(ctx, ";4");
    }
    if (style->blink == N00B_TRI_YES) {
        inline_emit_str(ctx, ";5");
    }
    if (style->reverse == N00B_TRI_YES) {
        inline_emit_str(ctx, ";7");
    }
    if (style->strikethrough == N00B_TRI_YES) {
        inline_emit_str(ctx, ";9");
    }
    if (style->double_underline == N00B_TRI_YES) {
        inline_emit_str(ctx, ";21");
    }

    // Foreground color: direct RGB > palette > 256-color index.
    if (n00b_color_is_set(style->fg_rgb)) {
        int  rgb = n00b_color_rgb(style->fg_rgb);
        char buf[32];
        int  len = snprintf(buf, sizeof(buf), ";38;2;%d;%d;%d",
                            (rgb >> 16) & 0xFF,
                            (rgb >> 8) & 0xFF,
                            rgb & 0xFF);
        inline_emit(ctx, buf, len);
    }
    else if (style->fg_palette_ix >= 0 && style->fg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->fg_palette_ix);
        if (n00b_color_is_set(resolved)) {
            int  rgb = n00b_color_rgb(resolved);
            char buf[32];
            int  len = snprintf(buf, sizeof(buf), ";38;2;%d;%d;%d",
                                (rgb >> 16) & 0xFF,
                                (rgb >> 8) & 0xFF,
                                rgb & 0xFF);
            inline_emit(ctx, buf, len);
        }
    }

    // Background color: direct RGB > palette > 256-color index.
    if (n00b_color_is_set(style->bg_rgb)) {
        int  rgb = n00b_color_rgb(style->bg_rgb);
        char buf[32];
        int  len = snprintf(buf, sizeof(buf), ";48;2;%d;%d;%d",
                            (rgb >> 16) & 0xFF,
                            (rgb >> 8) & 0xFF,
                            rgb & 0xFF);
        inline_emit(ctx, buf, len);
    }
    else if (style->bg_palette_ix >= 0 && style->bg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->bg_palette_ix);
        if (n00b_color_is_set(resolved)) {
            int  rgb = n00b_color_rgb(resolved);
            char buf[32];
            int  len = snprintf(buf, sizeof(buf), ";48;2;%d;%d;%d",
                                (rgb >> 16) & 0xFF,
                                (rgb >> 8) & 0xFF,
                                rgb & 0xFF);
            inline_emit(ctx, buf, len);
        }
    }

    inline_emit_str(ctx, "m");
}

// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
ansi_inline_init(void)
{
    ansi_inline_ctx_t *ctx = n00b_alloc(ansi_inline_ctx_t, .no_scan = true);

    ctx->fd       = STDOUT_FILENO;
    ctx->buf_size = INLINE_INITIAL_BUF;
    ctx->buf      = n00b_alloc_size(1, INLINE_INITIAL_BUF, .no_scan = true);
    ctx->buf_used = 0;

    struct winsize ws;

    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
    }
    else {
        ctx->cols = 80;
    }

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

    // Only refresh column width from the terminal.  Row count is
    // left at whatever was set externally (0 by default).
    struct winsize ws;

    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
    }

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
                inline_emit_style(ctx, cell->style);
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
            inline_emit_sgr_reset(ctx);
            last_style = nullptr;
        }

        inline_emit(ctx, "\n", 1);
    }
}

static void
ansi_inline_flush(void *vctx)
{
    ansi_inline_ctx_t *ctx = vctx;

    if (ctx->buf_used > 0) {
        write(ctx->fd, ctx->buf, ctx->buf_used);
        ctx->buf_used = 0;
    }
}

// -------------------------------------------------------------------
// Inline-specific helpers
// -------------------------------------------------------------------

/**
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
    .name         = "ansi_inline",
    .version      = N00B_RENDERER_ABI_VERSION,
    .init         = ansi_inline_init,
    .destroy      = ansi_inline_destroy,
    .capabilities = ansi_inline_capabilities,
    .get_size     = ansi_inline_get_size,
    .render_frame = ansi_inline_render_frame,
    .flush        = ansi_inline_flush,
};
