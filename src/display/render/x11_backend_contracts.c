#include <stdint.h>

#include "n00b.h"
#include "internal/display/x11_backend_contracts.h"

static uint32_t
x11_decode_utf8_codepoint(const char *buf, int nbytes)
{
    if (!buf || nbytes <= 0) {
        return 0;
    }

    const uint8_t *u = (const uint8_t *)buf;

    if (u[0] < 0x80u) {
        return u[0];
    }

    if ((u[0] & 0xE0u) == 0xC0u && nbytes >= 2) {
        return ((uint32_t)(u[0] & 0x1Fu) << 6)
             | (uint32_t)(u[1] & 0x3Fu);
    }

    if ((u[0] & 0xF0u) == 0xE0u && nbytes >= 3) {
        return ((uint32_t)(u[0] & 0x0Fu) << 12)
             | ((uint32_t)(u[1] & 0x3Fu) << 6)
             | (uint32_t)(u[2] & 0x3Fu);
    }

    if ((u[0] & 0xF8u) == 0xF0u && nbytes >= 4) {
        return ((uint32_t)(u[0] & 0x07u) << 18)
             | ((uint32_t)(u[1] & 0x3Fu) << 12)
             | ((uint32_t)(u[2] & 0x3Fu) << 6)
             | (uint32_t)(u[3] & 0x3Fu);
    }

    return 0;
}

bool
n00b_x11_translate_lookup_bytes(const char    *buf,
                                int            nbytes,
                                n00b_key_mod_t mods,
                                n00b_event_t  *out)
{
    if (!buf || nbytes <= 0 || !out) {
        return false;
    }

    uint32_t key = x11_decode_utf8_codepoint(buf, nbytes);

    if (key == 0) {
        return false;
    }

    out->type     = N00B_EVENT_KEY;
    out->key.mods = mods;

    if (key == '\r' || key == '\n') {
        out->key.key = N00B_KEY_ENTER;
        return true;
    }

    if (key == '\t') {
        out->key.key = N00B_KEY_TAB;
        return true;
    }

    if (key == 0x7Fu || key == 0x08u) {
        out->key.key = N00B_KEY_BACKSPACE;
        return true;
    }

    out->key.key = key;
    return true;
}

void
n00b_x11_translate_motion_state(unsigned int         state,
                                n00b_mouse_button_t *button,
                                n00b_mouse_action_t *action)
{
    if (!button || !action) {
        return;
    }

    *button = N00B_MOUSE_NONE;
    *action = N00B_MOUSE_MOVE;

    if (state & Button1Mask) {
        *button = N00B_MOUSE_LEFT;
        *action = N00B_MOUSE_DRAG;
        return;
    }

    if (state & Button2Mask) {
        *button = N00B_MOUSE_MIDDLE;
        *action = N00B_MOUSE_DRAG;
        return;
    }

    if (state & Button3Mask) {
        *button = N00B_MOUSE_RIGHT;
        *action = N00B_MOUSE_DRAG;
    }
}

void
n00b_x11_note_expose(n00b_x11_pending_state_t *state, int expose_count)
{
    if (!state || expose_count != 0) {
        return;
    }

    state->has_pending_repaint = true;
}

bool
n00b_x11_take_pending_event(n00b_x11_pending_state_t *state,
                            n00b_isize_t              current_rows,
                            n00b_isize_t              current_cols,
                            n00b_event_t             *out)
{
    if (!state || !out) {
        return false;
    }

    out->type = N00B_EVENT_NONE;

    if (state->has_pending_resize) {
        state->has_pending_resize = false;
        out->type = N00B_EVENT_RESIZE;
        out->resize.rows = state->pending_resize_rows;
        out->resize.cols = state->pending_resize_cols;
        return true;
    }

    if (state->has_pending_repaint) {
        state->has_pending_repaint = false;
        out->type = N00B_EVENT_RESIZE;
        out->resize.rows = current_rows;
        out->resize.cols = current_cols;
        return true;
    }

    return false;
}
