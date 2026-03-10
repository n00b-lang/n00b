#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/event.h"
#include "internal/display/terminal_input.h"

#if defined(N00B_HAVE_NOTCURSES) && N00B_HAVE_NOTCURSES
#include <notcurses/notcurses.h>
#endif

typedef struct {
    const unsigned char *data;
    size_t               len;
    size_t               ix;
} byte_stream_t;

static int
stream_read_byte(void *vctx, int32_t timeout_ms)
{
    (void)timeout_ms;
    byte_stream_t *s = vctx;
    if (s->ix >= s->len) {
        return -1;
    }
    return (int)s->data[s->ix++];
}

static void
test_parse_basic_keys(void)
{
    const unsigned char bytes[] = { 'a', '\t', '\r', 0x7F };
    byte_stream_t stream = {
        .data = bytes,
        .len  = sizeof(bytes),
    };
    n00b_terminal_input_state_t state = {};
    n00b_event_t                ev = {};

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == 'a');

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == N00B_KEY_TAB);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == N00B_KEY_ENTER);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == N00B_KEY_BACKSPACE);

    printf("  [PASS] ansi parser basic keys\n");
}

static void
test_parse_escape_sequences(void)
{
    const unsigned char bytes[] = {
        0x1B, '[', 'A',
        0x1B, '[', 'Z',
        0x1B, 'x',
    };
    byte_stream_t stream = {
        .data = bytes,
        .len  = sizeof(bytes),
    };
    n00b_terminal_input_state_t state = {};
    n00b_event_t                ev = {};

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == N00B_KEY_UP);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == N00B_KEY_TAB);
    assert(ev.key.mods == N00B_MOD_SHIFT);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_KEY && ev.key.key == 'x');
    assert(ev.key.mods == N00B_MOD_ALT);

    printf("  [PASS] ansi parser escape sequences\n");
}

static void
test_parse_sgr_mouse(void)
{
    const unsigned char bytes[] = {
        0x1B, '[', '<', '0', ';', '1', '0', ';', '5', 'M',
        0x1B, '[', '<', '3', '2', ';', '1', '1', ';', '5', 'M',
        0x1B, '[', '<', '0', ';', '1', '1', ';', '5', 'm',
    };
    byte_stream_t stream = {
        .data = bytes,
        .len  = sizeof(bytes),
    };
    n00b_terminal_input_state_t state = {};
    n00b_event_t                ev = {};

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_MOUSE);
    assert(ev.mouse.button == N00B_MOUSE_LEFT);
    assert(ev.mouse.action == N00B_MOUSE_PRESS);
    assert(ev.mouse.x == 9 && ev.mouse.y == 4);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_MOUSE);
    assert(ev.mouse.action == N00B_MOUSE_DRAG);
    assert(ev.mouse.button == N00B_MOUSE_LEFT);
    assert(ev.mouse.x == 10 && ev.mouse.y == 4);

    assert(n00b_terminal_parse_ansi_event(&state, stream_read_byte, &stream, 0, &ev));
    assert(ev.type == N00B_EVENT_MOUSE);
    assert(ev.mouse.action == N00B_MOUSE_RELEASE);
    assert(!state.mouse_button_down);

    printf("  [PASS] ansi parser sgr mouse\n");
}

static void
test_key_mapping_and_notcurses_translation(void)
{
    assert(n00b_terminal_map_key('\n') == N00B_KEY_ENTER);
    assert(n00b_terminal_map_key(0x7F) == N00B_KEY_BACKSPACE);

    n00b_terminal_input_state_t state = {};
    n00b_event_t                out = {};

    n00b_terminal_ncinput_view_t ctrl_c = {
        .id = 3,
        .eff_text0 = 0,
    };
    assert(n00b_terminal_translate_notcurses(&ctrl_c, &state, 1, 1, &out));
    assert(out.type == N00B_EVENT_KEY);
    assert(out.key.key == 'c');
    assert(out.key.mods & N00B_MOD_CTRL);

    n00b_terminal_ncinput_view_t printable = {
        .id = (uint32_t)'a',
        .shift = true,
        .eff_text0 = (uint32_t)'A',
    };
    assert(n00b_terminal_translate_notcurses(&printable, &state, 1, 1, &out));
    assert(out.type == N00B_EVENT_KEY);
    assert(out.key.key == 'A');
    assert(out.key.mods == N00B_MOD_SHIFT);

    n00b_terminal_ncinput_view_t null_key = {};
    assert(!n00b_terminal_translate_notcurses(&null_key, &state, 1, 1, &out));

#if defined(N00B_HAVE_NOTCURSES) && N00B_HAVE_NOTCURSES
    n00b_terminal_input_reset(&state);
    n00b_terminal_ncinput_view_t mouse = {
        .id     = NCKEY_BUTTON1,
        .evtype = NCTYPE_PRESS,
        .x      = 2,
        .y      = 3,
        .xpx    = 0,
        .ypx    = 0,
    };
    assert(n00b_terminal_translate_notcurses(&mouse, &state, 4, 2, &out));
    assert(out.type == N00B_EVENT_MOUSE);
    assert(out.mouse.button == N00B_MOUSE_LEFT);
    assert(out.mouse.action == N00B_MOUSE_PRESS);
    assert(out.mouse.x == 8 && out.mouse.y == 6);
#endif

    printf("  [PASS] key map + notcurses translation\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display terminal-input tests...\n");
    test_parse_basic_keys();
    test_parse_escape_sequences();
    test_parse_sgr_mouse();
    test_key_mapping_and_notcurses_translation();

    printf("Display terminal-input tests passed.\n");
    n00b_shutdown();
    return 0;
}
