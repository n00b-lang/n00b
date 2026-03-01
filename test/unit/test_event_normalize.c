/*
 * Unit tests for n00b_event_normalize().
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "display/event.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static n00b_event_t
make_key_event(uint32_t key, n00b_key_mod_t mods)
{
    return (n00b_event_t){
        .type = N00B_EVENT_KEY,
        .key  = { .key = key, .mods = mods },
    };
}

static void
assert_key(const n00b_event_t *ev, uint32_t key, n00b_key_mod_t mods,
           const char *label)
{
    if (ev->key.key != key || ev->key.mods != mods) {
        fprintf(stderr,
                "FAIL [%s]: expected key=0x%X mods=0x%X, "
                "got key=0x%X mods=0x%X\n",
                label, key, mods, ev->key.key, ev->key.mods);
        assert(0);
    }
}

// -------------------------------------------------------------------
// Rule 1: Raw CR/LF/TAB/ESC/BS/DEL → symbolic keys
// -------------------------------------------------------------------

static void
test_cr_becomes_enter(void)
{
    n00b_event_t ev = make_key_event('\r', N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ENTER, N00B_MOD_NONE, "CR → ENTER");
}

static void
test_lf_becomes_enter(void)
{
    n00b_event_t ev = make_key_event('\n', N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ENTER, N00B_MOD_NONE, "LF → ENTER");
}

static void
test_cr_strips_ctrl(void)
{
    n00b_event_t ev = make_key_event('\r', N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ENTER, N00B_MOD_NONE,
               "CR with CTRL → ENTER, no CTRL");
}

static void
test_lf_preserves_alt(void)
{
    n00b_event_t ev = make_key_event('\n',
                                      (n00b_key_mod_t)(N00B_MOD_CTRL | N00B_MOD_ALT));
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ENTER, N00B_MOD_ALT,
               "LF with CTRL|ALT → ENTER, ALT only");
}

static void
test_tab_becomes_symbolic(void)
{
    n00b_event_t ev = make_key_event('\t', N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_TAB, N00B_MOD_NONE, "TAB → KEY_TAB");
}

static void
test_tab_preserves_shift(void)
{
    n00b_event_t ev = make_key_event('\t', N00B_MOD_SHIFT);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_TAB, N00B_MOD_SHIFT,
               "Shift+TAB preserved");
}

static void
test_esc_becomes_symbolic(void)
{
    n00b_event_t ev = make_key_event(0x1B, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ESCAPE, N00B_MOD_NONE, "ESC → KEY_ESCAPE");
}

static void
test_bs_becomes_backspace(void)
{
    n00b_event_t ev = make_key_event(0x08, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_BACKSPACE, N00B_MOD_NONE,
               "BS → KEY_BACKSPACE");
}

static void
test_del_becomes_backspace(void)
{
    n00b_event_t ev = make_key_event(0x7F, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_BACKSPACE, N00B_MOD_NONE,
               "DEL → KEY_BACKSPACE");
}

// -------------------------------------------------------------------
// Rule 2: Raw control bytes 1–26 → Ctrl+lowercase letter
// -------------------------------------------------------------------

static void
test_byte3_no_mods_becomes_ctrl_c(void)
{
    // Raw byte 3 with no modifier flag (some backends emit this).
    n00b_event_t ev = make_key_event(3, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'c', N00B_MOD_CTRL,
               "byte 3, no mods → Ctrl+c");
}

static void
test_byte3_with_ctrl_becomes_ctrl_c(void)
{
    n00b_event_t ev = make_key_event(3, N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'c', N00B_MOD_CTRL,
               "byte 3, CTRL → Ctrl+c");
}

static void
test_byte1_becomes_ctrl_a(void)
{
    n00b_event_t ev = make_key_event(1, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'a', N00B_MOD_CTRL, "byte 1 → Ctrl+a");
}

static void
test_byte26_becomes_ctrl_z(void)
{
    n00b_event_t ev = make_key_event(26, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'z', N00B_MOD_CTRL, "byte 26 → Ctrl+z");
}

// -------------------------------------------------------------------
// Rule 3: Ctrl+uppercase → Ctrl+lowercase
// -------------------------------------------------------------------

static void
test_ctrl_uppercase_C_lowered(void)
{
    n00b_event_t ev = make_key_event('C', N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'c', N00B_MOD_CTRL,
               "Ctrl+C → Ctrl+c");
}

static void
test_ctrl_lowercase_c_unchanged(void)
{
    n00b_event_t ev = make_key_event('c', N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'c', N00B_MOD_CTRL,
               "Ctrl+c passthrough");
}

static void
test_ctrl_uppercase_A_lowered(void)
{
    n00b_event_t ev = make_key_event('A', N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'a', N00B_MOD_CTRL,
               "Ctrl+A → Ctrl+a");
}

static void
test_ctrl_uppercase_Z_lowered(void)
{
    n00b_event_t ev = make_key_event('Z', N00B_MOD_CTRL);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'z', N00B_MOD_CTRL,
               "Ctrl+Z → Ctrl+z");
}

// -------------------------------------------------------------------
// Passthrough: printable keys, special keys, non-key events
// -------------------------------------------------------------------

static void
test_printable_passthrough(void)
{
    n00b_event_t ev = make_key_event('x', N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'x', N00B_MOD_NONE,
               "printable 'x' passthrough");
}

static void
test_special_key_passthrough(void)
{
    n00b_event_t ev = make_key_event(N00B_KEY_UP, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_UP, N00B_MOD_NONE,
               "KEY_UP passthrough");
}

static void
test_symbolic_enter_passthrough(void)
{
    // A backend that already emits KEY_ENTER should not be altered.
    n00b_event_t ev = make_key_event(N00B_KEY_ENTER, N00B_MOD_NONE);
    n00b_event_normalize(&ev);
    assert_key(&ev, N00B_KEY_ENTER, N00B_MOD_NONE,
               "KEY_ENTER passthrough");
}

static void
test_alt_uppercase_not_lowered(void)
{
    // Alt+uppercase should NOT be lowercased (only CTRL triggers rule 3).
    n00b_event_t ev = make_key_event('X', N00B_MOD_ALT);
    n00b_event_normalize(&ev);
    assert_key(&ev, 'X', N00B_MOD_ALT,
               "Alt+X stays uppercase");
}

static void
test_non_key_event_passthrough(void)
{
    n00b_event_t ev = {
        .type   = N00B_EVENT_RESIZE,
        .resize = { .rows = 24, .cols = 80 },
    };
    n00b_event_normalize(&ev);
    assert(ev.type == N00B_EVENT_RESIZE);
    assert(ev.resize.rows == 24);
    assert(ev.resize.cols == 80);
}

static void
test_none_event_passthrough(void)
{
    n00b_event_t ev = { .type = N00B_EVENT_NONE };
    n00b_event_normalize(&ev);
    assert(ev.type == N00B_EVENT_NONE);
}

// -------------------------------------------------------------------
// Runner
// -------------------------------------------------------------------

typedef void (*test_fn)(void);

typedef struct {
    const char *name;
    test_fn     fn;
} test_entry_t;

static const test_entry_t tests[] = {
    // Rule 1
    { "cr_becomes_enter",       test_cr_becomes_enter       },
    { "lf_becomes_enter",       test_lf_becomes_enter       },
    { "cr_strips_ctrl",         test_cr_strips_ctrl         },
    { "lf_preserves_alt",       test_lf_preserves_alt       },
    { "tab_becomes_symbolic",   test_tab_becomes_symbolic   },
    { "tab_preserves_shift",    test_tab_preserves_shift    },
    { "esc_becomes_symbolic",   test_esc_becomes_symbolic   },
    { "bs_becomes_backspace",   test_bs_becomes_backspace   },
    { "del_becomes_backspace",  test_del_becomes_backspace  },
    // Rule 2
    { "byte3_no_mods→ctrl_c",  test_byte3_no_mods_becomes_ctrl_c  },
    { "byte3_with_ctrl→ctrl_c", test_byte3_with_ctrl_becomes_ctrl_c },
    { "byte1→ctrl_a",          test_byte1_becomes_ctrl_a   },
    { "byte26→ctrl_z",         test_byte26_becomes_ctrl_z  },
    // Rule 3
    { "ctrl_C→ctrl_c",         test_ctrl_uppercase_C_lowered   },
    { "ctrl_c_passthrough",    test_ctrl_lowercase_c_unchanged },
    { "ctrl_A→ctrl_a",         test_ctrl_uppercase_A_lowered   },
    { "ctrl_Z→ctrl_z",         test_ctrl_uppercase_Z_lowered   },
    // Passthrough
    { "printable_passthrough",      test_printable_passthrough      },
    { "special_key_passthrough",    test_special_key_passthrough    },
    { "symbolic_enter_passthrough", test_symbolic_enter_passthrough },
    { "alt_uppercase_not_lowered",  test_alt_uppercase_not_lowered  },
    { "non_key_event_passthrough",  test_non_key_event_passthrough  },
    { "none_event_passthrough",     test_none_event_passthrough     },
};

int
main(void)
{
    size_t n = sizeof(tests) / sizeof(tests[0]);

    for (size_t i = 0; i < n; i++) {
        printf("  %-35s", tests[i].name);
        tests[i].fn();
        printf(" OK\n");
    }

    printf("\nAll %zu event_normalize tests passed.\n", n);
    return 0;
}
