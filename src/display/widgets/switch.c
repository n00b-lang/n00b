/*
 * Switch widget: on/off toggle with sliding track visual.
 *
 * Visual layout:  focus(1) + track(5) + space(1) + label
 * Track:          [○━━] (off) or [━━●] (on)
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/switch.h"
#include "display/event.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

// Track glyphs.
#define SW_TRACK_WIDTH 5
#define SW_FOCUS_WIDTH 1

// Unicode codepoints for the track.
#define SW_LBRACKET   0x005B  // [
#define SW_RBRACKET   0x005D  // ]
#define SW_TRACK_BAR  0x2501  // ━
#define SW_THUMB_OFF  0x25CB  // ○
#define SW_THUMB_ON   0x25CF  // ●
#define SW_FOCUS_ON   0x25B8  // ▸
#define SW_FOCUS_OFF  0x0020  // space

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
switch_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
switch_render(n00b_plane_t *plane, void *data)
{
    n00b_switch_t *sw = (n00b_switch_t *)data;
    if (!sw) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    bool focused = (n00b_plane_get_state(plane) == N00B_WSTATE_FOCUSED
                    || n00b_plane_get_state(plane) == N00B_WSTATE_ACTIVE);

    int32_t col = 0;

    // --- Focus indicator ---
    n00b_text_style_t *focus_style = nullptr;
    if (focused) {
        focus_style = n00b_alloc(n00b_text_style_t);
        focus_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_FOCUS);
        focus_style->bold   = N00B_TRI_YES;
    }

    n00b_codepoint_t focus_cp = focused ? SW_FOCUS_ON : SW_FOCUS_OFF;
    n00b_plane_draw_glyph(plane, col, 0, focus_cp, .style = focus_style);
    col += SW_FOCUS_WIDTH * cpw;

    // --- Track ---
    n00b_text_style_t *track_style = n00b_alloc(n00b_text_style_t);
    track_style->fg_rgb = n00b_theme_resolve_color(
        focused ? N00B_PAL_FOCUS : N00B_PAL_BORDER);

    n00b_text_style_t *thumb_style = n00b_alloc(n00b_text_style_t);
    thumb_style->fg_rgb = n00b_theme_resolve_color(
        sw->on ? N00B_PAL_SUCCESS : N00B_PAL_TEXT_SECONDARY);
    thumb_style->bold = N00B_TRI_YES;

    // [
    n00b_plane_draw_glyph(plane, col, 0, SW_LBRACKET, .style = track_style);
    col += cpw;

    if (sw->on) {
        // ━━●
        n00b_plane_draw_glyph(plane, col, 0, SW_TRACK_BAR, .style = track_style);
        col += cpw;
        n00b_plane_draw_glyph(plane, col, 0, SW_TRACK_BAR, .style = track_style);
        col += cpw;
        n00b_plane_draw_glyph(plane, col, 0, SW_THUMB_ON, .style = thumb_style);
        col += cpw;
    }
    else {
        // ○━━
        n00b_plane_draw_glyph(plane, col, 0, SW_THUMB_OFF, .style = thumb_style);
        col += cpw;
        n00b_plane_draw_glyph(plane, col, 0, SW_TRACK_BAR, .style = track_style);
        col += cpw;
        n00b_plane_draw_glyph(plane, col, 0, SW_TRACK_BAR, .style = track_style);
        col += cpw;
    }

    // ]
    n00b_plane_draw_glyph(plane, col, 0, SW_RBRACKET, .style = track_style);
    col += cpw;

    // --- Label ---
    if (sw->label && sw->label->u8_bytes > 0
        && content_w > col + cpw) {
        n00b_plane_draw_text(plane, col + cpw, 0, sw->label);
    }
}

static bool
switch_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_switch_t *sw = (n00b_switch_t *)data;
    if (!sw) {
        return false;
    }

    // Mouse left-click toggles.
    if (event->type == N00B_EVENT_MOUSE) {
        if (event->mouse.button == N00B_MOUSE_LEFT
            && event->mouse.action == N00B_MOUSE_PRESS) {
            sw->on = !sw->on;
            n00b_plane_mark_dirty(plane);

            if (sw->on_change) {
                sw->on_change(plane, sw->on, sw->on_change_data);
            }
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key = event->key.key;

    // Space or Enter toggles.
    if (key == ' ' || key == N00B_KEY_ENTER) {
        sw->on = !sw->on;
        n00b_plane_mark_dirty(plane);

        if (sw->on_change) {
            sw->on_change(plane, sw->on, sw->on_change_data);
        }
        return true;
    }

    return false;
}

static bool
switch_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
switch_measure(n00b_plane_t *plane, void *data,
               int32_t *pref_w, int32_t *pref_h,
               int32_t *min_w,  int32_t *min_h)
{
    n00b_switch_t *sw = (n00b_switch_t *)data;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    int32_t label_w = 0;
    if (sw && sw->label) {
        label_w = n00b_plane_text_width(plane, sw->label, nullptr);
        if (label_w <= 0) {
            label_w = 0;
        }
    }

    // focus(1) + track(5) + space(1) + label  — all in pixel units
    *pref_w = (int32_t)((SW_FOCUS_WIDTH + SW_TRACK_WIDTH + 1) * cpw
                        + (label_w > 0 ? label_w : 0));
    *pref_h = lh;
    *min_w  = (int32_t)((SW_FOCUS_WIDTH + SW_TRACK_WIDTH) * cpw);
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_switch = {
    .kind         = "switch",
    .destroy      = switch_destroy,
    .render       = switch_render,
    .measure      = switch_measure,
    .handle_event = switch_handle_event,
    .can_focus    = switch_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_switch_new(n00b_string_t *label) _kargs {
    bool              on             = false;
    n00b_switch_cb_t  on_change      = nullptr;
    void             *on_change_data = nullptr;
    int32_t           width          = 0;
    int32_t           height         = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_canvas_t     *canvas        = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .style     = style,
                                           .canvas    = canvas,
                                           .allocator = allocator);

    // Auto-size: focus(1) + track(5) + space(1) + label.
    // Use plane text metrics when available; fall back to display width.
    if (width == 0) {
        int32_t char_w = n00b_plane_text_width(
            plane, n00b_string_from_cstr("M"), nullptr);
        if (char_w <= 0) {
            char_w = 1;
        }
        int32_t overhead = (int32_t)(SW_FOCUS_WIDTH + SW_TRACK_WIDTH + 1)
                           * char_w;
        int32_t label_w = 0;
        if (label) {
            label_w = n00b_plane_text_width(plane, label, nullptr);
            if (label_w <= 0) {
                label_w = n00b_unicode_display_width(label);
            }
        }
        width = overhead + (label_w > 0 ? label_w : 0);
        if (width <= 0) {
            width = (int32_t)(SW_FOCUS_WIDTH + SW_TRACK_WIDTH);
        }
    }

    if (height <= 0) {
        height = n00b_plane_line_height(plane, nullptr);
        if (height <= 0) {
            height = 1;
        }
    }

    plane->width = width;
    plane->height = height;

    plane->flex.align_self = N00B_ALIGN_START_CROSS;

    n00b_switch_t *sw = n00b_alloc(n00b_switch_t);
    sw->label          = label;
    sw->on             = on;
    sw->on_change      = on_change;
    sw->on_change_data = on_change_data;

    n00b_widget_attach(plane, &n00b_widget_switch, sw);
    n00b_plane_mark_dirty(plane);

    return plane;
}

bool
n00b_switch_is_on(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_switch) {
        return false;
    }

    n00b_switch_t *sw = (n00b_switch_t *)plane->widget_data;
    return sw->on;
}

void
n00b_switch_set_on(n00b_plane_t *plane, bool on)
{
    if (!plane || plane->widget_vtable != &n00b_widget_switch) {
        return;
    }

    n00b_switch_t *sw = (n00b_switch_t *)plane->widget_data;
    sw->on = on;
    n00b_plane_mark_dirty(plane);
}

void
n00b_switch_toggle(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_switch) {
        return;
    }

    n00b_switch_t *sw = (n00b_switch_t *)plane->widget_data;
    sw->on = !sw->on;
    n00b_plane_mark_dirty(plane);

    if (sw->on_change) {
        sw->on_change(plane, sw->on, sw->on_change_data);
    }
}
