/*
 * List widget: scrollable single-selection list.
 *
 * Each visible row: marker(2) + item text.
 * Selected row: ▸ + inverted style.
 * Scroll indicators: ▲/▼ at top/bottom when items overflow.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/list_widget.h"
#include "display/event.h"
#include "internal/display/widget_primitives.h"
#include "text/unicode/properties.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

#define LIST_MARKER_WIDTH  2
#define LIST_INIT_CAP      16
#define LIST_MARKER_SEL    0x25B8 // ▸
#define LIST_SCROLL_UP     0x25B2 // ▲
#define LIST_SCROLL_DOWN   0x25BC // ▼

// -------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------

static void
list_ensure_visible(n00b_list_widget_t *lw, n00b_isize_t visible_rows)
{
    if (lw->selected < 0) {
        return;
    }

    if (lw->selected < lw->scroll_offset) {
        lw->scroll_offset = lw->selected;
    }
    else if (lw->selected >= lw->scroll_offset + (int)visible_rows) {
        lw->scroll_offset = lw->selected - (int)visible_rows + 1;
    }
}

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
list_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
list_render(n00b_plane_t *plane, void *data)
{
    n00b_list_widget_t *lw = (n00b_list_widget_t *)data;
    if (!lw) {
        return;
    }

    n00b_plane_clear(plane);

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    if (content_w == 0 || content_h == 0) {
        return;
    }

    int32_t lh = n00b_widget_line_px_height(plane);

    int32_t cpw = n00b_widget_cell_px_width(plane);

    bool focused = n00b_widget_state_is_focused_or_active(plane);

    n00b_isize_t visible = content_h / lh;
    list_ensure_visible(lw, visible);

    bool has_scroll_up   = (lw->scroll_offset > 0);
    bool has_scroll_down = (lw->scroll_offset + (int)visible < (int)lw->item_count);

    for (n00b_isize_t row = 0; row < visible; row++) {
        int item_idx = lw->scroll_offset + (int)row;
        if (item_idx >= (int)lw->item_count) {
            break;
        }

        bool is_selected = (item_idx == lw->selected);

        // Scroll indicator in first/last row.
        if (row == 0 && has_scroll_up) {
            n00b_text_style_t *sc_style = n00b_alloc(n00b_text_style_t);
            sc_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_SECONDARY);
            n00b_plane_draw_glyph(plane, content_w - cpw, row * lh, LIST_SCROLL_UP, .style = sc_style);
        }
        if (row == visible - 1 && has_scroll_down) {
            n00b_text_style_t *sc_style = n00b_alloc(n00b_text_style_t);
            sc_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_SECONDARY);
            n00b_plane_draw_glyph(plane, content_w - cpw, row * lh, LIST_SCROLL_DOWN, .style = sc_style);
        }

        // Selection marker.
        if (is_selected) {
            n00b_text_style_t *marker_style = n00b_alloc(n00b_text_style_t);
            marker_style->fg_rgb = n00b_theme_resolve_color(
                focused ? N00B_PAL_FOCUS : N00B_PAL_PRIMARY);
            marker_style->bold = N00B_TRI_YES;
            n00b_plane_draw_glyph(plane, 0, row * lh, LIST_MARKER_SEL, .style = marker_style);
        }

        // Item text.
        n00b_string_t *item = lw->items[item_idx];
        if (item && item->u8_bytes > 0
            && content_w > LIST_MARKER_WIDTH * cpw) {
            n00b_string_t *display = item;

            if (is_selected) {
                n00b_text_style_t *sel_style = n00b_alloc(n00b_text_style_t);
                if (focused) {
                    sel_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_INVERSE);
                    sel_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_PRIMARY);
                    sel_style->bold   = N00B_TRI_YES;
                }
                else {
                    sel_style->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_PRIMARY);
                    sel_style->bg_rgb = n00b_theme_resolve_color(N00B_PAL_SURFACE_LIGHT);
                }
                display = n00b_str_set_base_style(display, sel_style);
            }

            n00b_plane_draw_text(plane, LIST_MARKER_WIDTH * cpw, row * lh, display);
        }
    }
}

static bool
list_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_list_widget_t *lw = (n00b_list_widget_t *)data;
    if (!lw || lw->item_count == 0) {
        return false;
    }

    // Mouse left-click selects a row.
    if (event->type == N00B_EVENT_MOUSE) {
        if (n00b_widget_event_is_left_press(event)) {
            int line_h = n00b_widget_line_px_height(plane);
            int row = event->mouse.y / line_h;
            int item_idx = lw->scroll_offset + row;
            if (item_idx >= 0 && item_idx < (int)lw->item_count) {
                lw->selected = item_idx;
                n00b_plane_mark_dirty(plane);
            }
            return true;
        }
        // Scroll wheel.
        if (event->mouse.button == N00B_MOUSE_SCROLL_UP) {
            if (lw->scroll_offset > 0) {
                lw->scroll_offset--;
                n00b_plane_mark_dirty(plane);
            }
            return true;
        }
        if (event->mouse.button == N00B_MOUSE_SCROLL_DOWN) {
            int32_t content_w;
            int32_t content_h;
            n00b_plane_content_size(plane, &content_w, &content_h);
            int32_t lh_scroll = n00b_widget_line_px_height(plane);
            int32_t visible_rows = content_h / lh_scroll;
            if (lw->scroll_offset + visible_rows < (int)lw->item_count) {
                lw->scroll_offset++;
                n00b_plane_mark_dirty(plane);
            }
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    uint32_t key = event->key.key;

    int32_t content_w;
    int32_t content_h;
    n00b_plane_content_size(plane, &content_w, &content_h);

    int count = (int)lw->item_count;

    if (key == N00B_KEY_DOWN) {
        if (lw->selected < 0) {
            lw->selected = 0;
        }
        else {
            lw->selected = (lw->selected + 1) % count;
        }
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_UP) {
        if (lw->selected < 0) {
            lw->selected = count - 1;
        }
        else {
            lw->selected = (lw->selected - 1 + count) % count;
        }
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_HOME) {
        lw->selected = 0;
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_END) {
        lw->selected = count - 1;
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_PAGE_DOWN) {
        int32_t lh_key = n00b_widget_line_px_height(plane);
        lw->selected += content_h / lh_key;
        if (lw->selected >= count) {
            lw->selected = count - 1;
        }
        n00b_plane_mark_dirty(plane);
        return true;
    }

    if (key == N00B_KEY_PAGE_UP) {
        int32_t lh_key = n00b_widget_line_px_height(plane);
        lw->selected -= content_h / lh_key;
        if (lw->selected < 0) {
            lw->selected = 0;
        }
        n00b_plane_mark_dirty(plane);
        return true;
    }

    // Enter/Space activates callback.
    if (n00b_widget_event_is_keyboard_activate(event)) {
        if (lw->selected >= 0 && lw->on_select) {
            lw->on_select(plane, lw->selected, lw->on_select_data);
        }
        return true;
    }

    return false;
}

static bool
list_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
list_measure(n00b_plane_t *plane, void *data,
             int32_t *pref_w, int32_t *pref_h,
             int32_t *min_w,  int32_t *min_h)
{
    n00b_list_widget_t *lw = (n00b_list_widget_t *)data;

    int32_t lh = n00b_widget_line_px_height(plane);

    int32_t cpw = n00b_widget_cell_px_width(plane);

    int32_t max_w = 0;
    if (lw) {
        for (n00b_isize_t i = 0; i < lw->item_count; i++) {
            if (lw->items[i]) {
                int32_t w = n00b_plane_text_width(plane, lw->items[i], nullptr);
                if (w > max_w) {
                    max_w = w;
                }
            }
        }
    }

    int32_t count = lw ? (int32_t)lw->item_count : 5;

    *pref_w = LIST_MARKER_WIDTH * cpw + max_w + cpw; // marker + text + scroll indicator
    *pref_h = (count < 5 ? count : 5) * lh;
    *min_w  = (LIST_MARKER_WIDTH + 1) * cpw;
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_list = {
    .kind         = "list",
    .destroy      = list_destroy,
    .render       = list_render,
    .measure      = list_measure,
    .handle_event = list_handle_event,
    .can_focus    = list_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_list_widget_new(n00b_string_t **items,
                     n00b_isize_t    count) _kargs {
    int                   selected       = -1;
    n00b_list_widget_cb_t on_select      = nullptr;
    void                 *on_select_data = nullptr;
    int32_t               width          = 0;
    int32_t               height         = 0;
    n00b_text_style_t    *style          = nullptr;
    n00b_canvas_t        *canvas         = nullptr;
    n00b_allocator_t     *allocator      = nullptr;
}
{
    n00b_plane_t *plane = n00b_new_kargs(n00b_plane_t, plane,
                                           .style     = style,
                                           .canvas    = canvas,
                                           .allocator = allocator);

    // Compute max item width for auto-sizing using plane font metrics.
    int32_t max_w = 0;
    for (n00b_isize_t i = 0; i < count; i++) {
        if (items[i]) {
            int32_t w = n00b_plane_text_width(plane, items[i], nullptr);
            if (w > max_w) {
                max_w = w;
            }
        }
    }

    if (width == 0) {
        width = (int32_t)(LIST_MARKER_WIDTH + max_w + 1);
    }
    if (width < LIST_MARKER_WIDTH + 1) {
        width = LIST_MARKER_WIDTH + 1;
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

    n00b_isize_t cap = count > LIST_INIT_CAP ? count * 2 : LIST_INIT_CAP;

    n00b_list_widget_t *lw = n00b_alloc(n00b_list_widget_t);
    lw->items         = n00b_alloc_array(n00b_string_t *, cap);
    lw->item_count    = count;
    lw->item_capacity = cap;
    lw->selected      = selected;
    lw->scroll_offset = 0;
    lw->on_select     = on_select;
    lw->on_select_data = on_select_data;

    for (n00b_isize_t i = 0; i < count; i++) {
        lw->items[i] = items[i];
    }

    n00b_widget_attach(plane, &n00b_widget_list, lw);
    n00b_plane_mark_dirty(plane);

    return plane;
}

int
n00b_list_widget_get_selected(n00b_plane_t *plane)
{
    n00b_list_widget_t *lw = n00b_widget_data_if_kind(plane, &n00b_widget_list);
    if (!lw) {
        return -1;
    }

    return lw->selected;
}

void
n00b_list_widget_set_selected(n00b_plane_t *plane, int index)
{
    n00b_list_widget_t *lw = n00b_widget_data_if_kind(plane, &n00b_widget_list);
    if (!lw) {
        return;
    }

    if (index < -1 || index >= (int)lw->item_count) {
        return;
    }

    lw->selected = index;
    n00b_plane_mark_dirty(plane);
}

void
n00b_list_widget_add_item(n00b_plane_t *plane, n00b_string_t *item)
{
    n00b_list_widget_t *lw = n00b_widget_data_if_kind(plane, &n00b_widget_list);
    if (!lw) {
        return;
    }

    if (lw->item_count >= lw->item_capacity) {
        n00b_isize_t new_cap = lw->item_capacity * 2;
        n00b_string_t **new_arr = n00b_alloc_array(n00b_string_t *, new_cap);
        for (n00b_isize_t i = 0; i < lw->item_count; i++) {
            new_arr[i] = lw->items[i];
        }
        lw->items         = new_arr;
        lw->item_capacity = new_cap;
    }

    lw->items[lw->item_count++] = item;
    n00b_plane_mark_dirty(plane);
}

void
n00b_list_widget_clear(n00b_plane_t *plane)
{
    n00b_list_widget_t *lw = n00b_widget_data_if_kind(plane, &n00b_widget_list);
    if (!lw) {
        return;
    }

    lw->item_count    = 0;
    lw->selected      = -1;
    lw->scroll_offset = 0;
    n00b_plane_mark_dirty(plane);
}

n00b_isize_t
n00b_list_widget_count(n00b_plane_t *plane)
{
    n00b_list_widget_t *lw = n00b_widget_data_if_kind(plane, &n00b_widget_list);
    if (!lw) {
        return 0;
    }

    return lw->item_count;
}
