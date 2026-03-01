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
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"

// Theme colors (matching slop's dark theme palette).
#define BTN_BORDER_NORMAL   0x585858  // Medium gray
#define BTN_BORDER_FOCUSED  0x89B4FA  // Primary blue
#define BTN_BORDER_ACTIVE   0xF5C2E7  // Pink/accent
#define BTN_TEXT_ACTIVE     0x1E1E2E  // Dark bg for inverted text
#define BTN_BG_ACTIVE       0x89B4FA  // Blue bg when pressed

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

    n00b_isize_t content_rows;
    n00b_isize_t content_cols;
    n00b_plane_content_size(plane, &content_rows, &content_cols);

    if (content_cols == 0 || content_rows == 0) {
        return;
    }

    // Center the label horizontally and vertically.
    int32_t label_width = n00b_unicode_display_width(btn->label);
    n00b_isize_t col_offset = 0;
    n00b_isize_t row_offset = 0;

    if (label_width < (int32_t)content_cols) {
        col_offset = (n00b_isize_t)(((int32_t)content_cols - label_width) / 2);
    }
    if (content_rows > 1) {
        row_offset = (content_rows - 1) / 2;
    }

    n00b_plane_put_str_at(plane, row_offset, col_offset, btn->label);
}

static bool
button_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_button_t *btn = (n00b_button_t *)data;
    if (!btn || event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key = event->key.key;

    // Enter or Space activates the button.
    bool activate = (key == N00B_KEY_ENTER || key == ' ');

    // Shortcut key match.
    if (!activate && btn->shortcut != 0 && key == btn->shortcut) {
        activate = true;
    }

    if (activate) {
        // Brief ACTIVE state.
        n00b_plane_set_state(plane, N00B_WSTATE_ACTIVE);
        plane->flags |= N00B_PLANE_DIRTY;

        if (btn->on_click) {
            btn->on_click(plane, btn->on_click_data);
        }

        // Return to FOCUSED state.
        n00b_plane_set_state(plane, N00B_WSTATE_FOCUSED);
        plane->flags |= N00B_PLANE_DIRTY;
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
               n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
               n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_button_t *btn = (n00b_button_t *)data;

    if (btn && btn->label) {
        int32_t w = n00b_unicode_display_width(btn->label);
        // Add 8 for border + padding + centering margin.
        *pref_cols = (n00b_isize_t)(w > 0 ? w + 6 : 5);
    }
    else {
        *pref_cols = 5;
    }
    *pref_rows = 3;
    *min_cols  = 3;
    *min_rows  = 1;
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
    n00b_isize_t       cols          = 0;
    n00b_isize_t       rows          = 0;
    n00b_text_style_t *style         = nullptr;
    n00b_allocator_t  *allocator     = nullptr;
}
{
    // Auto-size from label.
    if (cols == 0 && label) {
        int32_t w = n00b_unicode_display_width(label);
        cols = (n00b_isize_t)(w > 0 ? w + 6 : 5); // +6 for centering margin.
    }
    if (cols == 0) {
        cols = 10;
    }
    if (rows == 0) {
        rows = 3; // Top border + content + bottom border.
    }

    // Auto-create box props with rounded border if none given.
    if (!box) {
        box = n00b_alloc(n00b_box_props_t);
        box->border_theme = &n00b_border_rounded;
        box->borders      = N00B_BORDER_ALL;
        box->pad_left     = 1;
        box->pad_right    = 1;

        // Normal state: gray border.
        box->border_style = n00b_alloc(n00b_text_style_t);
        box->border_style->fg_rgb = n00b_color_make(BTN_BORDER_NORMAL);

        // Focused state: blue border to show focus ring.
        n00b_state_style_t *ss_focus = n00b_alloc(n00b_state_style_t);
        ss_focus->border_style = n00b_alloc(n00b_text_style_t);
        ss_focus->border_style->fg_rgb = n00b_color_make(BTN_BORDER_FOCUSED);
        ss_focus->border_style->bold   = N00B_TRI_YES;
        box->state_styles[N00B_WSTATE_FOCUSED] = ss_focus;

        // Active state: accent border + inverted fill.
        n00b_state_style_t *ss_active = n00b_alloc(n00b_state_style_t);
        ss_active->border_style = n00b_alloc(n00b_text_style_t);
        ss_active->border_style->fg_rgb = n00b_color_make(BTN_BORDER_ACTIVE);
        ss_active->fill_style = n00b_alloc(n00b_text_style_t);
        ss_active->fill_style->bg_rgb = n00b_color_make(BTN_BG_ACTIVE);
        ss_active->text_style = n00b_alloc(n00b_text_style_t);
        ss_active->text_style->fg_rgb = n00b_color_make(BTN_TEXT_ACTIVE);
        box->state_styles[N00B_WSTATE_ACTIVE] = ss_active;
    }

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .cols      = cols,
                                           .rows      = rows,
                                           .box       = box,
                                           .style     = style,
                                           .allocator = allocator);

    n00b_button_t *btn = n00b_alloc(n00b_button_t);
    btn->label         = label;
    btn->on_click      = on_click;
    btn->on_click_data = on_click_data;
    btn->shortcut      = shortcut;

    n00b_widget_attach(plane, &n00b_widget_button, btn);
    n00b_widget_render(plane);

    return plane;
}
