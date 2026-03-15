/*
 * Visual demo tool for eyeballing widgets across backends.
 *
 * Usage:
 *   widget_demo --widget label --backend tui    # ANSI alt-screen
 *   widget_demo --widget label --backend gui    # portable GUI (cocoa/x11)
 *   widget_demo --widget label --backend cocoa  # macOS Cocoa window
 *   widget_demo --widget label --backend x11    # Linux/Unix X11 window
 *   widget_demo -w label -b tui                 # short forms
 *
 * Press Ctrl-C to exit.
 * Tab cycles focus, Space/Enter activates widgets.
 *
 * Optional debug log:
 *   --debug-log <path> or N00B_WIDGET_DEMO_LOG=<path>
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
#include "display/render/backend_registry.h"
#include "display/render/box.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/label.h"
#include "display/widgets/divider.h"
#include "display/widgets/spacer.h"
#include "display/widgets/progress.h"
#include "display/widgets/button.h"
#include "display/widgets/checkbox.h"
#include "display/widgets/input.h"
#include "display/widgets/box.h"
#include "display/widgets/grid.h"
#include "display/widgets/split.h"
#include "display/widgets/zstack.h"
#include "display/widgets/switch.h"
#include "display/widgets/radio.h"
#include "display/widgets/link.h"
#include "display/widgets/list_widget.h"
#include "display/widgets/selectionlist.h"
#include "display/widgets/breadcrumb.h"
#include "display/event.h"
#include "display/event_loop.h"
#include "display/focus.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/string_ops.h"
#include "text/strings/theme.h"
#include "internal/display/widget_primitives.h"
#if defined(__APPLE__)
#include "display/render/backend_cocoa.h"
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
    dbg("  [%s] plane=%p  width=%u height=%u  "
        "x=%d y=%d z=%d  flags=0x%04x  vtable=%p  children.len=%zu\n",
        label, (void *)p,
        p->width, p->height,
        p->x, p->y, p->z,
        p->flags, (void *)p->widget_vtable,
        p->children.data ? p->children.len : 0);
}

static void
dbg_canvas(n00b_canvas_t *c)
{
    dbg("Canvas: vtable=%p  backend_ctx=%p  caps=0x%08x\n",
        (void *)c->vtable, c->backend_ctx, (unsigned)c->caps);
    dbg("  frame_rows=%u  frame_cols=%u\n",
        c->frame_rows, c->frame_cols);
    dbg("  planes.data=%p  planes.len=%zu  needs_full_redraw=%d  size_set=%d\n",
        (void *)c->planes.data, c->planes.len,
        c->needs_full_redraw, c->size_set);
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

static void
plane_outer_size_px(const n00b_plane_t *plane,
                    int32_t             cell_px_w,
                    int32_t             cell_px_h,
                    int32_t            *out_w,
                    int32_t            *out_h)
{
    int32_t width = plane ? plane->width : 0;
    int32_t height = plane ? plane->height : 0;

    if (plane && plane->box) {
        int32_t inset_top = 0;
        int32_t inset_bottom = 0;
        int32_t inset_left = 0;
        int32_t inset_right = 0;
        n00b_box_insets_px(plane->box,
                           cell_px_w,
                           cell_px_h,
                           &inset_top,
                           &inset_bottom,
                           &inset_left,
                           &inset_right);
        width += inset_left + inset_right;
        height += inset_top + inset_bottom;
    }

    if (out_w) {
        *out_w = width;
    }
    if (out_h) {
        *out_h = height;
    }
}

static n00b_plane_t *
make_demo_card(n00b_canvas_t               *canvas,
               int32_t                      gap_px,
               const n00b_border_theme_t   *theme,
               uint32_t                     border_rgb,
               uint32_t                     fill_rgb)
{
    n00b_text_style_t *fill_style = n00b_alloc(n00b_text_style_t);
    fill_style->bg_rgb = n00b_color_make(fill_rgb);

    n00b_plane_t *card = n00b_box_new(.canvas    = canvas,
                                      .direction = N00B_FLEX_COLUMN,
                                      .gap       = gap_px);
    card->box = n00b_box_props_new(
        .theme        = theme,
        .border_style = make_style(N00B_TRI_YES, N00B_TRI_NO, border_rgb),
        .fill_style   = fill_style,
        .pad_top      = 0,
        .pad_bottom   = 0,
        .pad_left     = 1,
        .pad_right    = 1);

    return card;
}

// ====================================================================
// Label demo
// ====================================================================

static void
demo_label(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_label start ===\n");

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t frame_w = (int32_t)canvas->frame_cols;
    int32_t frame_h = (int32_t)canvas->frame_rows;
    int32_t label_w = frame_w - 2 * cpw;

    dbg("frame: %dx%d px, cpw=%d cph=%d, label_w: %d\n",
        frame_w, frame_h, cpw, cph, label_w);

    // Create root plane first, add to canvas so children inherit it.
    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width  = frame_w;
    root->height = frame_h;
    n00b_canvas_add_plane(canvas, root);

    // --- Title label (bold, centered, cyan) ---
    n00b_text_style_t *title_style = make_style(N00B_TRI_YES,
                                                  N00B_TRI_NO,
                                                  0x00CED1);
    n00b_string_t *title_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Label Widget Demo"), title_style);

    n00b_plane_t *title = n00b_label_new(title_text,
                                          .canvas    = canvas,
                                          .width     = label_w,
                                          .height    = cph,
                                          .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, title, 1 * cpw, 0 * cph);
    dbg_plane("title", title);

    // --- Left-aligned label (green) ---
    n00b_text_style_t *left_style = make_style(N00B_TRI_NO,
                                                 N00B_TRI_NO,
                                                 0x00FF00);
    n00b_string_t *left_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Left-aligned (green)"), left_style);

    n00b_plane_t *left_lbl = n00b_label_new(left_text,
                                              .canvas    = canvas,
                                              .width     = label_w,
                                              .height    = cph,
                                              .alignment = N00B_ALIGN_LEFT);
    n00b_plane_add_child(root, left_lbl, 1 * cpw, 2 * cph);
    dbg_plane("left", left_lbl);

    // --- Center-aligned label (yellow, bold) ---
    n00b_text_style_t *center_style = make_style(N00B_TRI_YES,
                                                   N00B_TRI_NO,
                                                   0xFFFF00);
    n00b_string_t *center_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Center-aligned (bold yellow)"), center_style);

    n00b_plane_t *center_lbl = n00b_label_new(center_text,
                                                .canvas    = canvas,
                                                .width     = label_w,
                                                .height    = cph,
                                                .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, center_lbl, 1 * cpw, 4 * cph);
    dbg_plane("center", center_lbl);

    // --- Right-aligned label (magenta, italic) ---
    n00b_text_style_t *right_style = make_style(N00B_TRI_NO,
                                                  N00B_TRI_YES,
                                                  0xFF00FF);
    n00b_string_t *right_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Right-aligned (italic magenta)"), right_style);

    n00b_plane_t *right_lbl = n00b_label_new(right_text,
                                               .canvas    = canvas,
                                               .width     = label_w,
                                               .height    = cph,
                                               .alignment = N00B_ALIGN_RIGHT);
    n00b_plane_add_child(root, right_lbl, 1 * cpw, 6 * cph);
    dbg_plane("right", right_lbl);

    // --- Plain unstyled label ---
    n00b_plane_t *plain_lbl = n00b_label_new(
        n00b_string_from_cstr("Plain unstyled text"),
        .canvas = canvas,
        .width  = label_w,
        .height = cph);
    n00b_plane_add_child(root, plain_lbl, 1 * cpw, 8 * cph);
    dbg_plane("plain", plain_lbl);

    // --- Wrapped label (tomato red) ---
    n00b_text_style_t *wrap_style = make_style(N00B_TRI_NO,
                                                 N00B_TRI_NO,
                                                 0xFF6347);
    n00b_string_t *wrap_text = n00b_str_set_base_style(
        n00b_string_from_cstr(
            "This is a longer line of text that is designed to wrap across multiple rows "
            "when rendered in the terminal. It demonstrates word-wrap behavior in the label "
            "widget, flowing naturally across line boundaries without breaking mid-word."),
        wrap_style);

    n00b_plane_t *wrap_lbl = n00b_label_new(wrap_text,
                                              .canvas = canvas,
                                              .width  = label_w,
                                              .height = 5 * cph,
                                              .wrap   = true);
    n00b_plane_add_child(root, wrap_lbl, 1 * cpw, 10 * cph);
    dbg_plane("wrap", wrap_lbl);

    dbg("Canvas after setup:\n");
    dbg_canvas(canvas);
    dbg("=== demo_label end ===\n\n");
}

// ====================================================================
// ZStack demo
// ====================================================================

static n00b_plane_t *g_zstack_root = nullptr;
static n00b_plane_t *g_zstack_overlay = nullptr;
static n00b_plane_t *g_zstack_background_status = nullptr;
static n00b_plane_t *g_zstack_overlay_status = nullptr;
static n00b_plane_t *g_zstack_layer_status = nullptr;
static n00b_plane_t *g_grid_status_label = nullptr;
static n00b_plane_t *g_split_status_label = nullptr;

static void
set_demo_label_text(n00b_plane_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }

    n00b_label_set_text(label, n00b_string_from_cstr(text));
}

static void
set_zstack_layer_status(const char *text)
{
    set_demo_label_text(g_zstack_layer_status, text);
}

static void
set_grid_status(const char *text)
{
    set_demo_label_text(g_grid_status_label, text);
}

static void
set_split_status(const char *text)
{
    set_demo_label_text(g_split_status_label, text);
}

static void
on_zstack_background_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    set_demo_label_text(g_zstack_background_status,
                        "Background button fired.");
}

static void
on_zstack_overlay_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    set_demo_label_text(g_zstack_overlay_status,
                        "Overlay button captured the click.");
}

static void
on_zstack_bring_to_front_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    if (!g_zstack_root || !g_zstack_overlay) {
        return;
    }

    (void)n00b_zstack_bring_to_front(g_zstack_root, g_zstack_overlay);
    set_zstack_layer_status("Overlay layer order: front.");
}

static void
on_zstack_send_to_back_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    if (!g_zstack_root || !g_zstack_overlay) {
        return;
    }

    (void)n00b_zstack_send_to_back(g_zstack_root, g_zstack_overlay);
    set_zstack_layer_status("Overlay layer order: back.");
}

static void
on_grid_filters_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    set_grid_status("Left card action fired: filters toggled.");
}

static void
on_grid_refresh_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    set_grid_status("Wide card action fired: preview refreshed.");
}

static void
on_split_navigator_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    set_split_status("Navigator action fired from the left pane.");
}

static void
on_split_inspector_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;

    set_split_status("Inspector action fired from the right pane.");
}

static void
on_split_ratio_change(n00b_plane_t *plane, float ratio, void *data)
{
    char msg[96];

    (void)plane;
    (void)data;

    snprintf(msg, sizeof(msg), "Divider ratio: %.2f", ratio);
    set_split_status(msg);
}

static void
demo_zstack(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_zstack start ===\n");

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t frame_w = (int32_t)canvas->frame_cols;
    int32_t frame_h = (int32_t)canvas->frame_rows;
    int32_t gap_px = 2 * cpw;

    if (gap_px < 1) {
        gap_px = 1;
    }

    n00b_text_style_t *title_style = make_style(N00B_TRI_YES,
                                                N00B_TRI_NO,
                                                0x0B7285);
    n00b_text_style_t *note_style = make_style(N00B_TRI_NO,
                                               N00B_TRI_NO,
                                               0x5C6770);
    n00b_text_style_t *card_title_style = make_style(N00B_TRI_YES,
                                                     N00B_TRI_NO,
                                                     0x8C4A00);
    n00b_text_style_t *bg_fill_style = n00b_alloc(n00b_text_style_t);
    bg_fill_style->bg_rgb = n00b_color_make(0xF2E8D8);
    n00b_text_style_t *controls_fill_style = n00b_alloc(n00b_text_style_t);
    controls_fill_style->bg_rgb = n00b_color_make(0xE7F1F3);

    n00b_plane_t *root = n00b_box_new(.canvas    = canvas,
                                      .direction = N00B_FLEX_ROW,
                                      .gap       = gap_px);
    root->width  = frame_w;
    root->height = frame_h;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *scene = n00b_zstack_new(.canvas = canvas);
    scene->flex.grow   = 1.0f;
    scene->flex.shrink = 1.0f;
    scene->flex.basis  = 1;
    g_zstack_root      = scene;
    n00b_plane_add_child(root, scene, 0, 0);

    n00b_box_props_t *controls_box = n00b_box_props_new(
        .theme        = &n00b_border_double,
        .border_style = make_style(N00B_TRI_YES, N00B_TRI_NO, 0x0B7285),
        .fill_style   = controls_fill_style,
        .pad_top      = 1,
        .pad_bottom   = 1,
        .pad_left     = 2,
        .pad_right    = 2);
    n00b_plane_t *controls = n00b_new_kargs(n00b_plane_t, plane,
                                            .box    = controls_box,
                                            .canvas = canvas);
    controls->width  = frame_w > 72 * cpw ? 26 * cpw : n00b_max(20 * cpw, frame_w / 4);
    controls->height = frame_h;
    controls->flex.basis  = 0;
    controls->flex.grow   = 0.0f;
    controls->flex.shrink = 0.0f;

    int32_t controls_outer_w = 0;
    plane_outer_size_px(controls, cpw, cph, &controls_outer_w, nullptr);
    controls->flex.basis = controls_outer_w;
    n00b_plane_add_child(root, controls, 0, 0);

    n00b_widget_layout(root,
                       (n00b_rect_t){
                           .x      = 0,
                           .y      = 0,
                           .width  = frame_w,
                           .height = frame_h,
                       });

    n00b_plane_t *background = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *overlay    = n00b_new_kargs(n00b_plane_t, plane);
    g_zstack_overlay         = overlay;

    int32_t controls_inset_top = 0;
    int32_t controls_inset_left = 0;
    n00b_box_insets_px(controls->box,
                       cpw,
                       cph,
                       &controls_inset_top,
                       nullptr,
                       &controls_inset_left,
                       nullptr);

    n00b_plane_t *controls_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Layer Controls"), title_style),
        .canvas    = canvas,
        .width     = controls->width,
        .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(controls,
                         controls_title,
                         controls_inset_left,
                         controls_inset_top);

    g_zstack_layer_status = n00b_label_new(
        n00b_string_from_cstr("Overlay layer order: front."),
        .canvas = canvas,
        .width  = controls->width,
        .wrap   = true,
        .height = 2 * cph);
    n00b_plane_add_child(controls,
                         g_zstack_layer_status,
                         controls_inset_left,
                         controls_inset_top + cph);

    n00b_plane_t *bring_front_button = n00b_button_new(
        n00b_string_from_cstr("Bring Overlay To Front"),
        .canvas   = canvas,
        .on_click = on_zstack_bring_to_front_click);
    int32_t bring_front_h = 0;
    plane_outer_size_px(bring_front_button, cpw, cph, nullptr, &bring_front_h);
    n00b_plane_add_child(controls,
                         bring_front_button,
                         controls_inset_left,
                         controls_inset_top + 3 * cph);

    n00b_plane_t *send_back_button = n00b_button_new(
        n00b_string_from_cstr("Send Overlay To Back"),
        .canvas   = canvas,
        .on_click = on_zstack_send_to_back_click);
    n00b_plane_add_child(controls,
                         send_back_button,
                         controls_inset_left,
                         controls_inset_top + 3 * cph + bring_front_h + cph);

    n00b_plane_fill_rect(controls,
                         0,
                         0,
                         controls->width,
                         controls->height,
                         .style = controls_fill_style);
    int32_t text_w = scene->width - 4 * cpw;
    if (text_w < 1) {
        text_w = 1;
    }

    n00b_plane_t *bg_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("ZStack Demo"), title_style),
        .canvas = canvas,
        .width  = text_w);
    n00b_plane_add_child(background, bg_title, 2 * cpw, 1 * cph);

    n00b_plane_t *bg_note = n00b_label_new(
        n00b_str_set_base_style(
            n00b_string_from_cstr(
                "Use the control panel to move the centered overlay card in front of or behind the background button."),
            note_style),
        .canvas = canvas,
        .width  = text_w,
        .wrap   = true,
        .height = 3 * cph);
    n00b_plane_add_child(background, bg_note, 2 * cpw, 3 * cph);

    g_zstack_background_status = n00b_label_new(
        n00b_string_from_cstr("Background status: waiting."),
        .canvas = canvas,
        .width  = text_w);
    n00b_plane_add_child(background,
                         g_zstack_background_status,
                         2 * cpw,
                         scene->height - 3 * cph);

    n00b_box_props_t *card_box = n00b_box_props_new(
        .theme        = &n00b_border_double,
        .border_style = make_style(N00B_TRI_YES, N00B_TRI_NO, 0x8C4A00),
        .fill_style   = bg_fill_style,
        .pad_top      = 1,
        .pad_bottom   = 1,
        .pad_left     = 2,
        .pad_right    = 2);
    n00b_plane_t *card = n00b_new_kargs(n00b_plane_t, plane,
                                         .box    = card_box,
                                         .canvas = canvas);

    card->width  = scene->width > 40 * cpw ? 30 * cpw : n00b_max(14 * cpw, scene->width - 10 * cpw);
    card->height = scene->height > 14 * cph ? 6 * cph : n00b_max(4 * cph, scene->height - 8 * cph);
    n00b_plane_fill_rect(card, 0, 0, card->width, card->height, .style = bg_fill_style);

    int32_t card_outer_w = 0;
    int32_t card_outer_h = 0;
    plane_outer_size_px(card, cpw, cph, &card_outer_w, &card_outer_h);

    int32_t card_x = (scene->width - card_outer_w) / 2;
    int32_t card_y = (scene->height - card_outer_h) / 2;
    if (card_x < cpw) {
        card_x = cpw;
    }
    if (card_y < cph) {
        card_y = cph;
    }

    int32_t inset_top = 0;
    int32_t inset_left = 0;
    n00b_box_insets_px(card->box,
                       cpw,
                       cph,
                       &inset_top,
                       nullptr,
                       &inset_left,
                       nullptr);

    n00b_plane_t *card_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Overlay Card"), card_title_style),
        .canvas    = canvas,
        .width     = card->width,
        .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(card, card_title, inset_left, inset_top);

    g_zstack_overlay_status = n00b_label_new(
        n00b_string_from_cstr("Overlay status: click the top button."),
        .canvas = canvas,
        .width  = card->width,
        .wrap   = true,
        .height = 2 * cph);
    n00b_plane_add_child(card,
                         g_zstack_overlay_status,
                         inset_left,
                         inset_top + 2 * cph);

    n00b_plane_t *overlay_button = n00b_button_new(
        n00b_string_from_cstr("Overlay Button"),
        .canvas   = canvas,
        .on_click = on_zstack_overlay_click);
    int32_t overlay_btn_w = 0;
    int32_t overlay_btn_h = 0;
    plane_outer_size_px(overlay_button, cpw, cph, &overlay_btn_w, &overlay_btn_h);
    int32_t overlay_btn_x = inset_left + (card->width - overlay_btn_w) / 2;
    int32_t overlay_btn_y = inset_top + card->height - overlay_btn_h;
    if (overlay_btn_x < inset_left) {
        overlay_btn_x = inset_left;
    }
    if (overlay_btn_y < inset_top) {
        overlay_btn_y = inset_top;
    }
    n00b_plane_add_child(card, overlay_button, overlay_btn_x, overlay_btn_y);

    n00b_plane_add_child(overlay, card, card_x, card_y);

    n00b_plane_t *background_button = n00b_button_new(
        n00b_string_from_cstr("Background Button"),
        .canvas   = canvas,
        .on_click = on_zstack_background_click);
    n00b_plane_add_child(background,
                         background_button,
                         card_x + overlay_btn_x,
                         card_y + overlay_btn_y);

    n00b_zstack_push(scene, background);
    n00b_zstack_push(scene, overlay);
    n00b_widget_layout(scene,
                       (n00b_rect_t){
                           .x      = scene->bounds.x,
                           .y      = scene->bounds.y,
                           .width  = scene->bounds.width,
                           .height = scene->bounds.height,
                       });

    n00b_plane_fill_rect(background,
                         0,
                         0,
                         background->width,
                         background->height,
                         .style = bg_fill_style);

    dbg_plane("zstack-root", root);
    dbg_plane("zstack-scene", scene);
    dbg_plane("zstack-background", background);
    dbg_plane("zstack-overlay", overlay);
    dbg_plane("zstack-controls", controls);
    dbg("=== demo_zstack end ===\n\n");
}

// ====================================================================
// Grid demo
// ====================================================================

static void
demo_grid(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_grid start ===\n");

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t frame_w = (int32_t)canvas->frame_cols;
    int32_t frame_h = (int32_t)canvas->frame_rows;
    int32_t gap_px = 2 * cpw;
    int32_t card_gap_px = cph > 0 ? cph : 1;

    if (gap_px < 1) {
        gap_px = 1;
    }

    n00b_text_style_t *title_style = make_style(N00B_TRI_YES,
                                                N00B_TRI_NO,
                                                0x0F4C5C);
    n00b_text_style_t *note_style = make_style(N00B_TRI_NO,
                                               N00B_TRI_NO,
                                               0x5A646A);

    n00b_plane_t *root = n00b_grid_new(.canvas     = canvas,
                                       .columns    = 3,
                                       .gap        = gap_px,
                                       .pad_top    = 2 * cph,
                                       .pad_right  = 2 * cpw,
                                       .pad_bottom = 2 * cph,
                                       .pad_left   = 2 * cpw);
    root->width  = frame_w;
    root->height = frame_h;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *header_card = make_demo_card(canvas,
                                               card_gap_px,
                                               &n00b_border_double,
                                               0x0F4C5C,
                                               0xE7F4F1);
    n00b_plane_t *left_card = make_demo_card(canvas,
                                             card_gap_px,
                                             &n00b_border_plain,
                                             0x8C4A00,
                                             0xF4E8D6);
    n00b_plane_t *details_card = make_demo_card(canvas,
                                                card_gap_px,
                                                &n00b_border_double,
                                                0x7A1F36,
                                                0xF9E7EC);
    n00b_plane_t *status_card = make_demo_card(canvas,
                                               card_gap_px,
                                               &n00b_border_plain,
                                               0x495057,
                                               0xEEF2F5);

    n00b_plane_add_child(root, header_card, 0, 0);
    n00b_plane_add_child(root, left_card, 0, 0);
    n00b_plane_add_child(root, details_card, 0, 0);
    n00b_plane_add_child(root, status_card, 0, 0);
    n00b_grid_set_span(root, header_card, 3, 1);
    n00b_grid_set_span(root, details_card, 2, 1);
    n00b_grid_set_span(root, status_card, 3, 1);

    n00b_plane_t *header_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Grid Widget Demo"), title_style),
        .canvas = canvas);
    n00b_plane_add_child(header_card, header_title, 0, 0);

    n00b_plane_t *left_button = n00b_button_new(
        n00b_string_from_cstr("Toggle Filters"),
        .canvas   = canvas,
        .on_click = on_grid_filters_click);
    n00b_plane_add_child(left_card, left_button, 0, 0);

    n00b_plane_t *refresh_button = n00b_button_new(
        n00b_string_from_cstr("Refresh Preview"),
        .canvas   = canvas,
        .on_click = on_grid_refresh_click);
    n00b_plane_add_child(details_card, refresh_button, 0, 0);

    g_grid_status_label = n00b_label_new(
        n00b_str_set_base_style(
            n00b_string_from_cstr("Ready. Tab or click a card action."),
            note_style),
        .canvas = canvas);
    n00b_plane_add_child(status_card, g_grid_status_label, 0, 0);

    dbg_plane("grid-root", root);
    dbg_plane("grid-header", header_card);
    dbg_plane("grid-left", left_card);
    dbg_plane("grid-details", details_card);
    dbg_plane("grid-status", status_card);
    dbg("=== demo_grid end ===\n\n");
}

// ====================================================================
// Split demo
// ====================================================================

static void
demo_split(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_split start ===\n");

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t frame_w = (int32_t)canvas->frame_cols;
    int32_t frame_h = (int32_t)canvas->frame_rows;
    int32_t content_w = frame_w - 4 * cpw;
    int32_t gap_px = cph > 0 ? cph : 1;
    int32_t divider_px = cpw > 0 ? cpw : 1;

    if (content_w < 1) {
        content_w = 1;
    }

    n00b_text_style_t *title_style = make_style(N00B_TRI_YES,
                                                N00B_TRI_NO,
                                                0x7A3E00);
    n00b_text_style_t *note_style = make_style(N00B_TRI_NO,
                                               N00B_TRI_NO,
                                               0x5F6B73);
    n00b_text_style_t *left_title_style = make_style(N00B_TRI_YES,
                                                     N00B_TRI_NO,
                                                     0x0B7285);
    n00b_text_style_t *right_title_style = make_style(N00B_TRI_YES,
                                                      N00B_TRI_NO,
                                                      0x8C2F39);

    n00b_plane_t *root = n00b_box_new(.canvas     = canvas,
                                      .direction  = N00B_FLEX_COLUMN,
                                      .gap        = gap_px,
                                      .pad_top    = 2 * cph,
                                      .pad_right  = 2 * cpw,
                                      .pad_bottom = 2 * cph,
                                      .pad_left   = 2 * cpw);
    root->width  = frame_w;
    root->height = frame_h;
    n00b_canvas_add_plane(canvas, root);

    n00b_plane_t *title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Split Widget Demo"), title_style),
        .canvas = canvas,
        .width = content_w);
    n00b_plane_add_child(root, title, 0, 0);

    n00b_plane_t *instructions = n00b_label_new(
        n00b_str_set_base_style(
            n00b_string_from_cstr(
                "Drag the divider to resize both panes live, then click either pane button to verify child widgets still receive input."),
            note_style),
        .canvas = canvas,
        .width = content_w,
        .wrap = true,
        .height = 3 * cph);
    n00b_plane_add_child(root, instructions, 0, 0);

    g_split_status_label = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Divider ratio: 0.38"), note_style),
        .canvas = canvas,
        .width = content_w);
    n00b_plane_add_child(root, g_split_status_label, 0, 0);

    n00b_plane_t *left_card = make_demo_card(canvas,
                                             gap_px,
                                             &n00b_border_double,
                                             0x0B7285,
                                             0xE6F4F1);
    n00b_plane_t *right_card = make_demo_card(canvas,
                                              gap_px,
                                              &n00b_border_plain,
                                              0x8C2F39,
                                              0xF9E7EA);

    n00b_plane_t *left_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Navigator"), left_title_style),
        .canvas = canvas);
    n00b_plane_t *left_note = n00b_label_new(
        n00b_str_set_base_style(
            n00b_string_from_cstr(
                "A compact list-style pane that behaves like a sidebar or project navigator."),
            note_style),
        .canvas = canvas,
        .wrap = true,
        .height = 3 * cph);
    n00b_plane_t *left_button = n00b_button_new(
        n00b_string_from_cstr("Open Item"),
        .canvas   = canvas,
        .on_click = on_split_navigator_click);
    n00b_plane_add_child(left_card, left_title, 0, 0);
    n00b_plane_add_child(left_card, left_note, 0, 0);
    n00b_plane_add_child(left_card, left_button, 0, 0);

    n00b_plane_t *right_title = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Inspector"), right_title_style),
        .canvas = canvas);
    n00b_plane_t *right_note = n00b_label_new(
        n00b_str_set_base_style(
            n00b_string_from_cstr(
                "A detail pane sized for longer summaries, properties, or editor metadata."),
            note_style),
        .canvas = canvas,
        .wrap = true,
        .height = 3 * cph);
    n00b_plane_t *right_button = n00b_button_new(
        n00b_string_from_cstr("Apply Change"),
        .canvas   = canvas,
        .on_click = on_split_inspector_click);
    n00b_plane_add_child(right_card, right_title, 0, 0);
    n00b_plane_add_child(right_card, right_note, 0, 0);
    n00b_plane_add_child(right_card, right_button, 0, 0);

    n00b_plane_t *split = n00b_split_new(left_card,
                                         right_card,
                                         .canvas = canvas,
                                         .ratio = 0.38f,
                                         .min_first_px = 16 * divider_px,
                                         .min_second_px = 18 * divider_px,
                                         .divider_px = divider_px,
                                         .on_change = on_split_ratio_change);
    split->flex.grow   = 1.0f;
    split->flex.shrink = 1.0f;
    split->flex.basis  = 1;
    n00b_plane_add_child(root, split, 0, 0);

    dbg_plane("split-root", root);
    dbg_plane("split-widget", split);
    dbg_plane("split-left-card", left_card);
    dbg_plane("split-right-card", right_card);
    dbg("=== demo_split end ===\n\n");
}

// ====================================================================
// All-widgets interactive demo
// ====================================================================

// State shared between widgets in the all-widgets demo.
static n00b_plane_t  *g_root_plane    = nullptr;
static n00b_plane_t  *g_status_label  = nullptr;
static n00b_plane_t  *g_progress_bar  = nullptr;
static n00b_plane_t  *g_font_demo_label = nullptr;
static n00b_text_style_t *g_font_demo_style = nullptr;
static n00b_text_style_t *g_status_style = nullptr;
static bool           g_auto_progress = false;

#define FONT_DEMO_SIZE 32

static void
refresh_font_demo_label(void)
{
    if (!g_font_demo_label || !g_font_demo_style) {
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Font size preview (%d px)", FONT_DEMO_SIZE);
    n00b_string_t *text = n00b_str_set_base_style(
        n00b_string_from_cstr(msg),
        g_font_demo_style);
    n00b_label_set_text(g_font_demo_label, text);
}

static void
set_status_text(n00b_string_t *msg)
{
    if (!g_status_label || !msg) {
        return;
    }

    if (g_status_style) {
        msg = n00b_str_set_base_style(msg, g_status_style);
    }

    // Avoid immediate callback-path render. Update model state and let the
    // event loop's normal rerender pass draw once with consistent metrics.
    n00b_label_t *label =
        n00b_widget_data_if_kind(g_status_label, &n00b_widget_label);
    if (!label) {
        return;
    }
    label->text = msg;
    n00b_plane_mark_dirty(g_status_label);
}

static void
on_button_click(n00b_plane_t *plane, void *data)
{
    (void)data;

    if (!plane
        || plane->widget_vtable != &n00b_widget_button
        || !plane->widget_data) {
        return;
    }

    n00b_button_t *btn = (n00b_button_t *)plane->widget_data;
    btn->label         = n00b_string_from_cstr("Clicked!");
    n00b_plane_mark_dirty(plane);
    n00b_widget_render(plane);
}

static void
on_checkbox_change(n00b_plane_t *plane, bool checked, void *data)
{
    (void)plane;
    (void)data;
    g_auto_progress = checked;
    set_status_text(n00b_string_from_cstr(
        checked ? "Auto-progress enabled" : "Auto-progress disabled"));
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
        set_status_text(msg);
    }
}

static void
on_switch_change(n00b_plane_t *plane, bool on, void *data)
{
    (void)plane;
    (void)data;
    set_status_text(n00b_string_from_cstr(on ? "Switch: ON" : "Switch: OFF"));
}

static void
on_radio_change(n00b_plane_t *plane, int selected, void *data)
{
    (void)plane;
    (void)data;
    const char *names[] = {"Red", "Green", "Blue"};
    if (selected >= 0 && selected < 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Radio: %s selected", names[selected]);
        n00b_string_t *status = n00b_string_from_cstr(msg);
        set_status_text(status);
    }
}

static void
on_link_click(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    set_status_text(n00b_string_from_cstr("Link clicked!"));
}

static void
on_list_select(n00b_plane_t *plane, int index, void *data)
{
    (void)plane;
    (void)data;
    char msg[64];
    snprintf(msg, sizeof(msg), "List: item %d activated", index);
    n00b_string_t *status = n00b_string_from_cstr(msg);
    set_status_text(status);
}

static void
on_sellist_change(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    int count = n00b_selectionlist_selected_count(plane);
    char msg[64];
    snprintf(msg, sizeof(msg), "Selection: %d items selected", count);
    n00b_string_t *status = n00b_string_from_cstr(msg);
    set_status_text(status);
}

static void
on_breadcrumb_click(n00b_plane_t *plane, n00b_isize_t index, void *data)
{
    (void)plane;
    (void)data;
    char msg[64];
    snprintf(msg, sizeof(msg), "Breadcrumb: segment %zu clicked",
             (size_t)index);
    n00b_string_t *status = n00b_string_from_cstr(msg);
    set_status_text(status);
}

static void
demo_all(n00b_canvas_t *canvas)
{
    dbg("\n=== demo_all start ===\n");

    int32_t cpw = (int32_t)canvas->cell_px_w;
    int32_t cph = (int32_t)canvas->cell_px_h;
    int32_t frame_w = (int32_t)canvas->frame_cols;
    int32_t frame_h = (int32_t)canvas->frame_rows;
    int32_t content_w = frame_w - 2 * cpw;

    // Create root and add to canvas first so children inherit metrics.
    n00b_plane_t *root = n00b_box_new(.canvas    = canvas,
                                        .direction = N00B_FLEX_COLUMN,
                                        .gap       = cph);
    root->width  = frame_w;
    root->height = frame_h;
    g_root_plane = root;
    n00b_canvas_add_plane(canvas, root);

    // Title label.
    n00b_text_style_t *title_style = make_style(N00B_TRI_YES, N00B_TRI_NO, 0x00CED1);
    n00b_string_t *title_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Widget Demo - All Widgets"), title_style);
    n00b_plane_t *title = n00b_label_new(title_text,
                                           .canvas    = canvas,
                                           .width     = content_w,
                                           .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, title, 0, 0);

    // Horizontal divider.
    n00b_plane_t *div1 = n00b_divider_new(.canvas = canvas, .width = content_w);
    n00b_plane_add_child(root, div1, 0, 0);

    // Progress bar.
    g_progress_bar = n00b_progress_new(.canvas = canvas, .width = content_w, .value = 0.3);
    g_progress_bar->flex.shrink = 1.0f;
    n00b_plane_add_child(root, g_progress_bar, 0, 0);

    // Spacer.
    n00b_plane_t *sp = n00b_spacer_new(.canvas = canvas, .width = content_w, .height = cph);
    n00b_plane_add_child(root, sp, 0, 0);

    // Button.
    n00b_plane_t *btn = n00b_button_new(
        n00b_string_from_cstr("Click Me"),
        .canvas   = canvas,
        .on_click = on_button_click);
    n00b_plane_add_child(root, btn, 0, 0);

    // Checkboxes.
    n00b_plane_t *cb = n00b_checkbox_new(
        n00b_string_from_cstr("Auto-progress"),
        .canvas    = canvas,
        .on_change = on_checkbox_change);
    n00b_plane_add_child(root, cb, 0, 0);

    n00b_plane_t *cb_circle = n00b_checkbox_new(
        n00b_string_from_cstr("Circle style"),
        .canvas    = canvas,
        .indicator = N00B_CB_STYLE_CIRCLE);
    n00b_plane_add_child(root, cb_circle, 0, 0);

    n00b_plane_t *cb_square = n00b_checkbox_new(
        n00b_string_from_cstr("Square style"),
        .canvas    = canvas,
        .indicator = N00B_CB_STYLE_SQUARE);
    n00b_plane_add_child(root, cb_square, 0, 0);

    n00b_plane_t *cb_ascii = n00b_checkbox_new(
        n00b_string_from_cstr("ASCII style"),
        .canvas    = canvas,
        .indicator = N00B_CB_STYLE_ASCII);
    n00b_plane_add_child(root, cb_ascii, 0, 0);

    // Text input.
    n00b_string_t *ph = n00b_string_from_cstr("Type here...");
    n00b_plane_t *inp = n00b_input_new(.canvas      = canvas,
                                         .width       = content_w,
                                         .placeholder = ph,
                                         .on_submit   = on_input_submit);
    inp->flex.shrink = 1.0f;
    n00b_plane_add_child(root, inp, 0, 0);

    // Switch widget.
    n00b_plane_t *sw = n00b_switch_new(
        n00b_string_from_cstr("Dark mode"),
        .canvas    = canvas,
        .on_change = on_switch_change);
    n00b_plane_add_child(root, sw, 0, 0);

    // Radio group.
    n00b_radio_group_t *rg = n00b_radio_group_new();
    n00b_radio_group_on_change(rg, on_radio_change, nullptr);

    n00b_text_style_t *red_style = n00b_alloc(n00b_text_style_t);
    red_style->fg_rgb    = n00b_color_make(0xFF0000);
    red_style->font_size = 28;
    red_style->font_hint = N00B_FONT_SANS;

    n00b_text_style_t *green_style = n00b_alloc(n00b_text_style_t);
    green_style->fg_rgb    = n00b_color_make(0x00AA00);
    green_style->font_size = 28;
    green_style->font_hint = N00B_FONT_SANS;

    n00b_text_style_t *blue_style = n00b_alloc(n00b_text_style_t);
    blue_style->fg_rgb    = n00b_color_make(0x0066FF);
    blue_style->font_size = 28;
    blue_style->font_hint = N00B_FONT_SANS;

    n00b_plane_t *r1 = n00b_radio_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Red"), red_style),
        .canvas = canvas, .group = rg, .height = 2 * cph);
    n00b_plane_add_child(root, r1, 0, 0);

    n00b_plane_t *r2 = n00b_radio_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Green"), green_style),
        .canvas = canvas, .group = rg, .height = 2 * cph);
    n00b_plane_add_child(root, r2, 0, 0);

    n00b_plane_t *r3 = n00b_radio_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Blue"), blue_style),
        .canvas = canvas, .group = rg, .height = 2 * cph);
    n00b_plane_add_child(root, r3, 0, 0);

    // Font size preview with a fixed larger size (for notcurses pixel mode).
    n00b_plane_t *font_div = n00b_divider_new(.canvas = canvas,
                                                .width  = content_w,
                                                .label  = n00b_string_from_cstr("Font Size"));
    n00b_plane_add_child(root, font_div, 0, 0);

    g_font_demo_style = make_style(N00B_TRI_YES, N00B_TRI_NO, 0xFFD166);
    g_font_demo_style->font_hint = N00B_FONT_SANS;
    g_font_demo_style->font_size = FONT_DEMO_SIZE;

    g_font_demo_label = n00b_label_new(
        n00b_str_set_base_style(n00b_string_from_cstr("Font size preview"), g_font_demo_style),
        .canvas    = canvas,
        .width     = content_w,
        .alignment = N00B_ALIGN_CENTER);
    n00b_plane_add_child(root, g_font_demo_label, 0, 0);
    refresh_font_demo_label();

    // Link widget.
    n00b_plane_t *lk = n00b_link_new(
        n00b_string_from_cstr("n00b documentation"),
        .canvas   = canvas,
        .on_click = on_link_click);
    n00b_plane_add_child(root, lk, 0, 0);

    // List widget.
    n00b_string_t *list_items[] = {
        n00b_string_from_cstr("Alpha"),
        n00b_string_from_cstr("Beta"),
        n00b_string_from_cstr("Gamma"),
        n00b_string_from_cstr("Delta"),
        n00b_string_from_cstr("Epsilon"),
    };
    n00b_plane_t *lst = n00b_list_widget_new(list_items, 5,
                                               .canvas    = canvas,
                                               .height    = 4 * cph,
                                               .on_select = on_list_select);
    n00b_plane_add_child(root, lst, 0, 0);

    // Selection list.
    n00b_string_t *sel_labels[] = {
        n00b_string_from_cstr("Feature A"),
        n00b_string_from_cstr("Feature B"),
        n00b_string_from_cstr("Feature C"),
    };
    n00b_plane_t *slist = n00b_selectionlist_new(sel_labels, 3,
                                                   .canvas    = canvas,
                                                   .height    = 3 * cph,
                                                   .on_change = on_sellist_change);
    n00b_plane_add_child(root, slist, 0, 0);

    // Breadcrumb.
    n00b_plane_t *bcrumb = n00b_breadcrumb_new(
        .canvas   = canvas,
        .width    = content_w,
        .on_click = on_breadcrumb_click);
    n00b_breadcrumb_push(bcrumb, n00b_string_from_cstr("Home"), nullptr);
    n00b_breadcrumb_push(bcrumb, n00b_string_from_cstr("Products"), nullptr);
    n00b_breadcrumb_push(bcrumb, n00b_string_from_cstr("Electronics"), nullptr);
    n00b_breadcrumb_push(bcrumb, n00b_string_from_cstr("Current"), nullptr);
    n00b_plane_add_child(root, bcrumb, 0, 0);

    // Divider before status.
    n00b_plane_t *div2 = n00b_divider_new(.canvas = canvas,
                                            .width  = content_w,
                                            .label  = n00b_string_from_cstr("Status"));
    n00b_plane_add_child(root, div2, 0, 0);

    // Status label.
    n00b_text_style_t *status_style = make_style(N00B_TRI_NO, N00B_TRI_YES, 0xAAAAAA);
    n00b_string_t *status_text = n00b_str_set_base_style(
        n00b_string_from_cstr("Ready. Tab to navigate, Enter/Space to interact."),
        status_style);
    g_status_style = status_style;
    g_status_label = n00b_label_new(status_text, .canvas = canvas, .width = content_w);
    g_status_label->flex.grow = 1.0f;
    n00b_plane_add_child(root, g_status_label, 0, 0);

    dbg("=== demo_all end ===\n\n");
}

// ====================================================================
// Usage
// ====================================================================

static void
usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --widget <name> [--backend <auto|tui|gui|cocoa|x11|nc|stream|dumb>] [--theme <name>] [--debug-log <path>]\n"
            "\n"
            "Widgets:  label, grid, split, zstack, all\n"
            "Backends: auto (policy-driven), tui (ANSI alt-screen),\n"
            "          gui (portable alias),\n"
            "          cocoa (macOS native), x11 (Linux/Unix native),\n"
            "          nc / notcurses (pixel + cell-based terminal),\n"
            "          stream (buffer capture), dumb (no-op)\n"
            "          gui maps to cocoa on macOS, x11 on Linux/Unix\n"
            "\n"
            "Debug log: --debug-log <path> or N00B_WIDGET_DEMO_LOG=<path>\n"
            "\n"
            "Short flags: -w <widget> -b <backend> -t <theme>\n",
            prog);
    exit(1);
}

static bool
backend_request_used_fallback(const char *requested_backend,
                              const char *selected_backend,
                              bool        allow_fallback,
                              bool        allow_env_override)
{
    if (!selected_backend || !selected_backend[0]) {
        return false;
    }

    n00b_string_t *requested = n00b_string_from_cstr(
        (requested_backend && requested_backend[0]) ? requested_backend : "auto");
    n00b_list_t(n00b_string_t *) candidates =
        n00b_renderer_candidate_names(requested,
                                      .allow_fallback     = allow_fallback,
                                      .allow_env_override = allow_env_override);

    if (candidates.len == 0) {
        return false;
    }

    n00b_string_t *primary_candidate = n00b_list_get(candidates, 0);
    n00b_result_t(n00b_renderer_vtable_ptr_t) primary_resolved =
        n00b_renderer_resolve_exact(primary_candidate, .allow_dynamic_load = false);

    if (n00b_result_is_ok(primary_resolved)) {
        const n00b_renderer_vtable_t *vtable = n00b_result_get(primary_resolved);
        if (vtable && vtable->name) {
            return strcmp(vtable->name, selected_backend) != 0;
        }
    }

    return !n00b_unicode_str_eq(primary_candidate,
                                n00b_string_from_cstr(selected_backend),
                                .case_sensitive = false);
}

static bool
backend_uses_terminal_io(const char *selected_backend)
{
    if (!selected_backend) {
        return false;
    }

    return strcmp(selected_backend, "ansi") == 0
        || strcmp(selected_backend, "notcurses") == 0;
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    const char *widget_name  = nullptr;
    const char *backend_name = "auto";
    const char *theme_name   = nullptr;
    const char *debug_log_path = nullptr;
    const bool  allow_fallback = true;
    const bool  allow_dynamic_load = true;
    const bool  allow_env_override = true;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--widget") == 0 || strcmp(argv[i], "-w") == 0)
            && i + 1 < argc) {
            widget_name = argv[++i];
        }
        else if ((strcmp(argv[i], "--backend") == 0 || strcmp(argv[i], "-b") == 0)
                 && i + 1 < argc) {
            backend_name = argv[++i];
        }
        else if ((strcmp(argv[i], "--theme") == 0 || strcmp(argv[i], "-t") == 0)
                 && i + 1 < argc) {
            theme_name = argv[++i];
        }
        else if (strcmp(argv[i], "--debug-log") == 0 && i + 1 < argc) {
            debug_log_path = argv[++i];
        }
        else {
            usage(argv[0]);
        }
    }

    if (!widget_name) {
        usage(argv[0]);
    }

    if (!debug_log_path || !debug_log_path[0]) {
        const char *env_debug_path = getenv("N00B_WIDGET_DEMO_LOG");
        if (env_debug_path && env_debug_path[0]) {
            debug_log_path = env_debug_path;
        }
    }

    if (debug_log_path && debug_log_path[0]) {
        g_log = fopen(debug_log_path, "w");
        if (!g_log) {
            fprintf(stderr,
                    "Warning: could not open debug log '%s': %s\n",
                    debug_log_path,
                    strerror(errno));
        }
    }

    dbg("widget_demo started: widget=%s backend=%s\n", widget_name, backend_name);

    // Initialize runtime.
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);
    dbg("Runtime initialized.\n");

    // Apply theme override if requested.
    if (theme_name) {
        if (!n00b_theme_set_current(theme_name)) {
            fprintf(stderr, "Unknown theme: %s\n", theme_name);
            n00b_shutdown();
            return 1;
        }
        dbg("Theme set to: %s\n", theme_name);
    }

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
    n00b_canvas_init(canvas,
                     .backend_name           = n00b_string_from_cstr(backend_name),
                     .backend_allow_fallback = allow_fallback,
                     .backend_allow_dynamic_load = allow_dynamic_load,
                     .backend_allow_env_override = allow_env_override,
                     .output                 = stdout_topic);

    if (!canvas->backend_ctx) {
        fprintf(stderr,
                "Failed to initialize backend '%s' (or fallback candidates).\n",
                backend_name);
        n00b_canvas_destroy(canvas);
        n00b_shutdown();
        if (g_log) {
            fclose(g_log);
        }
        return 1;
    }

    const char *selected_backend = canvas->vtable && canvas->vtable->name
                                 ? canvas->vtable->name
                                 : "unknown";
    bool used_fallback = backend_request_used_fallback(backend_name,
                                                       selected_backend,
                                                       allow_fallback,
                                                       allow_env_override);
    bool uses_terminal_io = backend_uses_terminal_io(selected_backend);

    // Avoid writing directly to stderr once a backend owns terminal state
    // (notcurses). Out-of-band writes can desynchronize the managed TUI.
    if (!(canvas->caps & N00B_RCAP_MANAGES_TTY)) {
        fprintf(stderr, "Backend request '%s' selected '%s'%s\n",
                backend_name,
                selected_backend,
                used_fallback ? " (fallback)" : "");
    }

    dbg("Selected backend: requested='%s' selected='%s' fallback=%d\n",
        backend_name, selected_backend, (int)used_fallback);
    dbg("Selected vtable: name='%s' version=%u\n",
        selected_backend,
        canvas->vtable ? canvas->vtable->version : 0u);

    dbg("Canvas initialized:\n");
    dbg_canvas(canvas);

    // Dispatch to widget demo.
    bool use_event_loop = false;
    if (strcmp(widget_name, "label") == 0) {
        demo_label(canvas);
    }
    else if (strcmp(widget_name, "grid") == 0) {
        demo_grid(canvas);
        use_event_loop = true;
    }
    else if (strcmp(widget_name, "split") == 0) {
        demo_split(canvas);
        use_event_loop = true;
    }
    else if (strcmp(widget_name, "zstack") == 0) {
        demo_zstack(canvas);
        use_event_loop = true;
    }
    else if (strcmp(widget_name, "all") == 0) {
        demo_all(canvas);
        use_event_loop = true;
    }
    else {
        fprintf(stderr, "Unknown widget: %s\n", widget_name);
        n00b_canvas_destroy(canvas);
        n00b_shutdown();
        if (g_log) {
            fclose(g_log);
        }
        return 1;
    }

    if (use_event_loop && (!canvas->vtable || !canvas->vtable->poll_event)) {
        fprintf(stderr,
                "Backend '%s' has no input polling; using single-frame mode.\n",
                selected_backend);
        use_event_loop = false;
    }

    // Render.
    dbg("\n=== About to render ===\n");

    if (uses_terminal_io) {

        if (use_event_loop) {
            // Interactive event loop for "all" mode.
            // n00b_canvas_run manages raw mode, alt screen, and
            // signal-safe cleanup internally.
            dbg("Starting event loop...\n");
            n00b_canvas_run(canvas, .tick_ms = 50);
            dbg("Event loop exited.\n");
        }
        else {
            dbg("Entering alt screen...\n");
            n00b_canvas_alt_screen_enter(canvas);

            dbg("Calling canvas_render...\n");
            n00b_canvas_render(canvas);

            dbg("After render:\n");
            dbg_canvas(canvas);

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
        // Non-terminal backend — use the event loop for interactive mode,
        // static render + optional platform pump for non-interactive demos.
        if (use_event_loop) {
            dbg("Starting non-terminal event loop...\n");
            n00b_canvas_run(canvas, .tick_ms = 50);
            dbg("Non-terminal event loop exited.\n");
        }
        else {
            dbg("Calling canvas_render (non-terminal)...\n");
            n00b_canvas_render(canvas);

            dbg("After render:\n");
            dbg_canvas(canvas);

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
