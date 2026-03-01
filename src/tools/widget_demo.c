/*
 * Visual demo tool for eyeballing widgets across backends.
 *
 * Usage:
 *   widget_demo --widget label --backend tui    # ANSI alt-screen
 *   widget_demo --widget label --backend cocoa  # macOS Cocoa window
 *   widget_demo -w label -b tui                 # short forms
 *
 * Press Ctrl-C to exit.
 * Tab cycles focus, Space/Enter activates widgets.
 *
 * Debug log written to /tmp/widget_demo.log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#endif

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/label.h"
#include "display/widgets/divider.h"
#include "display/widgets/spacer.h"
#include "display/widgets/progress.h"
#include "display/widgets/button.h"
#include "display/widgets/checkbox.h"
#include "display/widgets/input.h"
#include "display/event.h"
#include "display/event_loop.h"
#include "display/focus.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/string_ops.h"
#if defined(__APPLE__)
#include "display/render/backend_cocoa.h"
#endif
#if defined(N00B_HAVE_NOTCURSES)
#include "display/render/backend_notcurses.h"
#endif

// ====================================================================
// Debug log
// ====================================================================

static FILE *g_log = nullptr;

static void
dbg(const char *fmt, ...)
{
    if (!g_log) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

static void
dbg_plane(const char *label, n00b_plane_t *p)
{
    dbg("  [%s] plane=%p  cols=%u rows=%u  vp_cols=%u vp_rows=%u  "
        "x=%d y=%d z=%d  flags=0x%04x  vtable=%p  children.len=%zu\n",
        label, (void *)p,
        p->total_cols, p->total_rows,
        p->vp_cols, p->vp_rows,
        p->x, p->y, p->z,
        p->flags, (void *)p->widget_vtable,
        p->children.data ? p->children.len : 0);
}

static void
dbg_plane_grid(const char *label, n00b_plane_t *p, n00b_isize_t max_rows,
               n00b_isize_t max_cols)
{
    dbg("  [%s] grid dump (up to %ux%u):\n", label, max_rows, max_cols);
    n00b_isize_t rows = p->total_rows < max_rows ? p->total_rows : max_rows;
    n00b_isize_t cols = p->total_cols < max_cols ? p->total_cols : max_cols;

    for (n00b_isize_t r = 0; r < rows; r++) {
        dbg("    row %2u: |", r);
        for (n00b_isize_t c = 0; c < cols; c++) {
            n00b_option_t(n00b_const_rcell_ptr_t) opt =
                n00b_plane_get_cell(p, r, c);
            if (n00b_option_is_set(opt)) {
                const n00b_rcell_t *cell = n00b_option_get(opt);
                if (cell->flags & N00B_CELL_OCCUPIED) {
                    if (cell->grapheme_len == 1) {
                        dbg("%c", cell->grapheme[0]);
                    }
                    else if (cell->grapheme_len > 1) {
                        dbg("?");
                    }
                    else {
                        dbg(".");
                    }
                }
                else {
                    dbg(" ");
                }
            }
            else {
                dbg("X"); // out of bounds
            }
        }
        dbg("|\n");
    }
}

static void
dbg_canvas(n00b_canvas_t *c)
{
    dbg("Canvas: vtable=%p  backend_ctx=%p  caps=0x%08x\n",
        (void *)c->vtable, c->backend_ctx, (unsigned)c->caps);
    dbg("  frame_rows=%u  frame_cols=%u  frame=%p  prev_frame=%p\n",
        c->frame_rows, c->frame_cols,
        (void *)c->frame, (void *)c->prev_frame);
    dbg("  planes.data=%p  planes.len=%zu  needs_full_redraw=%d  size_set=%d\n",
        (void *)c->planes.data, c->planes.len,
        c->needs_full_redraw, c->size_set);
}

static void
dbg_frame_sample(n00b_rcell_t *frame, n00b_isize_t rows, n00b_isize_t cols,
                 n00b_isize_t sample_rows)
{
    dbg("Frame sample (first %u rows of %ux%u):\n", sample_rows, rows, cols);
    n00b_isize_t nr = rows < sample_rows ? rows : sample_rows;
    n00b_isize_t nc = cols < 80 ? cols : 80;

    for (n00b_isize_t r = 0; r < nr; r++) {
        n00b_isize_t occupied = 0;
        for (n00b_isize_t c = 0; c < cols; c++) {
            if (frame[r * cols + c].flags & N00B_CELL_OCCUPIED) {
                occupied++;
            }
        }
        dbg("  row %2u: %u/%u occupied |", r, occupied, cols);
        for (n00b_isize_t c = 0; c < nc; c++) {
            n00b_rcell_t *cell = &frame[r * cols + c];
            if (cell->flags & N00B_CELL_OCCUPIED) {
                if (cell->grapheme_len >= 1) {
                    dbg("%c", cell->grapheme[0]);
                }
                else {
                    dbg(".");
                }
            }
            else {
                dbg(" ");
            }
        }
        dbg("|\n");
    }
}

// ====================================================================
// Terminal raw mode helpers (for "press any key to exit")
// ====================================================================

#ifndef _WIN32
static struct termios orig_termios;
static bool           raw_mode_active = false;

static void
restore_terminal(void)
{
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = false;
    }
}

static void
enter_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) {
        dbg("WARNING: stdin is not a tty, skipping raw mode\n");
        return;
    }
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(restore_terminal);

    struct termios raw = orig_termios;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = true;
    dbg("Raw mode entered.\n");
}
#endif

// ====================================================================
// Style helpers
// ====================================================================

static n00b_text_style_t *
make_style(n00b_tristate_t bold,
           n00b_tristate_t italic,
           uint32_t        fg_rgb)
{
    n00b_text_style_t *s = n00b_alloc(n00b_text_style_t);
    s->bold   = bold;
    s->italic = italic;
    s->fg_rgb = n00b_color_make(fg_rgb);
    return s;
}

// ====================================================================
// Label demo
// ====================================================================

static void
demo_label(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_label start ===\n");

    // Use the full canvas width; leave 2-col margin for the root border.
    n00b_isize_t frame_cols = canvas->frame_cols;
    n00b_isize_t frame_rows = canvas->frame_rows;
    n00b_isize_t label_cols = frame_cols - 2;

    dbg("frame: %u x %u, label_cols: %u\n", frame_cols, frame_rows, label_cols);

    // --- Title label (bold, centered, cyan) ---
    n00b_string_t *title_text = n00b_string_from_cstr("Label Widget Demo");
    dbg("title_text: u8_bytes=%zu codepoints=%zu data='%.*s'\n",
        title_text->u8_bytes, title_text->codepoints,
        (int)title_text->u8_bytes, title_text->data);

    n00b_text_style_t *title_style = make_style(N00B_TRI_YES,
                                                  N00B_TRI_NO,
                                                  0x00CED1);
    title_text = n00b_str_set_base_style(title_text, title_style);

    n00b_plane_t *title = n00b_label_new(title_text,
                                          .cols      = label_cols,
                                          .alignment = N00B_ALIGN_CENTER);
    dbg_plane("title", title);
    dbg_plane_grid("title", title, 2, label_cols);

    // --- Left-aligned label (green) ---
    n00b_string_t *left_text = n00b_string_from_cstr("Left-aligned (green)");
    n00b_text_style_t *left_style = make_style(N00B_TRI_NO,
                                                 N00B_TRI_NO,
                                                 0x00FF00);
    left_text = n00b_str_set_base_style(left_text, left_style);

    n00b_plane_t *left_lbl = n00b_label_new(left_text,
                                              .cols      = label_cols,
                                              .alignment = N00B_ALIGN_LEFT);
    dbg_plane("left", left_lbl);
    dbg_plane_grid("left", left_lbl, 2, label_cols);

    // --- Center-aligned label (yellow, bold) ---
    n00b_string_t *center_text = n00b_string_from_cstr("Center-aligned (bold yellow)");
    n00b_text_style_t *center_style = make_style(N00B_TRI_YES,
                                                   N00B_TRI_NO,
                                                   0xFFFF00);
    center_text = n00b_str_set_base_style(center_text, center_style);

    n00b_plane_t *center_lbl = n00b_label_new(center_text,
                                                .cols      = label_cols,
                                                .alignment = N00B_ALIGN_CENTER);
    dbg_plane("center", center_lbl);
    dbg_plane_grid("center", center_lbl, 2, label_cols);

    // --- Right-aligned label (magenta, italic) ---
    n00b_string_t *right_text = n00b_string_from_cstr("Right-aligned (italic magenta)");
    n00b_text_style_t *right_style = make_style(N00B_TRI_NO,
                                                  N00B_TRI_YES,
                                                  0xFF00FF);
    right_text = n00b_str_set_base_style(right_text, right_style);

    n00b_plane_t *right_lbl = n00b_label_new(right_text,
                                               .cols      = label_cols,
                                               .alignment = N00B_ALIGN_RIGHT);
    dbg_plane("right", right_lbl);
    dbg_plane_grid("right", right_lbl, 2, label_cols);

    // --- Plain unstyled label ---
    n00b_string_t *plain_text = n00b_string_from_cstr("Plain unstyled text");
    n00b_plane_t  *plain_lbl  = n00b_label_new(plain_text, .cols = label_cols);
    dbg_plane("plain", plain_lbl);
    dbg_plane_grid("plain", plain_lbl, 2, label_cols);

    // --- Wrapped label (red on wider text into narrow box) ---
    n00b_string_t *wrap_text = n00b_string_from_cstr(
        "This is a longer line of text that is designed to wrap across multiple rows "
        "when rendered in the terminal. It demonstrates word-wrap behavior in the label "
        "widget, flowing naturally across line boundaries without breaking mid-word.");
    n00b_text_style_t *wrap_style = make_style(N00B_TRI_NO,
                                                 N00B_TRI_NO,
                                                 0xFF6347);
    wrap_text = n00b_str_set_base_style(wrap_text, wrap_style);

    n00b_plane_t *wrap_lbl = n00b_label_new(wrap_text,
                                              .cols = label_cols,
                                              .rows = 5,
                                              .wrap = true);
    dbg_plane("wrap", wrap_lbl);
    dbg_plane_grid("wrap", wrap_lbl, 6, label_cols);

    // Create a root plane spanning the full frame.
    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane,
                                         .cols = frame_cols,
                                         .rows = frame_rows);
    dbg_plane("root (before children)", root);

    // Position children vertically within root (1-col left margin).
    n00b_plane_add_child(root, title,      1, 0);
    n00b_plane_add_child(root, left_lbl,   1, 2);
    n00b_plane_add_child(root, center_lbl, 1, 4);
    n00b_plane_add_child(root, right_lbl,  1, 6);
    n00b_plane_add_child(root, plain_lbl,  1, 8);
    n00b_plane_add_child(root, wrap_lbl,   1, 10);

    dbg_plane("root (after children)", root);

    n00b_canvas_add_plane(canvas, root);

    dbg("Canvas after add_plane:\n");
    dbg_canvas(canvas);

    dbg("=== demo_label end ===\n\n");
}

// ====================================================================
// All-widgets interactive demo
// ====================================================================

// State shared between widgets in the all-widgets demo.
static n00b_plane_t  *g_root_plane    = nullptr;
static n00b_plane_t  *g_status_label  = nullptr;
static n00b_plane_t  *g_progress_bar  = nullptr;
static bool           g_auto_progress = false;

/**
 * Resize callback: update root plane dimensions when the terminal
 * is resized.  Widget positions stay fixed (vertical layout) — we
 * only need to expand/shrink the root and content-width widgets.
 */
static void
on_resize(n00b_canvas_t *canvas, void *data)
{
    (void)data;
    if (!g_root_plane) {
        return;
    }

    n00b_isize_t new_cols = canvas->frame_cols;
    n00b_isize_t new_rows = canvas->frame_rows;
    n00b_isize_t content_cols = new_cols - 2;

    // Keep row count at the content height (or clamp to terminal).
    n00b_isize_t root_rows = g_root_plane->total_rows;
    if (root_rows > new_rows) {
        root_rows = new_rows;
    }

    // Resize root plane — reallocates the grid.
    n00b_plane_resize(g_root_plane, root_rows, new_cols);
    g_root_plane->vp_cols = new_cols;
    g_root_plane->vp_rows = root_rows;

    // Resize all child widget planes to the new content width.
    if (g_root_plane->children.data) {
        for (size_t i = 0; i < g_root_plane->children.len; i++) {
            n00b_plane_t *child = g_root_plane->children.data[i];
            if (!child) {
                continue;
            }
            // Skip widgets with fixed width (buttons stay their own size).
            if (child->total_cols <= 20) {
                continue;
            }
            n00b_plane_resize(child, child->total_rows, content_cols);
            child->vp_cols = content_cols;
        }
    }
}

static void
on_button_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    if (g_status_label) {
        n00b_label_set_text(g_status_label,
                             n00b_string_from_cstr("Button clicked!"));
    }
}

static void
on_checkbox_change(n00b_plane_t *plane, bool checked, void *data)
{
    (void)plane;
    (void)data;
    g_auto_progress = checked;
    if (g_status_label) {
        n00b_label_set_text(g_status_label,
                             n00b_string_from_cstr(
                                 checked ? "Auto-progress enabled"
                                         : "Auto-progress disabled"));
    }
}

static void
on_input_submit(n00b_plane_t *plane, n00b_string_t *text, void *data)
{
    (void)plane;
    (void)data;
    if (g_status_label && text) {
        // Build "Input: <text>" message.
        n00b_string_t *prefix = n00b_string_from_cstr("Input: ");
        n00b_string_t *msg    = n00b_unicode_str_cat(prefix, text);
        n00b_label_set_text(g_status_label, msg);
    }
}

static void
demo_all(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_all start ===\n");

    n00b_isize_t frame_cols = canvas->frame_cols;
    n00b_isize_t frame_rows = canvas->frame_rows;
    n00b_isize_t content_cols = frame_cols - 2;

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane,
                                          .cols = frame_cols,
                                          .rows = frame_rows);
    g_root_plane = root;

    n00b_isize_t y = 0;

    // Title label.
    n00b_text_style_t *title_style = make_style(N00B_TRI_YES, N00B_TRI_NO, 0x00CED1);
    n00b_string_t *title_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Widget Demo - All Widgets"), title_style);
    n00b_plane_t *title = n00b_label_new(title_text,
                                           .cols = content_cols,
                                           .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, title, 1, y);
    y += 2;

    // Horizontal divider.
    n00b_plane_t *div1 = n00b_divider_new(.cols = content_cols);
    n00b_plane_add_child(root, div1, 1, y);
    y += 2;

    // Progress bar.
    g_progress_bar = n00b_progress_new(.cols = content_cols, .value = 0.3);
    n00b_plane_add_child(root, g_progress_bar, 1, y);
    y += 2;

    // Spacer.
    n00b_plane_t *sp = n00b_spacer_new(.cols = content_cols, .rows = 1);
    n00b_plane_add_child(root, sp, 1, y);
    y += 1;

    // Button.
    n00b_plane_t *btn = n00b_button_new(
        n00b_string_from_cstr("Click Me"),
        .on_click = on_button_click);
    n00b_plane_add_child(root, btn, 1, y);
    y += 6; // Button outer height = 3 content + 2 border = 5, plus 1 gap.

    // Checkbox.
    n00b_plane_t *cb = n00b_checkbox_new(
        n00b_string_from_cstr("Auto-progress"),
        .on_change = on_checkbox_change);
    n00b_plane_add_child(root, cb, 1, y);
    y += 2;

    // Text input.
    n00b_string_t *ph = n00b_string_from_cstr("Type here...");
    n00b_plane_t *inp = n00b_input_new(.cols = content_cols,
                                         .placeholder = ph,
                                         .on_submit = on_input_submit);
    n00b_plane_add_child(root, inp, 1, y);
    y += 2;

    // Divider before status.
    n00b_plane_t *div2 = n00b_divider_new(.cols = content_cols,
                                            .label = n00b_string_from_cstr("Status"));
    n00b_plane_add_child(root, div2, 1, y);
    y += 2;

    // Status label.
    n00b_text_style_t *status_style = make_style(N00B_TRI_NO, N00B_TRI_YES, 0xAAAAAA);
    n00b_string_t *status_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Ready. Tab to navigate, Enter/Space to interact."),
        status_style);
    g_status_label = n00b_label_new(status_text, .cols = content_cols);
    n00b_plane_add_child(root, g_status_label, 1, y);
    y += 2;

    // Size root plane to actual content height instead of full terminal.
    root->total_rows = (n00b_isize_t)y;
    root->vp_rows    = (n00b_isize_t)y;

    n00b_canvas_add_plane(canvas, root);
    dbg("=== demo_all end ===\n\n");
}

// ====================================================================
// Usage
// ====================================================================

static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --widget <name> --backend <tui|cocoa|nc>\n"
            "\n"
            "Widgets:  label, all\n"
            "Backends: tui (ANSI alt-screen), cocoa (macOS native),\n"
            "          nc / notcurses (pixel + cell-based terminal)\n"
            "\n"
            "Short flags: -w <widget> -b <backend>\n",
            prog);
    exit(1);
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    const char *widget_name  = nullptr;
    const char *backend_name = nullptr;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--widget") == 0 || strcmp(argv[i], "-w") == 0)
            && i + 1 < argc) {
            widget_name = argv[++i];
        }
        else if ((strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "-b") == 0)
                 && i + 1 < argc) {
            backend_name = argv[++i];
        }
        else {
            usage(argv[0]);
        }
    }

    if (!widget_name || !backend_name) {
        usage(argv[0]);
    }

    // Open debug log.
    g_log = fopen("/tmp/widget_demo.log", "w");
    if (!g_log) {
        fprintf(stderr, "Warning: could not open /tmp/widget_demo.log\n");
    }
    dbg("widget_demo started: widget=%s backend=%s\n", widget_name, backend_name);

    // Select backend vtable.
    const n00b_renderer_vtable_t *vtable = nullptr;

    if (strcmp(backend_name, "tui") == 0) {
        vtable = &n00b_renderer_ansi;
    }
#if defined(__APPLE__)
    else if (strcmp(backend_name, "cocoa") == 0) {
        vtable = &n00b_renderer_cocoa;
    }
#endif
#if defined(N00B_HAVE_NOTCURSES)
    else if (strcmp(backend_name, "nc") == 0
             || strcmp(backend_name, "notcurses") == 0) {
        vtable = &n00b_renderer_notcurses;
    }
#endif
    else {
        fprintf(stderr, "Unknown backend: %s\n", backend_name);
        usage(argv[0]);
    }

    dbg("Selected vtable: name='%s' version=%u\n", vtable->name, vtable->version);

    // Initialize runtime.
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    dbg("Runtime initialized.\n");

    // Get the runtime's stdout conduit topic — the ANSI backend writes
    // rendered escape sequences here, and the fd_writer subscriber
    // forwards them to fd 1.
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    dbg("stdout_topic = %p\n", (void *)stdout_topic);

    // Create canvas with chosen backend.  Heap-allocate so the GC
    // can trace through it to find all reachable planes and widgets.
    n00b_canvas_t *canvas = n00b_alloc(n00b_canvas_t);
    n00b_canvas_init(canvas, .vtable = vtable, .output = stdout_topic);

    dbg("Canvas initialized:\n");
    dbg_canvas(canvas);

    // Dispatch to widget demo.
    bool use_event_loop = false;
    if (strcmp(widget_name, "label") == 0) {
        demo_label(canvas);
    }
    else if (strcmp(widget_name, "all") == 0) {
        demo_all(canvas);
        use_event_loop = true;
    }
    else {
        fprintf(stderr, "Unknown widget: %s\n", widget_name);
        n00b_shutdown();
        return 1;
    }

    // Render.
    dbg("\n=== About to render ===\n");

    if (strcmp(backend_name, "tui") == 0
        || strcmp(backend_name, "nc") == 0
        || strcmp(backend_name, "notcurses") == 0) {

        if (use_event_loop) {
            // Interactive event loop for "all" mode.
            // n00b_canvas_run manages raw mode, alt screen, and
            // signal-safe cleanup internally.
            dbg("Starting event loop...\n");
            n00b_canvas_run(canvas, .tick_ms = 50,
                             .on_resize = on_resize);
            dbg("Event loop exited.\n");
        }
        else {
            dbg("Entering alt screen...\n");
            n00b_canvas_alt_screen_enter(canvas);

            dbg("Calling canvas_render...\n");
            n00b_canvas_render(canvas);

            dbg("After render:\n");
            dbg_canvas(canvas);
            if (canvas->frame) {
                dbg_frame_sample(canvas->frame, canvas->frame_rows,
                                 canvas->frame_cols, 16);
            }

#ifndef _WIN32
            dbg("Entering raw mode...\n");
            enter_raw_mode();

            dbg("Draining terminal responses...\n");
            {
                struct termios tmp;
                tcgetattr(STDIN_FILENO, &tmp);

                tmp.c_cc[VMIN]  = 0;
                tmp.c_cc[VTIME] = 1;
                tcsetattr(STDIN_FILENO, TCSANOW, &tmp);

                char drain_buf[256];
                ssize_t n;
                int total_drained = 0;

                while ((n = read(STDIN_FILENO, drain_buf, sizeof(drain_buf))) > 0) {
                    total_drained += (int)n;
                    dbg("Drained %zd bytes (total %d)\n", n, total_drained);
                }
                dbg("Drain complete: %d bytes total.\n", total_drained);

                tmp.c_cc[VMIN]  = 1;
                tmp.c_cc[VTIME] = 0;
                tcsetattr(STDIN_FILENO, TCSANOW, &tmp);
            }

            {
                int fl = fcntl(STDIN_FILENO, F_GETFL);
                if (fl != -1 && (fl & O_NONBLOCK)) {
                    dbg("Clearing O_NONBLOCK on stdin (flags were 0x%x)\n", fl);
                    fcntl(STDIN_FILENO, F_SETFL, fl & ~O_NONBLOCK);
                }
            }

            dbg("Waiting for keypress...\n");
            {
                char c = 0;
                ssize_t rc;
                struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };

                for (;;) {
                    int pr = poll(&pfd, 1, 60000);
                    dbg("poll() returned %d, revents=0x%x\n", pr, pfd.revents);

                    if (pr <= 0) {
                        break;
                    }

                    c  = 0;
                    rc = read(STDIN_FILENO, &c, 1);

                    if (rc == 1 && c != 0) {
                        break;
                    }
                    if (rc == 0 || (rc < 0 && errno != EINTR && errno != EAGAIN)) {
                        break;
                    }
                }
            }
            restore_terminal();
#endif

            dbg("Leaving alt screen...\n");
            n00b_canvas_alt_screen_leave(canvas);
        }
    }
    else {
        // Cocoa backend — use the event loop for interactive mode,
        // static render + pump for non-interactive demos.
        if (use_event_loop) {
            dbg("Starting Cocoa event loop...\n");
            n00b_canvas_run(canvas, .tick_ms = 50,
                             .on_resize = on_resize);
            dbg("Cocoa event loop exited.\n");
        }
        else {
            dbg("Calling canvas_render (cocoa)...\n");
            n00b_canvas_render(canvas);

            dbg("After render:\n");
            dbg_canvas(canvas);
            if (canvas->frame) {
                dbg_frame_sample(canvas->frame, canvas->frame_rows,
                                 canvas->frame_cols, 16);
            }

#if defined(__APPLE__)
            dbg("Pumping NSRunLoop...\n");
            for (int i = 0; i < 120; i++) {
                n00b_cocoa_run_loop_pump(0.5);
            }
#endif
        }
    }

    dbg("Shutting down...\n");
    n00b_canvas_destroy(canvas);
    n00b_shutdown();

    if (g_log) {
        fclose(g_log);
    }
    return 0;
}
