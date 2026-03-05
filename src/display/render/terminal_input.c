#include "n00b.h"
#include "internal/display/terminal_input.h"

#if defined(N00B_HAVE_NOTCURSES) && N00B_HAVE_NOTCURSES
#include <notcurses/notcurses.h>
#endif

static bool
parse_sgr_mouse(n00b_terminal_input_state_t *state,
                const char                  *buf,
                size_t                       len,
                bool                         pressed,
                n00b_event_t                *out)
{
    int cb = 0;
    int cx = 0;
    int cy = 0;
    int field = 0;

    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ';') {
            field++;
            continue;
        }
        if (buf[i] < '0' || buf[i] > '9') {
            continue;
        }
        int digit = buf[i] - '0';
        switch (field) {
        case 0:
            cb = cb * 10 + digit;
            break;
        case 1:
            cx = cx * 10 + digit;
            break;
        case 2:
            cy = cy * 10 + digit;
            break;
        default:
            break;
        }
    }

    out->type = N00B_EVENT_MOUSE;
    out->mouse.x = cx - 1;
    out->mouse.y = cy - 1;
    out->mouse.mods = N00B_MOD_NONE;
    if (cb & 4) {
        out->mouse.mods |= N00B_MOD_SHIFT;
    }
    if (cb & 8) {
        out->mouse.mods |= N00B_MOD_ALT;
    }
    if (cb & 16) {
        out->mouse.mods |= N00B_MOD_CTRL;
    }

    bool is_motion = (cb & 32) != 0;
    int btn_bits = cb & 3;

    if (cb & 64) {
        out->mouse.button = (btn_bits == 0) ? N00B_MOUSE_SCROLL_UP
                                             : N00B_MOUSE_SCROLL_DOWN;
        out->mouse.action = N00B_MOUSE_PRESS;
        return true;
    }

    if (is_motion) {
        if (state->mouse_button_down) {
            out->mouse.action = N00B_MOUSE_DRAG;
            switch (btn_bits) {
            case 0:
                out->mouse.button = N00B_MOUSE_LEFT;
                break;
            case 1:
                out->mouse.button = N00B_MOUSE_MIDDLE;
                break;
            case 2:
                out->mouse.button = N00B_MOUSE_RIGHT;
                break;
            default:
                out->mouse.button = N00B_MOUSE_NONE;
                break;
            }
        }
        else {
            out->mouse.action = N00B_MOUSE_MOVE;
            out->mouse.button = N00B_MOUSE_NONE;
        }
        return true;
    }

    switch (btn_bits) {
    case 0:
        out->mouse.button = N00B_MOUSE_LEFT;
        break;
    case 1:
        out->mouse.button = N00B_MOUSE_MIDDLE;
        break;
    case 2:
        out->mouse.button = N00B_MOUSE_RIGHT;
        break;
    default:
        out->mouse.button = N00B_MOUSE_NONE;
        break;
    }

    if (!pressed) {
        out->mouse.action = N00B_MOUSE_RELEASE;
        state->mouse_button_down = false;
    }
    else {
        out->mouse.action = N00B_MOUSE_PRESS;
        state->mouse_button_down = true;
    }

    return true;
}

static bool
parse_csi(n00b_terminal_input_state_t *state,
          n00b_terminal_read_byte_fn   read_byte,
          void                        *io_ctx,
          n00b_event_t                *out)
{
    char   buf[32];
    size_t len = 0;

    for (;;) {
        int c = read_byte(io_ctx, 50);
        if (c < 0) {
            return false;
        }
        if (c >= 0x40 && c <= 0x7E) {
            if ((c == 'M' || c == 'm') && len > 0 && buf[0] == '<') {
                return parse_sgr_mouse(state, buf + 1, len - 1, c == 'M', out);
            }

            out->type = N00B_EVENT_KEY;
            out->key.mods = N00B_MOD_NONE;
            switch (c) {
            case 'A':
                out->key.key = N00B_KEY_UP;
                return true;
            case 'B':
                out->key.key = N00B_KEY_DOWN;
                return true;
            case 'C':
                out->key.key = N00B_KEY_RIGHT;
                return true;
            case 'D':
                out->key.key = N00B_KEY_LEFT;
                return true;
            case 'H':
                out->key.key = N00B_KEY_HOME;
                return true;
            case 'F':
                out->key.key = N00B_KEY_END;
                return true;
            case 'Z':
                out->key.key = N00B_KEY_TAB;
                out->key.mods = N00B_MOD_SHIFT;
                return true;
            case '~':
                if (len == 0) {
                    return false;
                }
                int num = 0;
                for (size_t i = 0; i < len; i++) {
                    if (buf[i] < '0' || buf[i] > '9') {
                        break;
                    }
                    num = num * 10 + (buf[i] - '0');
                }
                switch (num) {
                case 1:
                    out->key.key = N00B_KEY_HOME;
                    return true;
                case 2:
                    out->key.key = N00B_KEY_INSERT;
                    return true;
                case 3:
                    out->key.key = N00B_KEY_DELETE;
                    return true;
                case 4:
                    out->key.key = N00B_KEY_END;
                    return true;
                case 5:
                    out->key.key = N00B_KEY_PAGE_UP;
                    return true;
                case 6:
                    out->key.key = N00B_KEY_PAGE_DOWN;
                    return true;
                case 15:
                    out->key.key = N00B_KEY_F5;
                    return true;
                case 17:
                    out->key.key = N00B_KEY_F6;
                    return true;
                case 18:
                    out->key.key = N00B_KEY_F7;
                    return true;
                case 19:
                    out->key.key = N00B_KEY_F8;
                    return true;
                case 20:
                    out->key.key = N00B_KEY_F9;
                    return true;
                case 21:
                    out->key.key = N00B_KEY_F10;
                    return true;
                case 23:
                    out->key.key = N00B_KEY_F11;
                    return true;
                case 24:
                    out->key.key = N00B_KEY_F12;
                    return true;
                default:
                    return false;
                }
            default:
                return false;
            }
        }
        if (len < sizeof(buf) - 1) {
            buf[len++] = (char)c;
        }
    }
}

static bool
parse_ss3(n00b_terminal_read_byte_fn read_byte, void *io_ctx, n00b_event_t *out)
{
    int c = read_byte(io_ctx, 50);
    if (c < 0) {
        return false;
    }

    out->type = N00B_EVENT_KEY;
    out->key.mods = N00B_MOD_NONE;
    switch (c) {
    case 'P':
        out->key.key = N00B_KEY_F1;
        return true;
    case 'Q':
        out->key.key = N00B_KEY_F2;
        return true;
    case 'R':
        out->key.key = N00B_KEY_F3;
        return true;
    case 'S':
        out->key.key = N00B_KEY_F4;
        return true;
    case 'A':
        out->key.key = N00B_KEY_UP;
        return true;
    case 'B':
        out->key.key = N00B_KEY_DOWN;
        return true;
    case 'C':
        out->key.key = N00B_KEY_RIGHT;
        return true;
    case 'D':
        out->key.key = N00B_KEY_LEFT;
        return true;
    default:
        return false;
    }
}

void
n00b_terminal_input_reset(n00b_terminal_input_state_t *state)
{
    if (!state) {
        return;
    }
    state->mouse_button_down = false;
}

bool
n00b_terminal_parse_ansi_event(n00b_terminal_input_state_t *state,
                                n00b_terminal_read_byte_fn   read_byte,
                                void                        *io_ctx,
                                int32_t                      timeout_ms,
                                n00b_event_t                *out)
{
    if (!state || !read_byte || !out) {
        return false;
    }

    out->type = N00B_EVENT_NONE;
    int c = read_byte(io_ctx, timeout_ms);
    if (c < 0) {
        return false;
    }

    if (c == 0x1B) {
        int next = read_byte(io_ctx, 50);
        if (next < 0) {
            out->type = N00B_EVENT_KEY;
            out->key.key = N00B_KEY_ESCAPE;
            out->key.mods = N00B_MOD_NONE;
            return true;
        }
        if (next == '[') {
            return parse_csi(state, read_byte, io_ctx, out);
        }
        if (next == 'O') {
            return parse_ss3(read_byte, io_ctx, out);
        }

        out->type = N00B_EVENT_KEY;
        out->key.key = (uint32_t)next;
        out->key.mods = N00B_MOD_ALT;
        return true;
    }

    out->type = N00B_EVENT_KEY;
    out->key.mods = N00B_MOD_NONE;

    if (c == 0x7F || c == 0x08) {
        out->key.key = N00B_KEY_BACKSPACE;
        return true;
    }
    if (c == '\r' || c == '\n') {
        out->key.key = N00B_KEY_ENTER;
        return true;
    }
    if (c == '\t') {
        out->key.key = N00B_KEY_TAB;
        return true;
    }
    if (c == 0x19) {
        out->key.key = N00B_KEY_TAB;
        out->key.mods = N00B_MOD_SHIFT;
        return true;
    }
    if (c < 0x20) {
        out->key.key = (uint32_t)(c + 'a' - 1);
        out->key.mods = N00B_MOD_CTRL;
        return true;
    }

    out->key.key = (uint32_t)c;
    return true;
}

uint32_t
n00b_terminal_map_key(uint32_t raw_key)
{
    switch (raw_key) {
#if defined(N00B_HAVE_NOTCURSES) && N00B_HAVE_NOTCURSES
    case NCKEY_UP:
        return N00B_KEY_UP;
    case NCKEY_DOWN:
        return N00B_KEY_DOWN;
    case NCKEY_LEFT:
        return N00B_KEY_LEFT;
    case NCKEY_RIGHT:
        return N00B_KEY_RIGHT;
    case NCKEY_HOME:
        return N00B_KEY_HOME;
    case NCKEY_END:
        return N00B_KEY_END;
    case NCKEY_PGUP:
        return N00B_KEY_PAGE_UP;
    case NCKEY_PGDOWN:
        return N00B_KEY_PAGE_DOWN;
    case NCKEY_INS:
        return N00B_KEY_INSERT;
    case NCKEY_DEL:
        return N00B_KEY_DELETE;
    case NCKEY_BACKSPACE:
        return N00B_KEY_BACKSPACE;
    case NCKEY_TAB:
        return N00B_KEY_TAB;
    case NCKEY_ENTER:
        return N00B_KEY_ENTER;
    case NCKEY_ESC:
        return N00B_KEY_ESCAPE;
    case NCKEY_F01:
        return N00B_KEY_F1;
    case NCKEY_F02:
        return N00B_KEY_F2;
    case NCKEY_F03:
        return N00B_KEY_F3;
    case NCKEY_F04:
        return N00B_KEY_F4;
    case NCKEY_F05:
        return N00B_KEY_F5;
    case NCKEY_F06:
        return N00B_KEY_F6;
    case NCKEY_F07:
        return N00B_KEY_F7;
    case NCKEY_F08:
        return N00B_KEY_F8;
    case NCKEY_F09:
        return N00B_KEY_F9;
    case NCKEY_F10:
        return N00B_KEY_F10;
    case NCKEY_F11:
        return N00B_KEY_F11;
    case NCKEY_F12:
        return N00B_KEY_F12;
#endif
    case 0x7F:
    case 0x08:
        return N00B_KEY_BACKSPACE;
    case '\r':
    case '\n':
        return N00B_KEY_ENTER;
    default:
        return raw_key;
    }
}

bool
n00b_terminal_translate_notcurses(const n00b_terminal_ncinput_view_t *in,
                                   n00b_terminal_input_state_t         *state,
                                   n00b_isize_t                         cell_px_w,
                                   n00b_isize_t                         cell_px_h,
                                   n00b_event_t                        *out)
{
    if (!in || !state || !out) {
        return false;
    }

    out->type = N00B_EVENT_NONE;
    if (cell_px_w < 1) {
        cell_px_w = 1;
    }
    if (cell_px_h < 1) {
        cell_px_h = 1;
    }

#if defined(N00B_HAVE_NOTCURSES) && N00B_HAVE_NOTCURSES
    if (in->id >= NCKEY_BUTTON1 && in->id <= NCKEY_BUTTON3) {
        out->type = N00B_EVENT_MOUSE;
        out->mouse.x = in->x * (int32_t)cell_px_w;
        out->mouse.y = in->y * (int32_t)cell_px_h;
        out->mouse.mods = N00B_MOD_NONE;
        if (in->shift) {
            out->mouse.mods |= N00B_MOD_SHIFT;
        }
        if (in->ctrl) {
            out->mouse.mods |= N00B_MOD_CTRL;
        }
        if (in->alt) {
            out->mouse.mods |= N00B_MOD_ALT;
        }

        switch (in->id) {
        case NCKEY_BUTTON1:
            out->mouse.button = N00B_MOUSE_LEFT;
            break;
        case NCKEY_BUTTON2:
            out->mouse.button = N00B_MOUSE_MIDDLE;
            break;
        case NCKEY_BUTTON3:
            out->mouse.button = N00B_MOUSE_RIGHT;
            break;
        default:
            out->mouse.button = N00B_MOUSE_NONE;
            break;
        }

        if (in->evtype == NCTYPE_RELEASE) {
            out->mouse.action = N00B_MOUSE_RELEASE;
            state->mouse_button_down = false;
        }
        else if (state->mouse_button_down) {
            out->mouse.action = N00B_MOUSE_DRAG;
        }
        else {
            out->mouse.action = N00B_MOUSE_PRESS;
            state->mouse_button_down = true;
        }
        return true;
    }

    if (in->id == NCKEY_SCROLL_UP || in->id == NCKEY_SCROLL_DOWN) {
        out->type = N00B_EVENT_MOUSE;
        out->mouse.x = in->x * (int32_t)cell_px_w;
        out->mouse.y = in->y * (int32_t)cell_px_h;
        out->mouse.button = (in->id == NCKEY_SCROLL_UP) ? N00B_MOUSE_SCROLL_UP
                                                         : N00B_MOUSE_SCROLL_DOWN;
        out->mouse.action = N00B_MOUSE_PRESS;
        out->mouse.mods = N00B_MOD_NONE;
        if (in->shift) {
            out->mouse.mods |= N00B_MOD_SHIFT;
        }
        if (in->ctrl) {
            out->mouse.mods |= N00B_MOD_CTRL;
        }
        if (in->alt) {
            out->mouse.mods |= N00B_MOD_ALT;
        }
        return true;
    }

    if (in->id == NCKEY_MOTION) {
        out->type = N00B_EVENT_MOUSE;
        out->mouse.x = in->x * (int32_t)cell_px_w;
        out->mouse.y = in->y * (int32_t)cell_px_h;
        out->mouse.button = N00B_MOUSE_NONE;
        out->mouse.mods = N00B_MOD_NONE;
        out->mouse.action = state->mouse_button_down ? N00B_MOUSE_DRAG
                                                      : N00B_MOUSE_MOVE;
        return true;
    }

    if (in->evtype == NCTYPE_RELEASE) {
        return false;
    }

    if (in->id >= NCKEY_INVALID) {
        uint32_t mapped = n00b_terminal_map_key(in->id);
        if (mapped == in->id) {
            return false;
        }
    }
#endif

    n00b_key_mod_t mods = N00B_MOD_NONE;
    if (in->shift) {
        mods |= N00B_MOD_SHIFT;
    }
    if (in->ctrl) {
        mods |= N00B_MOD_CTRL;
    }
    if (in->alt) {
        mods |= N00B_MOD_ALT;
    }

    out->type = N00B_EVENT_KEY;
    out->key.mods = mods;

    uint32_t mapped = n00b_terminal_map_key(in->id);
    if (mapped != in->id) {
        out->key.key = mapped;
        return true;
    }

    if (in->id >= 1 && in->id <= 26) {
        out->key.key = in->id + 'a' - 1;
        out->key.mods |= N00B_MOD_CTRL;
        return true;
    }

    out->key.key = in->eff_text0 ? in->eff_text0 : in->id;
    if (out->key.key == 0) {
        out->type = N00B_EVENT_NONE;
        return false;
    }

    return true;
}
