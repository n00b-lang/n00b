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
#include <time.h>

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
#include "internal/display/terminal_lifecycle.h"

// Global stop flag (set by n00b_canvas_run_stop).
static volatile bool g_run_stop = false;

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

    if (!n00b_canvas_backend_ready(canvas)) {
        n00b_display_diag_log(N00B_DISPLAY_DIAG_ERROR,
                              "event_loop",
                              "run skipped: backend not ready err=%d",
                              n00b_canvas_backend_error(canvas));
        n00b_display_diag_shutdown();
        return;
    }

    n00b_display_terminal_setup(canvas);

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
            // Prefer the backend's exact pixel size when it is available.
            n00b_render_size_t size = n00b_display_backend_get_size(canvas);
            if (size.cell_pixel_w > 0) {
                canvas->cell_px_w = size.cell_pixel_w;
            }
            if (size.cell_pixel_h > 0) {
                canvas->cell_px_h = size.cell_pixel_h;
            }

            n00b_isize_t px_rows = size.pixel_h > 0
                                 ? size.pixel_h
                                 : event.resize.rows * canvas->cell_px_h;
            n00b_isize_t px_cols = size.pixel_w > 0
                                 ? size.pixel_w
                                 : event.resize.cols * canvas->cell_px_w;
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

    n00b_display_terminal_teardown(canvas);
    n00b_display_diag_shutdown();
}

void
n00b_canvas_run_stop(void)
{
    g_run_stop = true;
}
