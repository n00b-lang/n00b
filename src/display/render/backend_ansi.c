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
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#endif
#include "n00b.h"
#include "core/alloc.h"
#include "display/render/backend.h"
#include "display/event.h"
#include "conduit/write.h"
#include "conduit/signal.h"
#include "text/strings/text_style.h"
#include "text/strings/theme.h"

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
#ifndef _WIN32
    n00b_conduit_signal_inbox_t            *sigwinch_inbox;
#endif
} ansi_ctx_t;

#define ANSI_INITIAL_BUF 16384

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

    // Try to get terminal size.
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        ctx->cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        ctx->rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
        ctx->rows = ws.ws_row;
    }
#endif
    else {
        ctx->cols = 80;
        ctx->rows = 25;
    }

#ifndef _WIN32
    {
        // Subscribe to SIGWINCH via the conduit signal system.
        FILE *df = fopen("/tmp/widget_demo.log", "a");
        if (df) setbuf(df, nullptr);

        if (!output) {
            if (df) fprintf(df, "[ansi_init] output is NULL, skipping SIGWINCH\n");
        }
        else {
            n00b_conduit_topic_base_t *base = (n00b_conduit_topic_base_t *)output;
            n00b_conduit_t *c = base->conduit;
            if (df) fprintf(df, "[ansi_init] output=%p base->conduit=%p\n", (void *)output, (void *)c);
            if (c) {
                n00b_result_t(n00b_conduit_topic_base_t *) sr =
                    n00b_conduit_signal_topic(c, SIGWINCH);
                if (n00b_result_is_ok(sr)) {
                    ctx->sigwinch_inbox = n00b_conduit_signal_inbox_new(c);
                    n00b_conduit_signal_subscribe(
                        n00b_result_get(sr), ctx->sigwinch_inbox,
                        .operations = N00B_CONDUIT_SIGNAL_ALL);
                    if (df) fprintf(df, "[ansi_init] SIGWINCH subscribed, inbox=%p\n", (void *)ctx->sigwinch_inbox);
                }
                else {
                    if (df) fprintf(df, "[ansi_init] n00b_conduit_signal_topic FAILED\n");
                }
            }
            else {
                if (df) fprintf(df, "[ansi_init] conduit is NULL\n");
            }
        }
        if (df) fclose(df);
    }
#endif

    return ctx;
}

static void
ansi_destroy(void *vctx)
{
    ansi_ctx_t *ctx = vctx;
    if (ctx) {
#ifndef _WIN32
        if (ctx->sigwinch_inbox) {
            n00b_conduit_topic_base_t *base =
                (n00b_conduit_topic_base_t *)ctx->output;
            if (base && base->conduit) {
                n00b_conduit_signal_unwatch(base->conduit, SIGWINCH);
            }
        }
#endif
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

    // Re-query terminal size.
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        ctx->cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        ctx->rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->cols = ws.ws_col;
        ctx->rows = ws.ws_row;
    }
#endif

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

    if (ctx->buf_used == 0) {
        return;
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
// Event polling
// -------------------------------------------------------------------

#ifndef _WIN32

// Try to read one byte from stdin (non-blocking).
// Returns the byte, or -1 if nothing available.
static int
ansi_try_read_byte(void)
{
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        return (int)c;
    }
    return -1;
}

// Read one byte, waiting up to timeout_ms.  Uses poll() for sub-sequence
// reads (CSI/SS3 parsing) where we don't need SIGWINCH responsiveness.
static int
ansi_read_byte(int timeout_ms)
{
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) {
        return -1;
    }
    return ansi_try_read_byte();
}

static bool
ansi_parse_csi(n00b_event_t *out)
{
    char   buf[32];
    size_t len = 0;

    for (;;) {
        int c = ansi_read_byte(50);
        if (c < 0) {
            return false;
        }
        if (c >= 0x40 && c <= 0x7E) {
            // Final byte.
            out->type     = N00B_EVENT_KEY;
            out->key.mods = N00B_MOD_NONE;

            switch (c) {
            case 'A': out->key.key = N00B_KEY_UP;        return true;
            case 'B': out->key.key = N00B_KEY_DOWN;      return true;
            case 'C': out->key.key = N00B_KEY_RIGHT;     return true;
            case 'D': out->key.key = N00B_KEY_LEFT;      return true;
            case 'H': out->key.key = N00B_KEY_HOME;      return true;
            case 'F': out->key.key = N00B_KEY_END;       return true;
            case 'Z':
                out->key.key  = N00B_KEY_TAB;
                out->key.mods = N00B_MOD_SHIFT;
                return true;
            case '~':
                if (len > 0) {
                    int num = 0;
                    for (size_t i = 0; i < len; i++) {
                        if (buf[i] >= '0' && buf[i] <= '9') {
                            num = num * 10 + (buf[i] - '0');
                        }
                        else {
                            break;
                        }
                    }
                    switch (num) {
                    case 1:  out->key.key = N00B_KEY_HOME;      return true;
                    case 2:  out->key.key = N00B_KEY_INSERT;    return true;
                    case 3:  out->key.key = N00B_KEY_DELETE;    return true;
                    case 4:  out->key.key = N00B_KEY_END;       return true;
                    case 5:  out->key.key = N00B_KEY_PAGE_UP;   return true;
                    case 6:  out->key.key = N00B_KEY_PAGE_DOWN; return true;
                    case 15: out->key.key = N00B_KEY_F5;        return true;
                    case 17: out->key.key = N00B_KEY_F6;        return true;
                    case 18: out->key.key = N00B_KEY_F7;        return true;
                    case 19: out->key.key = N00B_KEY_F8;        return true;
                    case 20: out->key.key = N00B_KEY_F9;        return true;
                    case 21: out->key.key = N00B_KEY_F10;       return true;
                    case 23: out->key.key = N00B_KEY_F11;       return true;
                    case 24: out->key.key = N00B_KEY_F12;       return true;
                    default: return false;
                    }
                }
                return false;
            default:
                return false;
            }
        }
        if (len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
        }
    }
}

static bool
ansi_parse_ss3(n00b_event_t *out)
{
    int c = ansi_read_byte(50);
    if (c < 0) {
        return false;
    }

    out->type     = N00B_EVENT_KEY;
    out->key.mods = N00B_MOD_NONE;

    switch (c) {
    case 'P': out->key.key = N00B_KEY_F1;   return true;
    case 'Q': out->key.key = N00B_KEY_F2;   return true;
    case 'R': out->key.key = N00B_KEY_F3;   return true;
    case 'S': out->key.key = N00B_KEY_F4;   return true;
    case 'A': out->key.key = N00B_KEY_UP;   return true;
    case 'B': out->key.key = N00B_KEY_DOWN; return true;
    case 'C': out->key.key = N00B_KEY_RIGHT;return true;
    case 'D': out->key.key = N00B_KEY_LEFT; return true;
    default:  return false;
    }
}

static FILE *
ansi_dbg(void)
{
    static FILE *f = nullptr;
    if (!f) {
        f = fopen("/tmp/widget_demo.log", "a");
        if (f) setbuf(f, nullptr);
    }
    return f;
}

static bool
ansi_check_sigwinch(ansi_ctx_t *ctx, n00b_event_t *out)
{
    if (!ctx->sigwinch_inbox) {
        return false;
    }
    if (!n00b_conduit_signal_inbox_has_messages(ctx->sigwinch_inbox)) {
        return false;
    }
    if (ansi_dbg()) fprintf(ansi_dbg(), "[ansi_check_sigwinch] inbox has messages!\n");
    while (n00b_conduit_signal_inbox_has_messages(ctx->sigwinch_inbox)) {
        n00b_conduit_signal_inbox_pop(ctx->sigwinch_inbox);
    }
    struct winsize ws;
    if (ioctl(ctx->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        ctx->rows = (n00b_isize_t)ws.ws_row;
        ctx->cols = (n00b_isize_t)ws.ws_col;
        out->type        = N00B_EVENT_RESIZE;
        out->resize.rows = ctx->rows;
        out->resize.cols = ctx->cols;
        if (ansi_dbg()) fprintf(ansi_dbg(), "[ansi_check_sigwinch] resize → %dx%d\n", (int)ws.ws_col, (int)ws.ws_row);
        return true;
    }
    return false;
}

static bool
ansi_poll_event(void *vctx, int32_t timeout_ms, n00b_event_t *out)
{
    ansi_ctx_t *ctx = vctx;
    out->type = N00B_EVENT_NONE;

    // Check for pending SIGWINCH.
    if (ansi_check_sigwinch(ctx, out)) {
        return true;
    }

    // Try non-blocking read from stdin.
    int c = ansi_try_read_byte();

    if (c < 0) {
        // No stdin data yet.  Wait on the inbox CV so we wake
        // immediately when SIGWINCH arrives, rather than sleeping
        // the full tick.  The CV times out after timeout_ms, giving
        // us the same polling cadence as before for keyboard input.
        if (ctx->sigwinch_inbox && timeout_ms > 0) {
            int64_t timeout_ns = (int64_t)timeout_ms * 1000000LL;
            n00b_condition_wait(&ctx->sigwinch_inbox->cv,
                                .timeout = timeout_ns);
        }
        else if (timeout_ms > 0) {
            // No inbox — fall back to poll() on stdin.
            struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
            poll(&pfd, 1, timeout_ms);
        }

        // Re-check SIGWINCH after waking.
        if (ansi_check_sigwinch(ctx, out)) {
            return true;
        }

        // Try stdin again.
        c = ansi_try_read_byte();
        if (c < 0) {
            return false;
        }
    }

    // ESC: start of escape sequence.
    if (c == 0x1B) {
        int next = ansi_read_byte(50);
        if (next < 0) {
            out->type     = N00B_EVENT_KEY;
            out->key.key  = N00B_KEY_ESCAPE;
            out->key.mods = N00B_MOD_NONE;
            return true;
        }
        if (next == '[') {
            return ansi_parse_csi(out);
        }
        if (next == 'O') {
            return ansi_parse_ss3(out);
        }
        // Alt+key.
        out->type     = N00B_EVENT_KEY;
        out->key.key  = (uint32_t)next;
        out->key.mods = N00B_MOD_ALT;
        return true;
    }

    // Control characters.
    out->type     = N00B_EVENT_KEY;
    out->key.mods = N00B_MOD_NONE;

    if (c == 0x7F || c == 0x08) {
        out->key.key = N00B_KEY_BACKSPACE;
        return true;
    }
    if (c == '\r' || c == '\n') {
        out->key.key = N00B_KEY_ENTER;
        return true;
    }
    if (c == '\t') {
        out->key.key = N00B_KEY_TAB;
        return true;
    }
    if (c == 0x19) {
        // Shift+Tab (backtab).
        out->key.key  = N00B_KEY_TAB;
        out->key.mods = N00B_MOD_SHIFT;
        return true;
    }
    if (c < 0x20) {
        out->key.key  = (uint32_t)(c + 'a' - 1);
        out->key.mods = N00B_MOD_CTRL;
        return true;
    }

    // Printable ASCII.
    out->key.key = (uint32_t)c;
    return true;
}
#endif // !_WIN32

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
#ifndef _WIN32
    .poll_event         = ansi_poll_event,
#endif
};
