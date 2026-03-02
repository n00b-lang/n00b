/*
 * Selection list: multi-select scrollable list with checkbox indicators.
 *
 * Each row: checkbox glyph + space + label.
 * Cursor row: highlighted.  Scroll indicators at overflow.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/backend.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/checkbox.h"
#include "display/widgets/selectionlist.h"
#include "display/event.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

#define SELLIST_INIT_CAP    16
#define SELLIST_SCROLL_UP   0x25B2 // ▲
#define SELLIST_SCROLL_DOWN 0x25BC // ▼

static const n00b_checkbox_glyphs_t *
sellist_resolve_glyphs(n00b_checkbox_indicator_t indicator,
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

// ASCII indicator strings.
#define SEL_ASCII_UNCHECKED "[ ]"
#define SEL_ASCII_CHECKED   "[x]"

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

static void
sellist_ensure_visible(n00b_selectionlist_t *sl, n00b_isize_t visible_rows)
{
    if (sl->cursor < 0) {
        return;
    }

    if (sl->cursor < sl->scroll_offset) {
        sl->scroll_offset = sl->cursor;
    }
    else if (sl->cursor >= sl->scroll_offset + (int)visible_rows) {
        sl->scroll_offset = sl->cursor - (int)visible_rows + 1;
    }
}

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
sellist_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
sellist_render(n00b_plane_t *plane, void *data)
{
    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)data;
    if (!sl) {
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

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    bool focused = (n00b_plane_get_state(plane) == N00B_WSTATE_FOCUSED
                    || n00b_plane_get_state(plane) == N00B_WSTATE_ACTIVE);

    // Visible row count derived from pixel height and line height.
    n00b_isize_t visible = content_h / lh;
    sellist_ensure_visible(sl, visible);

    bool has_scroll_up   = (sl->scroll_offset > 0);
    bool has_scroll_down = (sl->scroll_offset + (int)visible < (int)sl->count);

    const n00b_checkbox_glyphs_t *g = &sl->glyphs;

    for (n00b_isize_t row = 0; row < visible; row++) {
        int item_idx = sl->scroll_offset + (int)row;
        if (item_idx >= (int)sl->count) {
            break;
        }

        n00b_sellist_item_t *item = &sl->items[item_idx];
        bool is_cursor = (item_idx == sl->cursor);

        // Pixel y-coordinate for this row.
        int32_t py = (int32_t)row * lh;

        // Scroll indicators: placed at the right edge (content_w - cpw).
        if (row == 0 && has_scroll_up) {
            n00b_text_style_t *sc_style = n00b_alloc(n00b_text_style_t);
            sc_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_SECONDARY);
            n00b_plane_draw_glyph(plane, content_w - cpw, py, SELLIST_SCROLL_UP, .style = sc_style);
        }
        if (row == visible - 1 && has_scroll_down) {
            n00b_text_style_t *sc_style = n00b_alloc(n00b_text_style_t);
            sc_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_SECONDARY);
            n00b_plane_draw_glyph(plane, content_w - cpw, py, SELLIST_SCROLL_DOWN, .style = sc_style);
        }

        // col accumulates pixel x-position.
        int32_t col = 0;

        // Checkbox glyph.
        n00b_text_style_t *cb_style = nullptr;
        if (item->selected || (is_cursor && focused)) {
            cb_style = n00b_alloc(n00b_text_style_t);
            cb_style->fg_rgb = n00b_theme_resolve_color(
                item->selected ? N00B_PAL_SUCCESS : N00B_PAL_FOCUS);
            if (is_cursor && focused) {
                cb_style->bold = N00B_TRI_YES;
            }
        }

        if (g->unchecked == 0) {
            // ASCII path.
            const char    *ind = item->selected ? SEL_ASCII_CHECKED
                                                : SEL_ASCII_UNCHECKED;
            n00b_string_t *ind_str = n00b_string_from_cstr(ind);
            if (cb_style) {
                ind_str = n00b_str_set_base_style(ind_str, cb_style);
            }
            n00b_plane_draw_text(plane, col, py, ind_str);
        }
        else {
            n00b_codepoint_t cp = item->selected ? g->checked : g->unchecked;
            n00b_plane_draw_glyph(plane, col, py, cp, .style = cb_style);
        }
        // Advance past indicator (indicator_width characters × pixels-per-char).
        col += (int32_t)g->indicator_width * cpw;

        // Label text: one space (cpw pixels) after the indicator.
        if (item->label && item->label->u8_bytes > 0
            && content_w > col + cpw) {
            n00b_string_t *display = item->label;

            if (is_cursor && focused) {
                n00b_text_style_t *cur_style = n00b_alloc(n00b_text_style_t);
                cur_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_INVERSE);
                cur_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_PRIMARY);
                cur_style->bold   = N00B_TRI_YES;
                display = n00b_str_set_base_style(display, cur_style);
            }
            else if (is_cursor) {
                n00b_text_style_t *cur_style = n00b_alloc(n00b_text_style_t);
                cur_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_SURFACE_LIGHT);
                display = n00b_str_set_base_style(display, cur_style);
            }

            n00b_plane_draw_text(plane, col + cpw, py, display);
        }
    }
}

static bool
sellist_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)data;
    if (!sl || sl->count == 0) {
        return false;
    }

    // Mouse left-click toggles.
    if (event->type == N00B_EVENT_MOUSE) {
        if (event->mouse.button == N00B_MOUSE_LEFT
            && event->mouse.action == N00B_MOUSE_PRESS) {
            int line_h = n00b_plane_line_height(plane, nullptr);
            if (line_h < 1) line_h = 1;
            int row = event->mouse.y / line_h;
            int item_idx = sl->scroll_offset + row;
            if (item_idx >= 0 && item_idx < (int)sl->count) {
                sl->cursor = item_idx;
                sl->items[item_idx].selected = !sl->items[item_idx].selected;
                n00b_plane_mark_dirty(plane);
                if (sl->on_change) {
                    sl->on_change(plane, sl->on_change_data);
                }
            }
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key  = event->key.key;
    int      count = (int)sl->count;

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (key == N00B_KEY_DOWN) {
        sl->cursor = (sl->cursor + 1) % count;
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_UP) {
        sl->cursor = (sl->cursor - 1 + count) % count;
        n00b_plane_mark_dirty(plane);
        return true;
    }

    // Space or Enter toggles current item.
    if (key == ' ' || key == N00B_KEY_ENTER) {
        if (sl->cursor >= 0 && sl->cursor < count) {
            sl->items[sl->cursor].selected = !sl->items[sl->cursor].selected;
            n00b_plane_mark_dirty(plane);
            if (sl->on_change) {
                sl->on_change(plane, sl->on_change_data);
            }
        }
        return true;
    }

    // Ctrl+A: select all.
    if (key == 'a' && (event->key.mods & N00B_MOD_CTRL)) {
        for (int i = 0; i < count; i++) {
            sl->items[i].selected = true;
        }
        n00b_plane_mark_dirty(plane);
        if (sl->on_change) {
            sl->on_change(plane, sl->on_change_data);
        }
        return true;
    }

    // Ctrl+D: deselect all.
    if (key == 'd' && (event->key.mods & N00B_MOD_CTRL)) {
        for (int i = 0; i < count; i++) {
            sl->items[i].selected = false;
        }
        n00b_plane_mark_dirty(plane);
        if (sl->on_change) {
            sl->on_change(plane, sl->on_change_data);
        }
        return true;
    }

    return false;
}

static bool
sellist_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
sellist_measure(n00b_plane_t *plane, void *data,
                int32_t *pref_w, int32_t *pref_h,
                int32_t *min_w,  int32_t *min_h)
{
    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)data;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    int32_t cpw = n00b_plane_text_width(plane, n00b_string_from_cstr("M"), nullptr);
    if (cpw <= 0) cpw = 1;

    int32_t max_w = 0;
    uint8_t ind_w = 1;
    if (sl) {
        ind_w = sl->glyphs.indicator_width;
        for (n00b_isize_t i = 0; i < sl->count; i++) {
            if (sl->items[i].label) {
                int32_t w = n00b_plane_text_width(plane, sl->items[i].label, nullptr);
                if (w > max_w) {
                    max_w = w;
                }
            }
        }
    }

    int32_t count = sl ? (int32_t)sl->count : 5;

    // ind + space + label + scroll-indicator column, all in pixels.
    *pref_w = (int32_t)(ind_w * cpw + cpw + max_w + cpw);
    *pref_h = (count < 5 ? count : 5) * lh;
    *min_w  = (int32_t)(ind_w * cpw + cpw);
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_selectionlist = {
    .kind         = "selectionlist",
    .destroy      = sellist_destroy,
    .render       = sellist_render,
    .measure      = sellist_measure,
    .handle_event = sellist_handle_event,
    .can_focus    = sellist_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_selectionlist_new(n00b_string_t **labels,
                       n00b_isize_t    count) _kargs {
    n00b_sellist_cb_t          on_change      = nullptr;
    void                      *on_change_data = nullptr;
    n00b_checkbox_indicator_t  indicator      = N00B_CB_STYLE_AUTO;
    n00b_render_cap_t          caps           = N00B_RCAP_UNICODE;
    int32_t                    width          = 0;
    int32_t                    height         = 0;
    n00b_text_style_t         *style          = nullptr;
    n00b_canvas_t             *canvas         = nullptr;
    n00b_allocator_t          *allocator      = nullptr;
}
{
    const n00b_checkbox_glyphs_t *g = sellist_resolve_glyphs(indicator, caps);

    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .style     = style,
                                           .canvas    = canvas,
                                           .allocator = allocator);

    // Compute max label width for auto-sizing using plane font metrics.
    int32_t max_w = 0;
    for (n00b_isize_t i = 0; i < count; i++) {
        if (labels[i]) {
            int32_t w = n00b_plane_text_width(plane, labels[i], nullptr);
            if (w > max_w) {
                max_w = w;
            }
        }
    }

    if (width == 0) {
        width = (int32_t)(g->indicator_width + 1 + max_w + 1);
    }

    if (height == 0) {
        height = (int32_t)count * n00b_plane_line_height(plane, nullptr);
    }
    if (height <= 0) {
        height = 1;
    }

    plane->width = width;
    plane->height = height;

    plane->flex.align_self = N00B_ALIGN_START_CROSS;

    n00b_isize_t cap = count > SELLIST_INIT_CAP ? count * 2 : SELLIST_INIT_CAP;

    n00b_selectionlist_t *sl = n00b_alloc(n00b_selectionlist_t);
    sl->items         = n00b_alloc_array(n00b_sellist_item_t, cap);
    sl->count         = count;
    sl->capacity      = cap;
    sl->cursor        = 0;
    sl->scroll_offset = 0;
    sl->on_change     = on_change;
    sl->on_change_data = on_change_data;
    sl->glyphs        = *g;

    for (n00b_isize_t i = 0; i < count; i++) {
        sl->items[i].label    = labels[i];
        sl->items[i].selected = false;
        sl->items[i].user_data = nullptr;
    }

    n00b_widget_attach(plane, &n00b_widget_selectionlist, sl);
    n00b_plane_mark_dirty(plane);

    return plane;
}

void
n00b_selectionlist_toggle(n00b_plane_t *plane, int index)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    if (index < 0 || index >= (int)sl->count) {
        return;
    }

    sl->items[index].selected = !sl->items[index].selected;
    n00b_plane_mark_dirty(plane);
}

void
n00b_selectionlist_select_all(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    for (n00b_isize_t i = 0; i < sl->count; i++) {
        sl->items[i].selected = true;
    }
    n00b_plane_mark_dirty(plane);
}

void
n00b_selectionlist_select_none(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    for (n00b_isize_t i = 0; i < sl->count; i++) {
        sl->items[i].selected = false;
    }
    n00b_plane_mark_dirty(plane);
}

bool
n00b_selectionlist_is_selected(n00b_plane_t *plane, int index)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return false;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    if (index < 0 || index >= (int)sl->count) {
        return false;
    }

    return sl->items[index].selected;
}

int
n00b_selectionlist_selected_count(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return 0;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    int count = 0;
    for (n00b_isize_t i = 0; i < sl->count; i++) {
        if (sl->items[i].selected) {
            count++;
        }
    }
    return count;
}

void
n00b_selectionlist_add_item(n00b_plane_t  *plane,
                             n00b_string_t *label,
                             void          *user_data)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;

    if (sl->count >= sl->capacity) {
        n00b_isize_t new_cap = sl->capacity * 2;
        n00b_sellist_item_t *new_arr = n00b_alloc_array(n00b_sellist_item_t, new_cap);
        for (n00b_isize_t i = 0; i < sl->count; i++) {
            new_arr[i] = sl->items[i];
        }
        sl->items    = new_arr;
        sl->capacity = new_cap;
    }

    sl->items[sl->count].label     = label;
    sl->items[sl->count].selected  = false;
    sl->items[sl->count].user_data = user_data;
    sl->count++;

    n00b_plane_mark_dirty(plane);
}

void
n00b_selectionlist_clear(n00b_plane_t *plane)
{
    if (!plane || plane->widget_vtable != &n00b_widget_selectionlist) {
        return;
    }

    n00b_selectionlist_t *sl = (n00b_selectionlist_t *)plane->widget_data;
    sl->count         = 0;
    sl->cursor        = 0;
    sl->scroll_offset = 0;
    n00b_plane_mark_dirty(plane);
}
