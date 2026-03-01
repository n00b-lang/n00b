/*
 * Event loop: poll backend → dispatch events → re-render.
 *
 * For backends that do NOT manage their own terminal state (e.g. the
 * ANSI backend), the event loop enters raw mode, switches to the
 * alternate screen, and installs signal handlers that restore
 * everything on fatal signals.
 *
 * Backends that set N00B_RCAP_MANAGES_TTY (e.g. notcurses) handle
 * terminal state internally — the event loop leaves it alone.
 */

#include <stdio.h>
#include <signal.h>

#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#endif

#include "n00b.h"
#include "display/event_loop.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/widget.h"
#include "display/render/canvas.h"
#include "display/render/backend.h"
#include "display/render/plane.h"

// Global stop flag (set by n00b_canvas_run_stop).
static volatile bool g_run_stop = false;

// -------------------------------------------------------------------
// Terminal state saved for signal-safe restoration
// -------------------------------------------------------------------

#ifndef _WIN32
static struct termios    g_saved_termios;
static bool              g_termios_saved    = false;
static bool              g_manages_tty      = false;
static n00b_canvas_t    *g_signal_canvas    = nullptr;

/*
 * Signal handler — async-signal-safe.
 *
 * For backends that manage their own TTY (notcurses), we just set the
 * stop flag and let the loop exit normally so the backend's destroy
 * path can clean up.
 *
 * For backends where WE manage the TTY (ANSI), we restore terminal
 * state immediately (in case the process is about to die) and then
 * re-raise with SIG_DFL.
 */
static void
signal_cleanup_handler(int sig)
{
    if (!g_manages_tty) {
        static const char show_cursor[]  = "\033[?25h";
        static const char sgr_reset[]    = "\033[0m";
        static const char alt_leave[]    = "\033[?1049l";
        write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
        write(STDOUT_FILENO, sgr_reset,   sizeof(sgr_reset)   - 1);
        write(STDOUT_FILENO, alt_leave,   sizeof(alt_leave)    - 1);
    }

    // Restore original termios if we saved them.
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
        g_termios_saved = false;
    }

    g_signal_canvas = nullptr;

    // Re-raise with default handler so the parent gets the right exit status.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
event_loop_setup_terminal(n00b_canvas_t *canvas)
{
    g_signal_canvas = canvas;

    // Check if the backend manages terminal state itself.
    n00b_render_cap_t caps = canvas->vtable->capabilities(canvas->backend_ctx);
    g_manages_tty = (caps & N00B_RCAP_MANAGES_TTY) != 0;

    // Always save termios so the signal handler can restore on fatal
    // signals, even for backends that manage their own TTY state.
    if (isatty(STDIN_FILENO)) {
        tcgetattr(STDIN_FILENO, &g_saved_termios);
        g_termios_saved = true;
    }

    // Install signal handlers for cleanup.
    struct sigaction sa = {
        .sa_handler = signal_cleanup_handler,
        .sa_flags   = 0,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);

    if (g_manages_tty) {
        // Backend handles raw mode, alt screen, etc.
        return;
    }

    if (!isatty(STDIN_FILENO)) {
        return;
    }

    // Enter raw mode.
    struct termios raw = g_saved_termios;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // Enter alt screen.
    n00b_canvas_alt_screen_enter(canvas);
}

/*
 * Write terminal cleanup sequences directly to the fd, bypassing the
 * conduit/backend buffer.  The conduit may be async or already
 * partially shut down, so we can't rely on it for cleanup.
 */
static void
tty_write_raw(const char *seq)
{
    size_t len = strlen(seq);
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, seq, len);
        if (n <= 0) break;
        seq += n;
        len -= (size_t)n;
    }
}

static void
event_loop_teardown_terminal(n00b_canvas_t *canvas)
{
    (void)canvas;

    if (!g_manages_tty) {
        // We manage terminal state — write cleanup sequences directly
        // to stdout (the conduit may be async / shutting down).
        tty_write_raw("\033[?25h");    // Show cursor.
        tty_write_raw("\033[0m");      // Reset SGR.
        tty_write_raw("\033[?1049l");  // Leave alt screen.
    }

    // Restore termios (saved for all backends in setup).
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_termios_saved = false;
    }

    // Restore default signal handlers.
    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGHUP,  SIG_DFL);

    g_signal_canvas = nullptr;
    g_manages_tty   = false;
}
#endif /* _WIN32 */

// -------------------------------------------------------------------
// Dirty check: walk the plane tree for any dirty planes
// -------------------------------------------------------------------

static bool
plane_tree_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return false;
    }
    if (plane->flags & N00B_PLANE_DIRTY) {
        return true;
    }
    if (plane->children.data) {
        for (size_t i = 0; i < plane->children.len; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (child && plane_tree_dirty(child)) {
                return true;
            }
        }
    }
    return false;
}

static bool
canvas_any_dirty(n00b_canvas_t *canvas)
{
    if (canvas->needs_full_redraw) {
        return true;
    }
    if (canvas->planes.data) {
        for (size_t i = 0; i < canvas->planes.len; i++) {
            n00b_plane_t *p = canvas->planes.data[i];
            if (p && plane_tree_dirty(p)) {
                return true;
            }
        }
    }
    return false;
}

// -------------------------------------------------------------------
// Mark all planes dirty (used after resize)
// -------------------------------------------------------------------

static void
mark_plane_tree_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }
    plane->flags |= N00B_PLANE_DIRTY;
    if (plane->children.data) {
        for (size_t i = 0; i < plane->children.len; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (child) {
                mark_plane_tree_dirty(child);
            }
        }
    }
}

static void
canvas_mark_all_dirty(n00b_canvas_t *canvas)
{
    if (canvas->planes.data) {
        for (size_t i = 0; i < canvas->planes.len; i++) {
            n00b_plane_t *p = canvas->planes.data[i];
            if (p) {
                mark_plane_tree_dirty(p);
            }
        }
    }
}

// -------------------------------------------------------------------
// Re-render dirty widgets before compositing
// -------------------------------------------------------------------

static void
rerender_dirty(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }
    if ((plane->flags & N00B_PLANE_DIRTY) && plane->widget_vtable) {
        n00b_widget_render(plane);
    }
    if (plane->children.data) {
        for (size_t i = 0; i < plane->children.len; i++) {
            n00b_plane_t *child = plane->children.data[i];
            if (child) {
                rerender_dirty(child);
            }
        }
    }
}

static void
canvas_rerender_dirty(n00b_canvas_t *canvas)
{
    if (canvas->planes.data) {
        for (size_t i = 0; i < canvas->planes.len; i++) {
            n00b_plane_t *p = canvas->planes.data[i];
            if (p) {
                rerender_dirty(p);
            }
        }
    }
}

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

void
n00b_canvas_run(n00b_canvas_t *canvas) _kargs
{
    int32_t          tick_ms     = 16;
    n00b_resize_cb_t on_resize   = nullptr;
    void            *resize_data = nullptr;
}
{
    g_run_stop = false;

#ifndef _WIN32
    event_loop_setup_terminal(canvas);
#endif

    // Hide cursor during event loop.
    if (canvas->vtable->cursor_set_visible) {
        canvas->vtable->cursor_set_visible(canvas->backend_ctx, false);
    }

    // Create focus manager.
    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);

    // Initial render.
    n00b_canvas_render(canvas);

    // Warmup: discard spurious input events from backend initialization
    // (e.g. notcurses terminal probe responses).  The number of ticks
    // to drain is ceil(100ms / tick_ms).
    int warmup_ticks = (100 + tick_ms - 1) / tick_ms;

    while (!g_run_stop) {
        n00b_event_t event = { .type = N00B_EVENT_NONE };

        // Poll backend for input.
        if (canvas->vtable->poll_event) {
            canvas->vtable->poll_event(canvas->backend_ctx,
                                        tick_ms,
                                        &event);
        }

        // Normalize backend-specific key representations into
        // canonical form (e.g. raw byte 3 → Ctrl+c, '\r' → ENTER).
        n00b_event_normalize(&event);

        // During warmup, discard key events but still handle resize.
        if (warmup_ticks > 0) {
            warmup_ticks--;
            if (event.type == N00B_EVENT_KEY) {
                event.type = N00B_EVENT_NONE;
            }
        }

        if (event.type == N00B_EVENT_KEY) {
            uint32_t       key  = event.key.key;
            n00b_key_mod_t mods = event.key.mods;

            // Ctrl+C → quit (normalizer guarantees lowercase 'c').
            if (key == 'c' && (mods & N00B_MOD_CTRL)) {
                break;
            }

            // Tab → focus next.
            if (key == N00B_KEY_TAB && !(mods & N00B_MOD_SHIFT)) {
                n00b_focus_mgr_next(fm);
            }
            // Shift+Tab → focus prev.
            else if (key == N00B_KEY_TAB && (mods & N00B_MOD_SHIFT)) {
                n00b_focus_mgr_prev(fm);
            }
            // Dispatch to focused widget.
            else {
                n00b_plane_t *focused = n00b_focus_mgr_current(fm);
                if (focused) {
                    n00b_widget_handle_event(focused, &event);
                }
            }
        }
        else if (event.type == N00B_EVENT_RESIZE) {
            {
                FILE *df = fopen("/tmp/widget_demo.log", "a");
                if (df) {
                    setbuf(df, nullptr);
                    fprintf(df, "[event_loop] RESIZE event: %dx%d, planes=%zu\n",
                            (int)event.resize.cols, (int)event.resize.rows,
                            canvas->planes.len);
                    fclose(df);
                }
            }
            n00b_canvas_resize(canvas, event.resize.rows, event.resize.cols);
            if (on_resize) {
                on_resize(canvas, resize_data);
            }
            n00b_focus_mgr_rebuild(fm);
            canvas_mark_all_dirty(canvas);
            n00b_canvas_invalidate(canvas);
        }

        // Re-render if dirty.
        if (canvas_any_dirty(canvas)) {
            {
                FILE *df = fopen("/tmp/widget_demo.log", "a");
                if (df) {
                    setbuf(df, nullptr);
                    fprintf(df, "[event_loop] re-rendering (needs_full=%d, n_planes=%zu)\n",
                            canvas->needs_full_redraw, canvas->planes.len);
                    fclose(df);
                }
            }
            canvas_rerender_dirty(canvas);
            n00b_canvas_render(canvas);
        }
    }

    n00b_focus_mgr_destroy(fm);

    // Restore cursor visibility.
    if (canvas->vtable->cursor_set_visible) {
        canvas->vtable->cursor_set_visible(canvas->backend_ctx, true);
    }

#ifndef _WIN32
    event_loop_teardown_terminal(canvas);
#endif
}

void
n00b_canvas_run_stop(void)
{
    g_run_stop = true;
}
