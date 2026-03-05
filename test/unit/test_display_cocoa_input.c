#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "internal/display/cocoa_input.h"

static void
assert_key_translate(uint32_t key_code,
                     uint32_t cocoa_mod_flags,
                     uint32_t want_key,
                     n00b_key_mod_t want_mods)
{
    n00b_event_t ev = {};
    assert(n00b_cocoa_input_translate_key(key_code, cocoa_mod_flags, &ev));
    assert(ev.type == N00B_EVENT_KEY);
    assert(ev.key.key == want_key);
    assert(ev.key.mods == want_mods);
}

static void
test_modifiers(void)
{
    assert(n00b_cocoa_input_modifiers(0) == N00B_MOD_NONE);
    assert(n00b_cocoa_input_modifiers(N00B_COCOA_MOD_SHIFT) == N00B_MOD_SHIFT);
    assert(n00b_cocoa_input_modifiers(N00B_COCOA_MOD_CTRL) == N00B_MOD_CTRL);
    assert(n00b_cocoa_input_modifiers(N00B_COCOA_MOD_ALT) == N00B_MOD_ALT);

    n00b_key_mod_t combo = n00b_cocoa_input_modifiers(
        N00B_COCOA_MOD_SHIFT | N00B_COCOA_MOD_CTRL | N00B_COCOA_MOD_ALT);
    assert(combo == (N00B_MOD_SHIFT | N00B_MOD_CTRL | N00B_MOD_ALT));

    // Command is folded into ALT to preserve prior Cocoa semantics.
    assert(n00b_cocoa_input_modifiers(N00B_COCOA_MOD_CMD) == N00B_MOD_ALT);

    printf("  [PASS] cocoa modifiers\n");
}

static void
test_function_keys(void)
{
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_UP_ARROW) == N00B_KEY_UP);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_DOWN_ARROW) == N00B_KEY_DOWN);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_LEFT_ARROW) == N00B_KEY_LEFT);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_RIGHT_ARROW) == N00B_KEY_RIGHT);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_HOME) == N00B_KEY_HOME);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_END) == N00B_KEY_END);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_PAGE_UP) == N00B_KEY_PAGE_UP);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_PAGE_DOWN) == N00B_KEY_PAGE_DOWN);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_INSERT) == N00B_KEY_INSERT);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_DELETE) == N00B_KEY_DELETE);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_F1) == N00B_KEY_F1);
    assert(n00b_cocoa_input_function_key(N00B_COCOA_KEY_F12) == N00B_KEY_F12);
    assert(n00b_cocoa_input_function_key(0xF720u) == N00B_KEY_NONE);

    printf("  [PASS] cocoa function-key mapping\n");
}

static void
test_key_translation(void)
{
    assert_key_translate((uint32_t)'a', 0, (uint32_t)'a', N00B_MOD_NONE);
    assert_key_translate((uint32_t)'A', N00B_COCOA_MOD_SHIFT, (uint32_t)'A', N00B_MOD_SHIFT);
    assert_key_translate((uint32_t)'x', N00B_COCOA_MOD_CMD, (uint32_t)'x', N00B_MOD_ALT);

    assert_key_translate((uint32_t)'\r', 0, N00B_KEY_ENTER, N00B_MOD_NONE);
    assert_key_translate((uint32_t)'\n', N00B_COCOA_MOD_SHIFT, N00B_KEY_ENTER, N00B_MOD_SHIFT);
    assert_key_translate((uint32_t)'\t', 0, N00B_KEY_TAB, N00B_MOD_NONE);
    assert_key_translate(N00B_COCOA_KEY_BACKTAB_CHAR, 0, N00B_KEY_TAB, N00B_MOD_SHIFT);
    assert_key_translate((uint32_t)0x1B, 0, N00B_KEY_ESCAPE, N00B_MOD_NONE);
    assert_key_translate((uint32_t)0x7F, 0, N00B_KEY_BACKSPACE, N00B_MOD_NONE);
    assert_key_translate((uint32_t)0x08, 0, N00B_KEY_BACKSPACE, N00B_MOD_NONE);

    assert_key_translate(N00B_COCOA_KEY_UP_ARROW, 0, N00B_KEY_UP, N00B_MOD_NONE);
    assert_key_translate(N00B_COCOA_KEY_F2, N00B_COCOA_MOD_ALT, N00B_KEY_F2, N00B_MOD_ALT);

    n00b_event_t ev = {};
    assert(!n00b_cocoa_input_translate_key(0, 0, &ev));
    assert(!n00b_cocoa_input_translate_key(0xF720u, 0, &ev));
    assert(!n00b_cocoa_input_translate_key((uint32_t)'a', 0, nullptr));

    printf("  [PASS] cocoa key translation\n");
}

static void
test_mouse_translation(void)
{
    n00b_event_t ev = {};
    n00b_cocoa_input_translate_mouse(12,
                                      4,
                                      N00B_MOUSE_RIGHT,
                                      N00B_MOUSE_DRAG,
                                      N00B_COCOA_MOD_SHIFT | N00B_COCOA_MOD_CTRL,
                                      &ev);

    assert(ev.type == N00B_EVENT_MOUSE);
    assert(ev.mouse.x == 12);
    assert(ev.mouse.y == 4);
    assert(ev.mouse.button == N00B_MOUSE_RIGHT);
    assert(ev.mouse.action == N00B_MOUSE_DRAG);
    assert(ev.mouse.mods == (N00B_MOD_SHIFT | N00B_MOD_CTRL));

    // Null output is accepted as a no-op.
    n00b_cocoa_input_translate_mouse(0,
                                      0,
                                      N00B_MOUSE_NONE,
                                      N00B_MOUSE_MOVE,
                                      0,
                                      nullptr);

    printf("  [PASS] cocoa mouse translation\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running display cocoa-input tests...\n");
    test_modifiers();
    test_function_keys();
    test_key_translation();
    test_mouse_translation();

    printf("Display cocoa-input tests passed.\n");
    n00b_shutdown();
    return 0;
}
