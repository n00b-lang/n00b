/*
 * Checkbox widget: toggle with label and change callback.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/checkbox.h"
#include "display/event.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"

// Indicator strings (ASCII for universal font compatibility).
#define CHECKBOX_UNCHECKED_STR "[ ]"
#define CHECKBOX_CHECKED_STR   "[x]"
#define CHECKBOX_INDICATOR_WIDTH 3

// Focus indicator: 1-char glyph shown when focused, space otherwise.
#define CHECKBOX_FOCUS_INDICATOR ">"
#define CHECKBOX_FOCUS_WIDTH     1

// Theme colors.
#define CB_FOCUS_COLOR   0x89B4FA  // Primary blue
#define CB_CHECKED_COLOR 0xA6E3A1  // Green for checked

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

    n00b_isize_t content_rows;
    n00b_isize_t content_cols;
    n00b_plane_content_size(plane, &content_rows, &content_cols);

    if (content_cols == 0 || content_rows == 0) {
        return;
    }

    bool focused = (n00b_plane_get_state(plane) == N00B_WSTATE_FOCUSED
                    || n00b_plane_get_state(plane) == N00B_WSTATE_ACTIVE);

    n00b_isize_t col = 0;

    // Focus indicator: ">" when focused, " " otherwise (always reserve space).
    const char *focus_glyph = focused ? CHECKBOX_FOCUS_INDICATOR : " ";
    n00b_string_t *focus_str = n00b_string_from_cstr(focus_glyph);
    if (focused) {
        n00b_text_style_t *fs = n00b_alloc(n00b_text_style_t);
        fs->fg_rgb = n00b_color_make(CB_FOCUS_COLOR);
        fs->bold   = N00B_TRI_YES;
        focus_str = n00b_str_set_base_style(focus_str, fs);
    }
    n00b_plane_put_str_at(plane, 0, col, focus_str);
    col += CHECKBOX_FOCUS_WIDTH;

    // Render checkbox indicator with color.
    const char *indicator = cb->checked ? CHECKBOX_CHECKED_STR
                                        : CHECKBOX_UNCHECKED_STR;
    n00b_string_t *ind_str = n00b_string_from_cstr(indicator);
    if (focused || cb->checked) {
        n00b_text_style_t *is = n00b_alloc(n00b_text_style_t);
        is->fg_rgb = n00b_color_make(cb->checked ? CB_CHECKED_COLOR
                                                  : CB_FOCUS_COLOR);
        if (focused) {
            is->bold = N00B_TRI_YES;
        }
        ind_str = n00b_str_set_base_style(ind_str, is);
    }
    n00b_plane_put_str_at(plane, 0, col, ind_str);
    col += CHECKBOX_INDICATOR_WIDTH;

    // Render label after indicator + space.
    if (cb->label && cb->label->u8_bytes > 0
        && content_cols > col + 1) {
        n00b_plane_put_str_at(plane, 0, col + 1, cb->label);
    }
}

static bool
checkbox_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_checkbox_t *cb = (n00b_checkbox_t *)data;
    if (!cb || event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key = event->key.key;

    // Space or Enter toggles.
    if (key == ' ' || key == N00B_KEY_ENTER) {
        cb->checked = !cb->checked;
        n00b_widget_render(plane);

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
                 n00b_isize_t *pref_cols, n00b_isize_t *pref_rows,
                 n00b_isize_t *min_cols,  n00b_isize_t *min_rows)
{
    (void)plane;
    n00b_checkbox_t *cb = (n00b_checkbox_t *)data;

    // focus(1) + indicator(3) + space(1) + label width.
    int32_t label_w = 0;
    if (cb && cb->label) {
        label_w = n00b_unicode_display_width(cb->label);
    }
    *pref_cols = (n00b_isize_t)(CHECKBOX_FOCUS_WIDTH
                                + CHECKBOX_INDICATOR_WIDTH + 1
                                + (label_w > 0 ? label_w : 0));
    *pref_rows = 1;
    *min_cols  = 1;
    *min_rows  = 1;
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
    bool                checked        = false;
    n00b_checkbox_cb_t  on_change      = nullptr;
    void               *on_change_data = nullptr;
    n00b_isize_t        cols           = 0;
    n00b_isize_t        rows           = 1;
    n00b_text_style_t  *style          = nullptr;
    n00b_allocator_t   *allocator      = nullptr;
}
{
    // Auto-size: focus(1) + indicator(3) + space(1) + label.
    if (cols == 0 && label) {
        int32_t w = n00b_unicode_display_width(label);
        cols = (n00b_isize_t)(CHECKBOX_FOCUS_WIDTH
                              + CHECKBOX_INDICATOR_WIDTH + 1
                              + (w > 0 ? w : 0));
    }
    if (cols == 0) {
        cols = CHECKBOX_FOCUS_WIDTH + CHECKBOX_INDICATOR_WIDTH;
    }

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .cols      = cols,
                                           .rows      = rows,
                                           .style     = style,
                                           .allocator = allocator);

    n00b_checkbox_t *cb = n00b_alloc(n00b_checkbox_t);
    cb->label          = label;
    cb->checked        = checked;
    cb->on_change      = on_change;
    cb->on_change_data = on_change_data;

    n00b_widget_attach(plane, &n00b_widget_checkbox, cb);
    n00b_widget_render(plane);

    return plane;
}

void
n00b_checkbox_set_checked(n00b_plane_t *plane, bool checked)
{
    if (!plane || plane->widget_vtable != &n00b_widget_checkbox) {
        return;
    }

    n00b_checkbox_t *cb = (n00b_checkbox_t *)plane->widget_data;
    cb->checked = checked;
    n00b_widget_render(plane);
}

bool
n00b_checkbox_is_checked(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_checkbox) {
        return false;
    }

    n00b_checkbox_t *cb = (n00b_checkbox_t *)plane->widget_data;
    return cb->checked;
}
