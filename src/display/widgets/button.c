/*
 * Button widget: clickable with label, Enter/Space activation.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/event.h"
#include "internal/display/widget_primitives.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/theme.h"

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
button_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
button_render(n00b_plane_t *plane, void *data)
{
    n00b_button_t *btn = (n00b_button_t *)data;
    if (!btn || !btn->label) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    // Center the label horizontally and vertically.
    int32_t label_width = n00b_plane_text_width(plane, btn->label, nullptr);
    int32_t x = 0;
    int32_t y = 0;

    if (label_width < content_w) {
        x = (content_w - label_width) / 2;
    }
    if (content_h > 1) {
        int32_t line_h = n00b_plane_line_height(plane, nullptr);
        y = (content_h - line_h) / 2;
    }

    n00b_plane_draw_text(plane, x, y, btn->label);
}

static bool
button_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_button_t *btn = (n00b_button_t *)data;
    if (!btn) {
        return false;
    }

    // Mouse click activates the button.
    if (event->type == N00B_EVENT_MOUSE) {
        if (n00b_widget_event_is_left_press(event)) {
            n00b_plane_set_state(plane, N00B_WSTATE_ACTIVE);
            n00b_plane_mark_dirty(plane);

            if (btn->on_click) {
                btn->on_click(plane, btn->on_click_data);
            }

            n00b_plane_set_state(plane, N00B_WSTATE_FOCUSED);
            n00b_plane_mark_dirty(plane);
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key = event->key.key;

    // Enter or Space activates the button.
    bool activate = n00b_widget_event_is_keyboard_activate(event);

    // Shortcut key match.
    if (!activate && btn->shortcut != 0 && key == btn->shortcut) {
        activate = true;
    }

    if (activate) {
        // Brief ACTIVE state.
        n00b_plane_set_state(plane, N00B_WSTATE_ACTIVE);
        n00b_plane_mark_dirty(plane);

        if (btn->on_click) {
            btn->on_click(plane, btn->on_click_data);
        }

        // Return to FOCUSED state.
        n00b_plane_set_state(plane, N00B_WSTATE_FOCUSED);
        n00b_plane_mark_dirty(plane);
        return true;
    }

    return false;
}

static bool
button_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
button_measure(n00b_plane_t *plane, void *data,
               int32_t *pref_w, int32_t *pref_h,
               int32_t *min_w,  int32_t *min_h)
{
    n00b_button_t *btn = (n00b_button_t *)data;

    int32_t lh = n00b_widget_line_px_height(plane);

    // Determine one character's pixel width for converting character-unit
    // padding (2 chars) into pixels.
    int32_t cpw = n00b_widget_cell_px_width(plane);

    // Report content-area size only.  Box insets (borders + padding)
    // are added by the layout engine automatically.
    if (btn && btn->label) {
        int32_t w = n00b_plane_text_width(plane, btn->label, nullptr);
        *pref_w = (w > 0 ? w + 2 * cpw : 1);
    }
    else {
        *pref_w = 1;
    }
    *pref_h = lh;
    *min_w  = 1;
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_button = {
    .kind         = "button",
    .destroy      = button_destroy,
    .render       = button_render,
    .measure      = button_measure,
    .handle_event = button_handle_event,
    .can_focus    = button_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_button_new(n00b_string_t *label) _kargs {
    n00b_button_cb_t   on_click      = nullptr;
    void              *on_click_data = nullptr;
    n00b_codepoint_t   shortcut      = 0;
    n00b_box_props_t  *box           = nullptr;
    n00b_canvas_t     *canvas        = nullptr;
    int32_t            width         = 0;
    int32_t            height        = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
}
{
    // Auto-create box props with rounded border if none given.
    if (!box) {
        box = n00b_alloc(n00b_box_props_t);
        box->border_theme = &n00b_border_rounded;
        box->borders      = N00B_BORDER_ALL;
        box->pad_left     = 1;
        box->pad_right    = 1;

        // Normal state: gray border.
        box->border_style = n00b_alloc(n00b_text_style_t);
        box->border_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_BORDER);

        // Focused state: blue border to show focus ring.
        n00b_state_style_t *ss_focus = n00b_alloc(n00b_state_style_t);
        ss_focus->border_style = n00b_alloc(n00b_text_style_t);
        ss_focus->border_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_FOCUS);
        ss_focus->border_style->bold   = N00B_TRI_YES;
        box->state_styles[N00B_WSTATE_FOCUSED] = ss_focus;

        // Active state: accent border + inverted fill.
        n00b_state_style_t *ss_active = n00b_alloc(n00b_state_style_t);
        ss_active->border_style = n00b_alloc(n00b_text_style_t);
        ss_active->border_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_ACCENT);
        ss_active->fill_style = n00b_alloc(n00b_text_style_t);
        ss_active->fill_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_PRIMARY);
        ss_active->text_style = n00b_alloc(n00b_text_style_t);
        ss_active->text_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_INVERSE);
        box->state_styles[N00B_WSTATE_ACTIVE] = ss_active;
    }

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .box       = box,
                                           .canvas    = canvas,
                                           .style     = style,
                                           .allocator = allocator);

    // Auto-size from label using plane metrics (content area only; the
    // layout engine accounts for box insets automatically).
    if (width == 0 && label) {
        int32_t w   = n00b_plane_text_width(plane, label, nullptr);
        int32_t cpw = n00b_widget_cell_px_width(plane);
        width = (int32_t)(w > 0 ? w + 2 * cpw : 1);
    }
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = n00b_widget_line_px_height(plane);
    }

    plane->width = width;
    plane->height = height;

    // Buttons should not stretch on the cross axis by default.
    plane->flex.align_self = N00B_ALIGN_START_CROSS;

    n00b_button_t *btn = n00b_alloc(n00b_button_t);
    btn->label         = label;
    btn->on_click      = on_click;
    btn->on_click_data = on_click_data;
    btn->shortcut      = shortcut;

    n00b_widget_attach(plane, &n00b_widget_button, btn);
    n00b_plane_mark_dirty(plane);

    return plane;
}
