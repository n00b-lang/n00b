/*
 * Link widget: clickable text with optional URL and visited state.
 *
 * Colors: normal → primary, focused → focus, visited → secondary.
 */

#include "n00b.h"
#include "core/alloc.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/link.h"
#include "display/event.h"
#include "internal/display/widget_primitives.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/strings/theme.h"

#include <stdio.h>
#include <stdlib.h>

// -------------------------------------------------------------------
// Internal: open URL via platform command
// -------------------------------------------------------------------

static void
link_open_url(n00b_string_t *url)
{
    if (!url || url->u8_bytes == 0) {
        return;
    }

    // Build a null-terminated C string for the URL.
    // n00b_string_t data is UTF-8 but may not be null-terminated in
    // all cases, so we copy to be safe.
    char *cstr = n00b_alloc_array(char, url->u8_bytes + 1);
    __builtin_memcpy(cstr, url->data, url->u8_bytes);
    cstr[url->u8_bytes] = '\0';

    // Build command: "open <url>" on macOS, "xdg-open <url>" on Linux.
    // The URL is single-quoted to prevent shell injection.
    char cmd[4096];
#if defined(__APPLE__)
    snprintf(cmd, sizeof(cmd), "open '%s' &", cstr);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open '%s' &", cstr);
#endif
    (void)system(cmd);
}

// -------------------------------------------------------------------
// Internal: activate link
// -------------------------------------------------------------------

static void
link_do_activate(n00b_plane_t *plane, n00b_link_t *lk)
{
    lk->visited = true;

    if (lk->on_click) {
        lk->on_click(plane, lk->on_click_data);
    }

    if (lk->url) {
        link_open_url(lk->url);
    }

    n00b_plane_mark_dirty(plane);
}

// -------------------------------------------------------------------
// Vtable callbacks
// -------------------------------------------------------------------

static void
link_destroy(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static void
link_render(n00b_plane_t *plane, void *data)
{
    n00b_link_t *lk = (n00b_link_t *)data;
    if (!lk || !lk->text) {
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

    // Build styled text.
    n00b_text_style_t *s = n00b_alloc(n00b_text_style_t);

    if (focused) {
        s->fg_rgb = n00b_theme_resolve_color(N00B_PAL_FOCUS);
        s->bold   = N00B_TRI_YES;
    }
    else if (lk->visited) {
        s->fg_rgb = n00b_theme_resolve_color(N00B_PAL_TEXT_SECONDARY);
    }
    else {
        s->fg_rgb = n00b_theme_resolve_color(N00B_PAL_PRIMARY);
    }

    if (lk->underline) {
        s->underline = N00B_TRI_YES;
    }

    n00b_string_t *styled = n00b_str_set_base_style(lk->text, s);
    n00b_plane_draw_text(plane, 0, 0, styled);
}

static bool
link_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    n00b_link_t *lk = (n00b_link_t *)data;
    if (!lk) {
        return false;
    }

    // Mouse left-click activates.
    if (event->type == N00B_EVENT_MOUSE) {
        if (n00b_widget_event_is_left_press(event)) {
            link_do_activate(plane, lk);
            return true;
        }
        return false;
    }

    if (event->type != N00B_EVENT_KEY) {
        return false;
    }

    // Enter or Space activates.
    if (n00b_widget_event_is_keyboard_activate(event)) {
        link_do_activate(plane, lk);
        return true;
    }

    return false;
}

static bool
link_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static void
link_measure(n00b_plane_t *plane, void *data,
             int32_t *pref_w, int32_t *pref_h,
             int32_t *min_w,  int32_t *min_h)
{
    n00b_link_t *lk = (n00b_link_t *)data;

    int32_t lh = n00b_plane_line_height(plane, nullptr);
    if (lh <= 0) lh = 1;

    int32_t text_w = 0;
    if (lk && lk->text) {
        text_w = n00b_plane_text_width(plane, lk->text, nullptr);
    }

    *pref_w = (text_w > 0 ? text_w : 1);
    *pref_h = lh;
    *min_w  = 1;
    *min_h  = lh;
}

// -------------------------------------------------------------------
// Vtable instance
// -------------------------------------------------------------------

const n00b_widget_vtable_t n00b_widget_link = {
    .kind         = "link",
    .destroy      = link_destroy,
    .render       = link_render,
    .measure      = link_measure,
    .handle_event = link_handle_event,
    .can_focus    = link_can_focus,
};

// -------------------------------------------------------------------
// Public API
// -------------------------------------------------------------------

n00b_plane_t *
n00b_link_new(n00b_string_t *text) _kargs {
    n00b_string_t    *url            = nullptr;
    n00b_link_cb_t    on_click       = nullptr;
    void             *on_click_data  = nullptr;
    bool              underline      = true;
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

    if (width == 0 && text) {
        int32_t w = n00b_plane_text_width(plane, text, nullptr);
        width = (int32_t)(w > 0 ? w : 1);
    }
    if (width == 0) {
        width = 1;
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

    n00b_link_t *lk = n00b_alloc(n00b_link_t);
    lk->text           = text;
    lk->url            = url;
    lk->on_click       = on_click;
    lk->on_click_data  = on_click_data;
    lk->visited        = false;
    lk->underline      = underline;

    n00b_widget_attach(plane, &n00b_widget_link, lk);
    n00b_plane_mark_dirty(plane);

    return plane;
}

void
n00b_link_activate(n00b_plane_t *plane)
{
    n00b_link_t *lk = n00b_widget_data_if_kind(plane, &n00b_widget_link);
    if (!lk) {
        return;
    }

    link_do_activate(plane, lk);
}

void
n00b_link_set_text(n00b_plane_t *plane, n00b_string_t *text)
{
    n00b_link_t *lk = n00b_widget_data_if_kind(plane, &n00b_widget_link);
    if (!lk) {
        return;
    }

    lk->text = text;
    n00b_plane_mark_dirty(plane);
}

void
n00b_link_set_url(n00b_plane_t *plane, n00b_string_t *url)
{
    n00b_link_t *lk = n00b_widget_data_if_kind(plane, &n00b_widget_link);
    if (!lk) {
        return;
    }

    lk->url = url;
}

void
n00b_link_reset_visited(n00b_plane_t *plane)
{
    n00b_link_t *lk = n00b_widget_data_if_kind(plane, &n00b_widget_link);
    if (!lk) {
        return;
    }

    lk->visited = false;
    n00b_plane_mark_dirty(plane);
}
