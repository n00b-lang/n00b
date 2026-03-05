/*
 * Radio button widget with group-based mutual exclusion.
 *
 * Visual layout: focus(1) + radio glyph + space(1) + label
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/radio.h"
#include "display/event.h"
#include "internal/display/widget_primitives.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

// ====================================================================
// Glyph tables
// ====================================================================

const n00b_radio_glyphs_t n00b_radio_glyphs_ascii = {
    .unselected      = 0,     // String path: "( )" / "(*)"
    .selected        = 0,
    .focus_on        = '>',
    .focus_off       = ' ',
    .indicator_width = 3,
    .focus_width     = 1,
};

const n00b_radio_glyphs_t n00b_radio_glyphs_unicode = {
    .unselected      = 0x25CB, // ○
    .selected        = 0x25C9, // ◉
    .focus_on        = 0x25B8, // ▸
    .focus_off       = ' ',
    .indicator_width = 1,
    .focus_width     = 1,
};

// ASCII indicator strings.
#define RADIO_ASCII_UNSELECTED "( )"
#define RADIO_ASCII_SELECTED   "(*)"

// Initial group capacity.
#define RADIO_GROUP_INIT_CAP 8

// -------------------------------------------------------------------
// Glyph resolution
// -------------------------------------------------------------------

static const n00b_radio_glyphs_t *
radio_resolve_glyphs(n00b_radio_indicator_t indicator,
                     n00b_render_cap_t      caps)
{
    switch (indicator) {
    case N00B_RADIO_STYLE_ASCII:
        return &n00b_radio_glyphs_ascii;
    case N00B_RADIO_STYLE_UNICODE:
        return &n00b_radio_glyphs_unicode;
    case N00B_RADIO_STYLE_AUTO:
    default:
        if (caps & (N00B_RCAP_UNICODE | N00B_RCAP_GUI_EXT)) {
            return &n00b_radio_glyphs_unicode;
        }
        return &n00b_radio_glyphs_ascii;
    }
}

// -------------------------------------------------------------------
// Internal: select a radio in a group
// -------------------------------------------------------------------

static void
radio_group_select(n00b_radio_group_t *group, int index)
{
    if (!group || index < -1 || index >= (int)group->count) {
        return;
    }

    int prev = group->selected;
    if (prev == index) {
        return;
    }

    group->selected = index;

    // Re-render the previously selected radio.
    if (prev >= 0 && prev < (int)group->count) {
        n00b_widget_render(group->radios[prev]);
    }

    // Re-render the newly selected radio.
    if (index >= 0 && index < (int)group->count) {
        n00b_widget_render(group->radios[index]);
    }

    if (group->on_change) {
        // Use the newly selected plane, or nullptr if deselected.
        n00b_plane_t *plane = (index >= 0) ? group->radios[index] : nullptr;
        group->on_change(plane, index, group->on_change_data);
    }
}

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
radio_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
radio_render(n00b_plane_t *plane, void *data)
{
    n00b_radio_t *radio = (n00b_radio_t *)data;
    if (!radio) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    bool focused = n00b_widget_state_is_focused_or_active(plane);
    bool is_selected = (radio->group && radio->group->selected == radio->index);

    const n00b_radio_glyphs_t *g = &radio->glyphs;
    int32_t col = 0;

    int32_t cpw = n00b_widget_cell_px_width(plane);

    // For multi-row planes, center the indicator vertically.
    int32_t ind_row = content_h > 1 ? content_h / 2 : 0;

    // --- Focus indicator ---
    n00b_text_style_t *focus_style = nullptr;
    if (focused) {
        focus_style = n00b_alloc(n00b_text_style_t);
        focus_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_FOCUS);
        focus_style->bold   = N00B_TRI_YES;
    }

    n00b_codepoint_t focus_cp = focused ? g->focus_on : g->focus_off;
    n00b_plane_draw_glyph(plane, col, ind_row, focus_cp, .style = focus_style);
    col += g->focus_width * cpw;

    // --- Radio indicator ---
    n00b_text_style_t *ind_style = nullptr;
    if (focused || is_selected) {
        ind_style = n00b_alloc(n00b_text_style_t);
        ind_style->fg_rgb = n00b_theme_resolve_color(
            is_selected ? N00B_PAL_SUCCESS : N00B_PAL_FOCUS);
        if (focused) {
            ind_style->bold = N00B_TRI_YES;
        }
    }

    if (g->unselected == 0) {
        // ASCII string path.
        const char    *indicator = is_selected ? RADIO_ASCII_SELECTED
                                               : RADIO_ASCII_UNSELECTED;
        n00b_string_t *ind_str   = n00b_string_from_cstr(indicator);
        if (ind_style) {
            ind_str = n00b_str_set_base_style(ind_str, ind_style);
        }
        n00b_plane_draw_text(plane, col, ind_row, ind_str);
    }
    else {
        n00b_codepoint_t ind_cp = is_selected ? g->selected : g->unselected;
        n00b_plane_draw_glyph(plane, col, ind_row, ind_cp, .style = ind_style);
    }
    col += g->indicator_width * cpw;

    // --- Label ---
    if (radio->label && radio->label->u8_bytes > 0
        && content_w > col + cpw) {
        n00b_plane_draw_text(plane, col + cpw, ind_row, radio->label);
    }
}

static bool
radio_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_radio_t *radio = (n00b_radio_t *)data;
    if (!radio || !radio->group) {
        return false;
    }

    // Mouse left-click selects.
    if (event->type == N00B_EVENT_MOUSE) {
        if (n00b_widget_event_is_left_press(event)) {
            radio_group_select(radio->group, radio->index);
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    // Space or Enter selects.
    if (n00b_widget_event_is_keyboard_activate(event)) {
        radio_group_select(radio->group, radio->index);
        return true;
    }

    return false;
}

static bool
radio_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
radio_measure(n00b_plane_t *plane, void *data,
              int32_t *pref_w, int32_t *pref_h,
              int32_t *min_w,  int32_t *min_h)
{
    n00b_radio_t *radio = (n00b_radio_t *)data;

    int32_t lh = n00b_widget_line_px_height(plane);

    int32_t cpw = n00b_widget_cell_px_width(plane);

    int32_t label_w = 0;
    if (radio && radio->label) {
        label_w = n00b_plane_text_width(plane, radio->label, nullptr);
    }

    uint8_t focus_w     = radio ? radio->glyphs.focus_width     : 1;
    uint8_t indicator_w = radio ? radio->glyphs.indicator_width : 3;

    *pref_w = (int32_t)((focus_w + indicator_w + 1) * cpw
                        + (label_w > 0 ? label_w : 0));
    *pref_h = lh;
    *min_w  = 1;
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_radio = {
    .kind         = "radio",
    .destroy      = radio_destroy,
    .render       = radio_render,
    .measure      = radio_measure,
    .handle_event = radio_handle_event,
    .can_focus    = radio_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_radio_group_t *
n00b_radio_group_new(void)
{
    n00b_radio_group_t *g = n00b_alloc(n00b_radio_group_t);
    g->radios   = n00b_alloc_array(n00b_plane_t *, RADIO_GROUP_INIT_CAP);
    g->count    = 0;
    g->capacity = RADIO_GROUP_INIT_CAP;
    g->selected = -1;
    return g;
}

n00b_plane_t *
n00b_radio_new(n00b_string_t *label) _kargs {
    n00b_radio_group_t    *group     = nullptr;
    n00b_radio_indicator_t indicator = N00B_RADIO_STYLE_AUTO;
    n00b_render_cap_t      caps      = N00B_RCAP_UNICODE;
    n00b_canvas_t         *canvas    = nullptr;
    int32_t                width     = 0;
    int32_t                height    = 0;
    n00b_text_style_t     *style     = nullptr;
    n00b_allocator_t      *allocator = nullptr;
}
{
    const n00b_radio_glyphs_t *g = radio_resolve_glyphs(indicator, caps);

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

    plane->flex.align_self = N00B_ALIGN_START_CROSS;

    n00b_radio_t *radio = n00b_alloc(n00b_radio_t);
    radio->label  = label;
    radio->glyphs = *g;
    radio->group  = group;

    // Add to group if provided.
    if (group) {
        if (group->count >= group->capacity) {
            n00b_isize_t new_cap = group->capacity * 2;
            n00b_plane_t **new_arr = n00b_alloc_array(n00b_plane_t *, new_cap);
            for (n00b_isize_t i = 0; i < group->count; i++) {
                new_arr[i] = group->radios[i];
            }
            group->radios   = new_arr;
            group->capacity = new_cap;
        }
        radio->index = (int)group->count;
        group->radios[group->count++] = plane;
    }

    n00b_widget_attach(plane, &n00b_widget_radio, radio);
    n00b_plane_mark_dirty(plane);

    return plane;
}

int
n00b_radio_group_get_selected(n00b_radio_group_t *group)
{
    return group ? group->selected : -1;
}

void
n00b_radio_group_set_selected(n00b_radio_group_t *group, int index)
{
    radio_group_select(group, index);
}

bool
n00b_radio_is_selected(n00b_plane_t *plane)
{
    n00b_radio_t *radio = n00b_widget_data_if_kind(plane, &n00b_widget_radio);
    if (!radio) {
        return false;
    }

    return (radio->group && radio->group->selected == radio->index);
}

void
n00b_radio_group_on_change(n00b_radio_group_t *group,
                            n00b_radio_cb_t     cb,
                            void               *data)
{
    if (!group) {
        return;
    }
    group->on_change      = cb;
    group->on_change_data = data;
}
