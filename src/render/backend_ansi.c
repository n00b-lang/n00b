/*
 * ANSI terminal renderer backend.
 *
 * Full-featured terminal backend with:
 * - Differential rendering (only emit changed cells)
 * - SGR escape sequences for colors and decorations
 * - Cursor position tracking to minimize cursor movement
 * - Output buffering for efficiency
 */

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include "internal/n00b_windows_compat.h"
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif
#include "n00b.h"
#include "core/alloc.h"
#include "render/backend.h"
#include "conduit/write.h"
#include "strings/text_style.h"
#include "strings/theme.h"

// -------------------------------------------------------------------
// ANSI context
// -------------------------------------------------------------------

typedef struct {
    char                                   *buf;
    size_t                                  buf_size;
    size_t                                  buf_used;
    int                                     fd;
    n00b_conduit_topic_t(n00b_buffer_t *)  *output;
    n00b_isize_t                            rows;
    n00b_isize_t                            cols;
    n00b_isize_t                            cursor_row;
    n00b_isize_t                            cursor_col;
    bool                                    cursor_visible;
} ansi_ctx_t;

#define ANSI_INITIAL_BUF 16384

static void
n00b_os_write(int fd, const char *buf, size_t len)
{
#if defined(_WIN32)
    (void)fd;
    (void)fwrite(buf, 1, len, stdout);
    (void)fflush(stdout);
#else
    (void)write(fd, buf, len);
#endif
}

#if defined(_WIN32)
static bool
n00b_query_terminal_size(int fd, n00b_isize_t *rows, n00b_isize_t *cols)
{
    (void)fd;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h) {
        return false;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(h, &info)) {
        return false;
    }

    n00b_isize_t width = (n00b_isize_t)(info.srWindow.Right - info.srWindow.Left + 1);
    n00b_isize_t height = (n00b_isize_t)(info.srWindow.Bottom - info.srWindow.Top + 1);
    if (width <= 0 || height <= 0) {
        return false;
    }

    *cols = width;
    *rows = height;
    return true;
}
#else
static bool
n00b_query_terminal_size(int fd, n00b_isize_t *rows, n00b_isize_t *cols)
{
    struct winsize ws;
    if (ioctl(fd, TIOCGWINSZ, &ws) != 0 || ws.ws_col <= 0) {
        return false;
    }

    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return true;
}
#endif

// -------------------------------------------------------------------
// Output buffer helpers
// -------------------------------------------------------------------

static void
ansi_ensure(ansi_ctx_t *ctx, size_t needed)
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
ansi_emit(ansi_ctx_t *ctx, const char *data, size_t len)
{
    ansi_ensure(ctx, len);
    memcpy(ctx->buf + ctx->buf_used, data, len);
    ctx->buf_used += len;
}

static void
ansi_emit_str(ansi_ctx_t *ctx, const char *str)
{
    ansi_emit(ctx, str, strlen(str));
}

static void
ansi_emit_int(ansi_ctx_t *ctx, int val)
{
    char tmp[16];
    int len = snprintf(tmp, sizeof(tmp), "%d", val);
    ansi_emit(ctx, tmp, len);
}

// -------------------------------------------------------------------
// SGR escape generation
// -------------------------------------------------------------------

static void
ansi_emit_sgr_reset(ansi_ctx_t *ctx)
{
    ansi_emit_str(ctx, "\033[0m");
}

static void
ansi_emit_style(ansi_ctx_t *ctx, const n00b_text_style_t *style)
{
    if (!style) {
        ansi_emit_sgr_reset(ctx);
        return;
    }

    ansi_emit_str(ctx, "\033[0");

    if (style->bold == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";1");
    }
    if (style->dim == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";2");
    }
    if (style->italic == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";3");
    }
    if (style->underline == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";4");
    }
    if (style->blink == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";5");
    }
    if (style->reverse == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";7");
    }
    if (style->strikethrough == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";9");
    }
    if (style->double_underline == N00B_TRI_YES) {
        ansi_emit_str(ctx, ";21");
    }

    // Foreground color: direct RGB > palette > 256-color index.
    if (n00b_color_is_set(style->fg_rgb)) {
        int rgb = n00b_color_rgb(style->fg_rgb);
        char buf[32];
        int len = snprintf(buf, sizeof(buf), ";38;2;%d;%d;%d",
                           (rgb >> 16) & 0xFF,
                           (rgb >> 8) & 0xFF,
                           rgb & 0xFF);
        ansi_emit(ctx, buf, len);
    }
    else if (style->fg_palette_ix >= 0 && style->fg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->fg_palette_ix);
        if (n00b_color_is_set(resolved)) {
            int rgb = n00b_color_rgb(resolved);
            char buf[32];
            int len = snprintf(buf, sizeof(buf), ";38;2;%d;%d;%d",
                               (rgb >> 16) & 0xFF,
                               (rgb >> 8) & 0xFF,
                               rgb & 0xFF);
            ansi_emit(ctx, buf, len);
        }
    }

    // Background color: direct RGB > palette > 256-color index.
    if (n00b_color_is_set(style->bg_rgb)) {
        int rgb = n00b_color_rgb(style->bg_rgb);
        char buf[32];
        int len = snprintf(buf, sizeof(buf), ";48;2;%d;%d;%d",
                           (rgb >> 16) & 0xFF,
                           (rgb >> 8) & 0xFF,
                           rgb & 0xFF);
        ansi_emit(ctx, buf, len);
    }
    else if (style->bg_palette_ix >= 0 && style->bg_palette_ix < N00B_PAL_SIZE) {
        n00b_color_t resolved = n00b_theme_resolve_color(style->bg_palette_ix);
        if (n00b_color_is_set(resolved)) {
            int rgb = n00b_color_rgb(resolved);
            char buf[32];
            int len = snprintf(buf, sizeof(buf), ";48;2;%d;%d;%d",
                               (rgb >> 16) & 0xFF,
                               (rgb >> 8) & 0xFF,
                               rgb & 0xFF);
            ansi_emit(ctx, buf, len);
        }
    }

    ansi_emit_str(ctx, "m");
}

// -------------------------------------------------------------------
// Cursor movement
// -------------------------------------------------------------------

static void
ansi_move_cursor(ansi_ctx_t *ctx, n00b_isize_t row, n00b_isize_t col)
{
    if (ctx->cursor_row == row && ctx->cursor_col == col) {
        return;
    }

    // CSI row;col H (1-indexed).
    ansi_emit_str(ctx, "\033[");
    ansi_emit_int(ctx, (int)(row + 1));
    ansi_emit_str(ctx, ";");
    ansi_emit_int(ctx, (int)(col + 1));
    ansi_emit_str(ctx, "H");

    ctx->cursor_row = row;
    ctx->cursor_col = col;
}

// -------------------------------------------------------------------
// Vtable implementation
// -------------------------------------------------------------------

static void *
ansi_init(n00b_conduit_topic_t(n00b_buffer_t *) *output)
{
    ansi_ctx_t *ctx = n00b_alloc_with_opts(ansi_ctx_t, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->fd         = STDOUT_FILENO;
    ctx->output     = output;
    ctx->buf_size   = ANSI_INITIAL_BUF;
    ctx->buf        = n00b_alloc_array_with_opts(char, ANSI_INITIAL_BUF, &(n00b_alloc_opts_t){.no_scan = true});
    ctx->buf_used   = 0;
    ctx->cursor_visible = true;

    if (!n00b_query_terminal_size(ctx->fd, &ctx->rows, &ctx->cols)) {
        ctx->cols = 80;
        ctx->rows = 25;
    }

    return ctx;
}

static void
ansi_destroy(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    if (ctx) {
        n00b_free(ctx->buf);
        n00b_free(ctx);
    }
}

static n00b_render_cap_t
ansi_capabilities(void *vctx)
{
    (void)vctx;
    return N00B_RCAP_COLOR_BASIC
         | N00B_RCAP_COLOR_256
         | N00B_RCAP_COLOR_24BIT
         | N00B_RCAP_BOLD
         | N00B_RCAP_ITALIC
         | N00B_RCAP_UNDERLINE
         | N00B_RCAP_STRIKETHROUGH
         | N00B_RCAP_DIM
         | N00B_RCAP_CURSOR_MOVE
         | N00B_RCAP_ALT_SCREEN
         | N00B_RCAP_UNICODE
         | N00B_RCAP_WIDE_CHARS
         | N00B_RCAP_DIFF_RENDER;
}

static n00b_render_size_t
ansi_get_size(void *vctx)
{
    ansi_ctx_t *ctx = vctx;

    (void)n00b_query_terminal_size(ctx->fd, &ctx->rows, &ctx->cols);

    return (n00b_render_size_t){
        .rows = ctx->rows,
        .cols = ctx->cols,
    };
}

static void
ansi_render_frame(void *vctx, n00b_rcell_t *cells,
                  n00b_isize_t rows, n00b_isize_t cols,
                  n00b_rcell_t *prev_cells)
{
    ansi_ctx_t *ctx = vctx;
    ctx->buf_used = 0;

    const n00b_text_style_t *last_style = nullptr;

    for (n00b_isize_t r = 0; r < rows; r++) {
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_rcell_t *cell = &cells[r * cols + c];

            if (cell->flags & N00B_CELL_WIDE_CONT) {
                continue;
            }

            // Diff render: skip unchanged cells.
            if (prev_cells) {
                n00b_rcell_t *prev = &prev_cells[r * cols + c];
                if (n00b_rcell_equal(cell, prev)) {
                    continue;
                }
            }

            // Move cursor to this cell.
            ansi_move_cursor(ctx, r, c);

            // Emit style if changed.
            if (cell->style != last_style) {
                ansi_emit_style(ctx, cell->style);
                last_style = cell->style;
            }

            // Emit grapheme.
            if (cell->flags & N00B_CELL_OCCUPIED) {
                ansi_emit(ctx, cell->grapheme, cell->grapheme_len);
            }
            else {
                ansi_emit(ctx, " ", 1);
            }

            ctx->cursor_col += cell->display_width ? cell->display_width : 1;
        }
    }

    // Reset style at end.
    ansi_emit_sgr_reset(ctx);
}

static void
ansi_flush(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    if (ctx->buf_used > 0) {
        n00b_os_write(ctx->fd, ctx->buf, ctx->buf_used);
        ctx->buf_used = 0;
    }

    if (ctx->output) {
        n00b_buffer_t *buf = n00b_buffer_from_bytes(ctx->buf, (int64_t)ctx->buf_used);
        n00b_write(n00b_buffer_t *, ctx->output, buf);
    }

    ctx->buf_used = 0;
}

static void
ansi_cursor_set_visible(void *vctx, bool visible)
{
    ansi_ctx_t *ctx = vctx;
    if (visible) {
        ansi_emit_str(ctx, "\033[?25h");
    }
    else {
        ansi_emit_str(ctx, "\033[?25l");
    }
    ctx->cursor_visible = visible;
    ansi_flush(vctx);
}

static void
ansi_cursor_move(void *vctx, n00b_isize_t row, n00b_isize_t col)
{
    ansi_ctx_t *ctx = vctx;
    ansi_move_cursor(ctx, row, col);
    ansi_flush(vctx);
}

static void
ansi_alt_screen_enter(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    ansi_emit_str(ctx, "\033[?1049h");
    ansi_flush(vctx);
}

static void
ansi_alt_screen_leave(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    ansi_emit_str(ctx, "\033[?1049l");
    ansi_flush(vctx);
}

// -------------------------------------------------------------------
// Public vtable
// -------------------------------------------------------------------

const n00b_renderer_vtable_t n00b_renderer_ansi = {
    .name               = "ansi",
    .version            = N00B_RENDERER_ABI_VERSION,
    .init               = ansi_init,
    .destroy            = ansi_destroy,
    .capabilities       = ansi_capabilities,
    .get_size           = ansi_get_size,
    .render_frame       = ansi_render_frame,
    .flush              = ansi_flush,
    .cursor_set_visible = ansi_cursor_set_visible,
    .cursor_move        = ansi_cursor_move,
    .alt_screen_enter   = ansi_alt_screen_enter,
    .alt_screen_leave   = ansi_alt_screen_leave,
};
