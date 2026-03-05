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
#include <time.h>

#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#endif

#include "n00b.h"
#include "display/event_loop.h"
#include "display/event.h"
#include "display/focus.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "internal/display/backend_services.h"
#include "internal/display/diagnostics.h"
#include "internal/display/event_dispatch.h"
#include "internal/display/scene_contracts.h"

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
        static const char mouse_off[]    = "\033[?1006l\033[?1002l\033[?1000l";
        static const char show_cursor[]  = "\033[?25h";
        static const char sgr_reset[]    = "\033[0m";
        static const char alt_leave[]    = "\033[?1049l";
        write(STDOUT_FILENO, mouse_off,   sizeof(mouse_off)   - 1);
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

static void tty_write_raw(const char *seq);

static void
event_loop_setup_terminal(n00b_canvas_t *canvas)
{
    g_signal_canvas = canvas;

    // Check if the backend manages terminal state itself.
    n00b_render_cap_t caps = n00b_display_backend_caps(canvas);
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

    // Enable SGR mouse protocol if the backend reports mouse support.
    // Only button + drag — NOT all-motion (1003h) which floods the
    // event loop with motion events on every pixel movement.
    if (caps & N00B_RCAP_MOUSE) {
        tty_write_raw("\033[?1000h");  // Button events.
        tty_write_raw("\033[?1002h");  // Button + drag events.
        tty_write_raw("\033[?1006h");  // SGR extended coordinates.
    }
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
        tty_write_raw("\033[?1006l");  // Disable SGR extended mouse.
        tty_write_raw("\033[?1002l");  // Disable button+drag.
        tty_write_raw("\033[?1000l");  // Disable button events.
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
    n00b_display_diag_init();

#ifndef _WIN32
    event_loop_setup_terminal(canvas);
#endif

    // Hide cursor during event loop.
    n00b_display_backend_set_cursor_visible(canvas, false);

    // Create focus manager.
    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);

#define PROF_NOW(tv) clock_gettime(CLOCK_MONOTONIC, &(tv))
#define PROF_MS(a, b) \
    (((b).tv_sec - (a).tv_sec) * 1000.0 + \
     ((b).tv_nsec - (a).tv_nsec) / 1e6)

    struct timespec t0, t1, t2, t3;

    // Initial layout cascade + render.
    PROF_NOW(t0);
    n00b_display_scene_run_layout(canvas);
    PROF_NOW(t1);
    n00b_display_scene_mark_all_dirty(canvas);
    n00b_display_scene_rerender_dirty(canvas);
    PROF_NOW(t2);
    n00b_canvas_render(canvas);
    PROF_NOW(t3);

    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "event_loop",
                           "initial-render layout=%.2fms rerender=%.2fms canvas=%.2fms total=%.2fms",
                           PROF_MS(t0, t1),
                           PROF_MS(t1, t2),
                           PROF_MS(t2, t3),
                           PROF_MS(t0, t3));

    // Warmup: discard spurious input events from backend initialization
    // (e.g. notcurses terminal probe responses).  The number of ticks
    // to drain is ceil(100ms / tick_ms).
    int warmup_ticks = (100 + tick_ms - 1) / tick_ms;

    int tick_count   = 0;
    int render_count = 0;

    while (!g_run_stop) {
        n00b_event_t event = { .type = N00B_EVENT_NONE };

        // Poll backend for input.
        PROF_NOW(t0);
        (void)n00b_display_backend_poll_event(canvas, tick_ms, &event);
        PROF_NOW(t1);

        tick_count++;
        if (tick_count <= 20) {
            n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                   "event_loop",
                                   "tick=%d poll=%.2fms event=%d",
                                   tick_count,
                                   PROF_MS(t0, t1),
                                   (int)event.type);
        }

        // Normalize backend-specific key representations into
        // canonical form (e.g. raw byte 3 → Ctrl+c, '\r' → ENTER).
        n00b_event_normalize(&event);

        // During warmup, discard key and mouse events but still handle resize.
        if (warmup_ticks > 0) {
            warmup_ticks--;
            if (event.type == N00B_EVENT_KEY || event.type == N00B_EVENT_MOUSE) {
                event.type = N00B_EVENT_NONE;
            }
        }

        if (event.type == N00B_EVENT_RESIZE) {
            // Backend reports resize in cells; convert to pixels.
            n00b_isize_t px_rows = event.resize.rows * canvas->cell_px_h;
            n00b_isize_t px_cols = event.resize.cols * canvas->cell_px_w;
            n00b_canvas_resize(canvas, px_rows, px_cols);
            n00b_display_scene_run_layout(canvas);
            if (on_resize) {
                on_resize(canvas, resize_data);
            }
            n00b_focus_mgr_rebuild(fm);
            n00b_display_scene_mark_all_dirty(canvas);
            n00b_canvas_invalidate(canvas);
        }
        else {
            n00b_display_dispatch_result_t dispatch =
                n00b_display_dispatch_event(canvas, fm, &event);
            if (dispatch.should_stop) {
                break;
            }
        }

        // Re-render if dirty.
        if (n00b_display_scene_any_dirty(canvas)) {
            if (n00b_display_diag_would_log(N00B_DISPLAY_DIAG_TRACE)) {
                // Log which planes are dirty before rerender.
                for (size_t di = 0; di < canvas->planes.len; di++) {
                    n00b_plane_t *dp = canvas->planes.data[di];
                    if (dp && (dp->flags & N00B_PLANE_DIRTY)) {
                        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                               "event_loop",
                                               "dirty-before plane=%p gen=%u",
                                               (void *)dp,
                                               dp->render_gen);
                    }
                }
            }
            PROF_NOW(t1);
            n00b_display_scene_rerender_dirty(canvas);
            PROF_NOW(t2);
            // Check if anything is STILL dirty after rerender.
            if (n00b_display_diag_would_log(N00B_DISPLAY_DIAG_TRACE)
                && n00b_display_scene_any_dirty(canvas)) {
                for (size_t di = 0; di < canvas->planes.len; di++) {
                    n00b_plane_t *dp = canvas->planes.data[di];
                    if (dp && (dp->flags & N00B_PLANE_DIRTY)) {
                        n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                               "event_loop",
                                               "still-dirty plane=%p gen=%u widget=%d",
                                               (void *)dp,
                                               dp->render_gen,
                                               dp->widget_vtable != nullptr);
                    }
                }
            }
            n00b_canvas_render(canvas);
            PROF_NOW(t3);

            render_count++;
            n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                                   "event_loop",
                                   "tick-render=%d rerender=%.2fms canvas=%.2fms",
                                   render_count,
                                   PROF_MS(t1, t2),
                                   PROF_MS(t2, t3));
        }
    }

    n00b_display_diag_log(N00B_DISPLAY_DIAG_TRACE,
                           "event_loop",
                           "exit ticks=%d renders=%d",
                           tick_count,
                           render_count);

    n00b_focus_mgr_destroy(fm);

    // Restore cursor visibility.
    n00b_display_backend_set_cursor_visible(canvas, true);

#ifndef _WIN32
    event_loop_teardown_terminal(canvas);
#endif
}

void
n00b_canvas_run_stop(void)
{
    g_run_stop = true;
}
