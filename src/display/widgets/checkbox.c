/*
 * Checkbox widget: toggle with label and change callback.
 *
 * Supports multiple indicator styles (ASCII, ballot, circle, square,
 * GUI-native) resolved from backend capabilities or set explicitly.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/checkbox.h"
#include "display/event.h"
#include "internal/display/widget_primitives.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

// ====================================================================
// Glyph tables
// ====================================================================

const n00b_checkbox_glyphs_t n00b_cb_glyphs_ascii = {
    .unchecked       = 0,    // String path: "[ ]" / "[x]".
    .checked         = 0,
    .focus_on        = '>',
    .focus_off       = ' ',
    .indicator_width = 3,
    .focus_width     = 1,
};

const n00b_checkbox_glyphs_t n00b_cb_glyphs_ballot = {
    .unchecked       = 0x2610, // ☐
    .checked         = 0x2611, // ☑
    .focus_on        = 0x25B8, // ▸
    .focus_off       = ' ',
    .indicator_width = 1,
    .focus_width     = 1,
};

const n00b_checkbox_glyphs_t n00b_cb_glyphs_circle = {
    .unchecked       = 0x25CB, // ○
    .checked         = 0x25CF, // ●
    .focus_on        = 0x25B8, // ▸
    .focus_off       = ' ',
    .indicator_width = 1,
    .focus_width     = 1,
};

const n00b_checkbox_glyphs_t n00b_cb_glyphs_square = {
    .unchecked       = 0x25A1, // □
    .checked         = 0x25A0, // ■
    .focus_on        = 0x25B8, // ▸
    .focus_off       = ' ',
    .indicator_width = 1,
    .focus_width     = 1,
};

const n00b_checkbox_glyphs_t n00b_cb_glyphs_gui = {
    .unchecked       = 0x2610, // ☐ (same as ballot; GUI backends override rendering)
    .checked         = 0x2611, // ☑
    .focus_on        = 0x25B8, // ▸
    .focus_off       = ' ',
    .indicator_width = 1,
    .focus_width     = 1,
};

// -------------------------------------------------------------------
// Glyph resolution
// -------------------------------------------------------------------

static const n00b_checkbox_glyphs_t *
checkbox_resolve_glyphs(n00b_checkbox_indicator_t indicator,
                        n00b_render_cap_t         caps)
{
    switch (indicator) {
    case N00B_CB_STYLE_ASCII:
        return &n00b_cb_glyphs_ascii;
    case N00B_CB_STYLE_BALLOT:
        return &n00b_cb_glyphs_ballot;
    case N00B_CB_STYLE_CIRCLE:
        return &n00b_cb_glyphs_circle;
    case N00B_CB_STYLE_SQUARE:
        return &n00b_cb_glyphs_square;
    case N00B_CB_STYLE_GUI:
        return &n00b_cb_glyphs_gui;
    case N00B_CB_STYLE_AUTO:
    default:
        if (caps & N00B_RCAP_GUI_EXT) {
            return &n00b_cb_glyphs_gui;
        }
        if (caps & N00B_RCAP_UNICODE) {
            return &n00b_cb_glyphs_ballot;
        }
        return &n00b_cb_glyphs_ascii;
    }
}

// ASCII indicator strings (used when glyphs->unchecked == 0).
#define CB_ASCII_UNCHECKED "[ ]"
#define CB_ASCII_CHECKED   "[x]"

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
checkbox_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
checkbox_render(n00b_plane_t *plane, void *data)
{
    n00b_checkbox_t *cb = (n00b_checkbox_t *)data;
    if (!cb) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    // Pixel width of an average character cell (used to convert glyph counts).
    int32_t cpw = n00b_widget_cell_px_width(plane);

    bool focused = n00b_widget_state_is_focused_or_active(plane);

    const n00b_checkbox_glyphs_t *g = &cb->glyphs;
    int32_t col = 0;

    // --- Focus indicator ---
    n00b_text_style_t *focus_style = nullptr;
    if (focused) {
        focus_style = n00b_alloc(n00b_text_style_t);
        focus_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_FOCUS);
        focus_style->bold   = N00B_TRI_YES;
    }

    n00b_codepoint_t focus_cp = focused ? g->focus_on : g->focus_off;
    n00b_plane_draw_glyph(plane, col, 0, focus_cp, .style = focus_style);
    col += (int32_t)g->focus_width * cpw;

    // --- Checkbox indicator ---
    n00b_text_style_t *ind_style = nullptr;
    if (focused || cb->checked) {
        ind_style = n00b_alloc(n00b_text_style_t);
        ind_style->fg_rgb = n00b_theme_resolve_color(
            cb->checked ? N00B_PAL_SUCCESS : N00B_PAL_FOCUS);
        if (focused) {
            ind_style->bold = N00B_TRI_YES;
        }
    }

    if (g->unchecked == 0) {
        // ASCII string path.
        const char    *indicator = cb->checked ? CB_ASCII_CHECKED
                                               : CB_ASCII_UNCHECKED;
        n00b_string_t *ind_str   = n00b_string_from_cstr(indicator);
        if (ind_style) {
            ind_str = n00b_str_set_base_style(ind_str, ind_style);
        }
        n00b_plane_draw_text(plane, col, 0, ind_str);
        col += n00b_plane_text_width(plane, ind_str, nullptr);
    }
    else {
        // Single-codepoint path.
        n00b_codepoint_t ind_cp = cb->checked ? g->checked : g->unchecked;
        n00b_plane_draw_glyph(plane, col, 0, ind_cp, .style = ind_style);
        col += (int32_t)g->indicator_width * cpw;
    }

    // --- Label ---
    if (cb->label && cb->label->u8_bytes > 0
        && content_w > col + cpw) {
        n00b_plane_draw_text(plane, col + cpw, 0, cb->label);
    }
}

static bool
checkbox_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_checkbox_t *cb = (n00b_checkbox_t *)data;
    if (!cb) {
        return false;
    }

    // Mouse left-click toggles.
    if (event->type == N00B_EVENT_MOUSE) {
        if (n00b_widget_event_is_left_press(event)) {
            cb->checked = !cb->checked;
            n00b_plane_mark_dirty(plane);

            if (cb->on_change) {
                cb->on_change(plane, cb->checked, cb->on_change_data);
            }
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    // Space or Enter toggles.
    if (n00b_widget_event_is_keyboard_activate(event)) {
        cb->checked = !cb->checked;
        n00b_plane_mark_dirty(plane);

        if (cb->on_change) {
            cb->on_change(plane, cb->checked, cb->on_change_data);
        }
        return true;
    }

    return false;
}

static bool
checkbox_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
checkbox_measure(n00b_plane_t *plane, void *data,
                 int32_t *pref_w, int32_t *pref_h,
                 int32_t *min_w,  int32_t *min_h)
{
    n00b_checkbox_t *cb = (n00b_checkbox_t *)data;

    int32_t lh = n00b_widget_line_px_height(plane);

    // Pixel width of an average character cell (use "M" as reference).
    int32_t cpw = n00b_widget_cell_px_width(plane);

    int32_t label_w = 0;
    if (cb && cb->label) {
        label_w = n00b_plane_text_width(plane, cb->label, nullptr);
    }

    uint8_t focus_w     = cb ? cb->glyphs.focus_width     : 1;
    uint8_t indicator_w = cb ? cb->glyphs.indicator_width : 3;

    // focus + indicator + space (all in pixel units) + label
    *pref_w = (int32_t)((focus_w + indicator_w + 1) * cpw
                        + (label_w > 0 ? label_w : 0));
    *pref_h = lh;
    *min_w  = 1;
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_checkbox = {
    .kind         = "checkbox",
    .destroy      = checkbox_destroy,
    .render       = checkbox_render,
    .measure      = checkbox_measure,
    .handle_event = checkbox_handle_event,
    .can_focus    = checkbox_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_checkbox_new(n00b_string_t *label) _kargs {
    bool                       checked        = false;
    n00b_checkbox_cb_t         on_change      = nullptr;
    void                      *on_change_data = nullptr;
    n00b_checkbox_indicator_t  indicator      = N00B_CB_STYLE_AUTO;
    n00b_render_cap_t          caps           = N00B_RCAP_UNICODE;
    n00b_canvas_t             *canvas         = nullptr;
    int32_t                    width          = 0;
    int32_t                    height         = 0;
    n00b_text_style_t         *style          = nullptr;
    n00b_allocator_t          *allocator      = nullptr;
}
{
    const n00b_checkbox_glyphs_t *g = checkbox_resolve_glyphs(indicator, caps);

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .canvas    = canvas,
                                           .style     = style,
                                           .allocator = allocator);

    // Auto-size: focus + indicator + space + label, using plane metrics.
    if (width == 0) {
        int32_t label_w = 0;
        if (label) {
            label_w = n00b_plane_text_width(plane, label, nullptr);
        }
        int32_t cell_w = n00b_widget_cell_px_width(plane);
        int32_t overhead = (int32_t)(g->focus_width + g->indicator_width + 1);
        width = (int32_t)(overhead * cell_w + (label_w > 0 ? label_w : 0));
    }
    if (width == 0) {
        width = (int32_t)(g->focus_width + g->indicator_width);
    }
    if (height == 0) {
        height = n00b_widget_line_px_height(plane);
    }

    plane->width  = width;
    plane->height = height;

    // Checkboxes should not stretch on the cross axis by default.
    plane->flex.align_self = N00B_ALIGN_START_CROSS;

    n00b_checkbox_t *cb = n00b_alloc(n00b_checkbox_t);
    cb->label          = label;
    cb->glyphs         = *g;
    cb->indicator      = indicator;
    cb->checked        = checked;
    cb->on_change      = on_change;
    cb->on_change_data = on_change_data;

    n00b_widget_attach(plane, &n00b_widget_checkbox, cb);
    n00b_plane_mark_dirty(plane);

    return plane;
}

void
n00b_checkbox_set_checked(n00b_plane_t *plane, bool checked)
{
    n00b_checkbox_t *cb =
        n00b_widget_data_if_kind(plane, &n00b_widget_checkbox);
    if (!cb) {
        return;
    }

    cb->checked = checked;
    n00b_plane_mark_dirty(plane);
}

bool
n00b_checkbox_is_checked(n00b_plane_t *plane)
{
    n00b_checkbox_t *cb =
        n00b_widget_data_if_kind(plane, &n00b_widget_checkbox);
    if (!cb) {
        return false;
    }

    return cb->checked;
}

void
n00b_checkbox_set_indicator(n00b_plane_t              *plane,
                             n00b_checkbox_indicator_t  indicator,
                             n00b_render_cap_t          caps)
{
    n00b_checkbox_t *cb =
        n00b_widget_data_if_kind(plane, &n00b_widget_checkbox);
    if (!cb) {
        return;
    }

    const n00b_checkbox_glyphs_t *g = checkbox_resolve_glyphs(indicator, caps);

    cb->indicator = indicator;
    cb->glyphs    = *g;

    n00b_plane_mark_dirty(plane);
}
