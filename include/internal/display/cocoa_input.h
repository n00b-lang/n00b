#pragma once

#include <stdbool.h>
#include <stdint.h>

#if defined(__OBJC__)
#include "display/render/cocoa_bridge.h"
#else
#include "n00b.h"
#include "display/event.h"
#endif

typedef enum : uint8_t {
    N00B_COCOA_MOD_SHIFT = 1 << 0,
    N00B_COCOA_MOD_CTRL  = 1 << 1,
    N00B_COCOA_MOD_ALT   = 1 << 2,
    N00B_COCOA_MOD_CMD   = 1 << 3,
} n00b_cocoa_mod_mask_t;

enum {
    N00B_COCOA_FUNCTION_KEY_MIN = 0xF700u,
    N00B_COCOA_FUNCTION_KEY_MAX = 0xF8FFu,

    N00B_COCOA_KEY_UP_ARROW    = 0xF700u,
    N00B_COCOA_KEY_DOWN_ARROW  = 0xF701u,
    N00B_COCOA_KEY_LEFT_ARROW  = 0xF702u,
    N00B_COCOA_KEY_RIGHT_ARROW = 0xF703u,

    N00B_COCOA_KEY_F1  = 0xF704u,
    N00B_COCOA_KEY_F2  = 0xF705u,
    N00B_COCOA_KEY_F3  = 0xF706u,
    N00B_COCOA_KEY_F4  = 0xF707u,
    N00B_COCOA_KEY_F5  = 0xF708u,
    N00B_COCOA_KEY_F6  = 0xF709u,
    N00B_COCOA_KEY_F7  = 0xF70Au,
    N00B_COCOA_KEY_F8  = 0xF70Bu,
    N00B_COCOA_KEY_F9  = 0xF70Cu,
    N00B_COCOA_KEY_F10 = 0xF70Du,
    N00B_COCOA_KEY_F11 = 0xF70Eu,
    N00B_COCOA_KEY_F12 = 0xF70Fu,

    N00B_COCOA_KEY_INSERT    = 0xF727u,
    N00B_COCOA_KEY_DELETE    = 0xF728u,
    N00B_COCOA_KEY_HOME      = 0xF729u,
    N00B_COCOA_KEY_END       = 0xF72Bu,
    N00B_COCOA_KEY_PAGE_UP   = 0xF72Cu,
    N00B_COCOA_KEY_PAGE_DOWN = 0xF72Du,

    N00B_COCOA_KEY_BACKTAB_CHAR = 0x19u,
};

extern n00b_key_mod_t n00b_cocoa_input_modifiers(uint32_t cocoa_mod_flags);
extern uint32_t       n00b_cocoa_input_function_key(uint32_t cocoa_function_key);
extern bool           n00b_cocoa_input_translate_key(uint32_t      key_code,
                                                       uint32_t      cocoa_mod_flags,
                                                       n00b_event_t *out);
extern void           n00b_cocoa_input_translate_mouse(int32_t              x,
                                                         int32_t              y,
                                                         n00b_mouse_button_t  button,
                                                         n00b_mouse_action_t  action,
                                                         uint32_t             cocoa_mod_flags,
                                                         n00b_event_t        *out);
