#include "n00b.h"
#include "display/mouse.h"
#include "display/widget.h"
#include "internal/display/event_dispatch.h"

n00b_display_dispatch_result_t
n00b_display_dispatch_event(n00b_canvas_t      *canvas,
                             n00b_focus_mgr_t   *fm,
                             const n00b_event_t *event)
{
    n00b_display_dispatch_result_t result = {};

    if (!canvas || !event) {
        return result;
    }

    if (event->type == N00B_EVENT_KEY) {
        uint32_t       key  = event->key.key;
        n00b_key_mod_t mods = event->key.mods;

        result.handled = true;

        if (key == N00B_KEY_TAB && !(mods & N00B_MOD_SHIFT)) {
            if (fm) {
                n00b_plane_t *before = n00b_focus_mgr_current(fm);
                n00b_focus_mgr_next(fm);
                n00b_plane_t *after = n00b_focus_mgr_current(fm);
                result.focus_changed = before != after;
            }
            return result;
        }

        if (key == N00B_KEY_TAB && (mods & N00B_MOD_SHIFT)) {
            if (fm) {
                n00b_plane_t *before = n00b_focus_mgr_current(fm);
                n00b_focus_mgr_prev(fm);
                n00b_plane_t *after = n00b_focus_mgr_current(fm);
                result.focus_changed = before != after;
            }
            return result;
        }

        if (fm) {
            n00b_plane_t *focused = n00b_focus_mgr_current(fm);
            if (focused) {
                result.handled = n00b_widget_handle_event(focused, event);
            }
            else {
                result.handled = false;
            }
        }
        else {
            result.handled = false;
        }

        if (key == 'c' && (mods & N00B_MOD_CTRL) && !result.handled) {
            result.handled     = true;
            result.should_stop = true;
        }

        return result;
    }

    if (event->type == N00B_EVENT_MOUSE) {
        n00b_plane_t *before = fm ? n00b_focus_mgr_current(fm) : nullptr;
        n00b_mouse_route_event(canvas, fm, event);
        result.handled       = true;
        result.focus_changed = fm && (before != n00b_focus_mgr_current(fm));
    }

    return result;
}
