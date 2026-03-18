#include "n00b.h"
#include "internal/display/cocoa_input.h"

n00b_key_mod_t
n00b_cocoa_input_modifiers(uint32_t cocoa_mod_flags)
{
    n00b_key_mod_t mods = N00B_MOD_NONE;

    if (cocoa_mod_flags & N00B_COCOA_MOD_SHIFT) {
        mods |= N00B_MOD_SHIFT;
    }
    if (cocoa_mod_flags & N00B_COCOA_MOD_CTRL) {
        mods |= N00B_MOD_CTRL;
    }
    if (cocoa_mod_flags & N00B_COCOA_MOD_ALT) {
        mods |= N00B_MOD_ALT;
    }

    // Preserve legacy behavior: Command is treated as Alt in n00b events.
    if (cocoa_mod_flags & N00B_COCOA_MOD_CMD) {
        mods |= N00B_MOD_ALT;
    }

    return mods;
}

uint32_t
n00b_cocoa_input_function_key(uint32_t cocoa_function_key)
{
    switch (cocoa_function_key) {
    case N00B_COCOA_KEY_UP_ARROW:
        return N00B_KEY_UP;
    case N00B_COCOA_KEY_DOWN_ARROW:
        return N00B_KEY_DOWN;
    case N00B_COCOA_KEY_LEFT_ARROW:
        return N00B_KEY_LEFT;
    case N00B_COCOA_KEY_RIGHT_ARROW:
        return N00B_KEY_RIGHT;
    case N00B_COCOA_KEY_HOME:
        return N00B_KEY_HOME;
    case N00B_COCOA_KEY_END:
        return N00B_KEY_END;
    case N00B_COCOA_KEY_PAGE_UP:
        return N00B_KEY_PAGE_UP;
    case N00B_COCOA_KEY_PAGE_DOWN:
        return N00B_KEY_PAGE_DOWN;
    case N00B_COCOA_KEY_INSERT:
        return N00B_KEY_INSERT;
    case N00B_COCOA_KEY_DELETE:
        return N00B_KEY_DELETE;
    case N00B_COCOA_KEY_F1:
        return N00B_KEY_F1;
    case N00B_COCOA_KEY_F2:
        return N00B_KEY_F2;
    case N00B_COCOA_KEY_F3:
        return N00B_KEY_F3;
    case N00B_COCOA_KEY_F4:
        return N00B_KEY_F4;
    case N00B_COCOA_KEY_F5:
        return N00B_KEY_F5;
    case N00B_COCOA_KEY_F6:
        return N00B_KEY_F6;
    case N00B_COCOA_KEY_F7:
        return N00B_KEY_F7;
    case N00B_COCOA_KEY_F8:
        return N00B_KEY_F8;
    case N00B_COCOA_KEY_F9:
        return N00B_KEY_F9;
    case N00B_COCOA_KEY_F10:
        return N00B_KEY_F10;
    case N00B_COCOA_KEY_F11:
        return N00B_KEY_F11;
    case N00B_COCOA_KEY_F12:
        return N00B_KEY_F12;
    default:
        return N00B_KEY_NONE;
    }
}

bool
n00b_cocoa_input_translate_key(uint32_t      key_code,
                                uint32_t      cocoa_mod_flags,
                                n00b_event_t *out)
{
    if (!out || key_code == 0) {
        return false;
    }

    out->type     = N00B_EVENT_KEY;
    out->key.mods = n00b_cocoa_input_modifiers(cocoa_mod_flags);

    if (key_code >= N00B_COCOA_FUNCTION_KEY_MIN
        && key_code <= N00B_COCOA_FUNCTION_KEY_MAX) {
        out->key.key = n00b_cocoa_input_function_key(key_code);
        return out->key.key != N00B_KEY_NONE;
    }

    switch (key_code) {
    case '\r':
    case '\n':
        out->key.key = N00B_KEY_ENTER;
        return true;

    case '\t':
        out->key.key = N00B_KEY_TAB;
        return true;

    case N00B_COCOA_KEY_BACKTAB_CHAR:
        out->key.key = N00B_KEY_TAB;
        out->key.mods = (n00b_key_mod_t)(out->key.mods | N00B_MOD_SHIFT);
        return true;

    case 0x1B:
        out->key.key = N00B_KEY_ESCAPE;
        return true;

    case 0x7F:
    case 0x08:
        out->key.key = N00B_KEY_BACKSPACE;
        return true;

    default:
        out->key.key = key_code;
        return true;
    }
}

void
n00b_cocoa_input_translate_mouse(int32_t              x,
                                  int32_t              y,
                                  n00b_mouse_button_t  button,
                                  n00b_mouse_action_t  action,
                                  uint32_t             cocoa_mod_flags,
                                  n00b_event_t        *out)
{
    if (!out) {
        return;
    }

    out->type         = N00B_EVENT_MOUSE;
    out->mouse.x      = x;
    out->mouse.y      = y;
    out->mouse.button = button;
    out->mouse.action = action;
    out->mouse.mods   = n00b_cocoa_input_modifiers(cocoa_mod_flags);
}

void
n00b_cocoa_input_translate_mouse_point(double               x,
                                       double               y,
                                       n00b_isize_t         cell_w,
                                       n00b_isize_t         cell_h,
                                       n00b_mouse_button_t  button,
                                       n00b_mouse_action_t  action,
                                       uint32_t             cocoa_mod_flags,
                                       n00b_event_t        *out)
{
    (void)cell_w;
    (void)cell_h;

    n00b_cocoa_input_translate_mouse((int32_t)x,
                                     (int32_t)y,
                                     button,
                                     action,
                                     cocoa_mod_flags,
                                     out);
}
