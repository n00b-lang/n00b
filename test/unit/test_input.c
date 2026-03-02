/*
 * Unit tests for the input widget.
 */

#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/widget.h"
#include "display/widgets/input.h"
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

static bool g_submitted = false;
static bool g_changed   = false;

static void
test_on_submit(n00b_plane_t *plane, n00b_string_t *text, void *data)
{
    (void)plane;
    (void)text;
    (void)data;
    g_submitted = true;
}

static void
test_on_change(n00b_plane_t *plane, n00b_string_t *text, void *data)
{
    (void)plane;
    (void)text;
    (void)data;
    g_changed = true;
}

// -------------------------------------------------------------------
// Test 1: Input creation
// -------------------------------------------------------------------

static void
test_input_create(void)
{
    n00b_plane_t *inp = n00b_input_new(.width = 20);

    assert(inp != nullptr);
    assert(inp->widget_vtable == &n00b_widget_input);
    assert(n00b_widget_can_focus(inp));

    n00b_string_t *text = n00b_input_get_text(inp);
    assert(text != nullptr);
    assert(text->codepoints == 0);

    printf("  [PASS] input create\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Test 2: Type characters
// -------------------------------------------------------------------

static void
test_input_type(void)
{
    g_changed = false;

    n00b_plane_t *inp = n00b_input_new(.width = 20,
                                         .on_change = test_on_change);

    // Type "Hi".
    n00b_event_t e_H = make_key_event('H', N00B_MOD_NONE);
    n00b_event_t e_i = make_key_event('i', N00B_MOD_NONE);

    assert(n00b_widget_handle_event(inp, &e_H));
    assert(n00b_widget_handle_event(inp, &e_i));
    assert(g_changed);

    n00b_string_t *text = n00b_input_get_text(inp);
    assert(text->codepoints == 2);
    assert(text->data[0] == 'H');
    assert(text->data[1] == 'i');

    printf("  [PASS] input type\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Test 3: Backspace
// -------------------------------------------------------------------

static void
test_input_backspace(void)
{
    n00b_string_t *initial = n00b_string_from_cstr("AB");
    n00b_plane_t  *inp     = n00b_input_new(.width = 20, .text = initial);

    n00b_string_t *text = n00b_input_get_text(inp);
    assert(text->codepoints == 2);

    n00b_event_t e_bs = make_key_event(N00B_KEY_BACKSPACE, N00B_MOD_NONE);
    n00b_widget_handle_event(inp, &e_bs);

    text = n00b_input_get_text(inp);
    assert(text->codepoints == 1);
    assert(text->data[0] == 'A');

    printf("  [PASS] input backspace\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Test 4: Set text programmatically
// -------------------------------------------------------------------

static void
test_input_set_text(void)
{
    n00b_plane_t *inp = n00b_input_new(.width = 20);

    n00b_string_t *new_text = n00b_string_from_cstr("Hello");
    n00b_input_set_text(inp, new_text);

    n00b_string_t *got = n00b_input_get_text(inp);
    assert(got->codepoints == 5);
    assert(memcmp(got->data, "Hello", 5) == 0);

    printf("  [PASS] input set_text\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Test 5: Enter triggers submit
// -------------------------------------------------------------------

static void
test_input_submit(void)
{
    g_submitted = false;

    n00b_plane_t *inp = n00b_input_new(.width = 20,
                                         .on_submit = test_on_submit);

    n00b_event_t e_enter = make_key_event(N00B_KEY_ENTER, N00B_MOD_NONE);
    bool consumed = n00b_widget_handle_event(inp, &e_enter);
    assert(consumed);
    assert(g_submitted);

    printf("  [PASS] input submit\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Test 6: Max length
// -------------------------------------------------------------------

static void
test_input_max_length(void)
{
    n00b_plane_t *inp = n00b_input_new(.width = 20, .max_length = 3);

    n00b_event_t e_a = make_key_event('a', N00B_MOD_NONE);
    n00b_widget_handle_event(inp, &e_a);
    n00b_widget_handle_event(inp, &e_a);
    n00b_widget_handle_event(inp, &e_a);
    n00b_widget_handle_event(inp, &e_a); // Should be rejected.

    n00b_string_t *text = n00b_input_get_text(inp);
    assert(text->codepoints == 3);

    printf("  [PASS] input max_length\n");
    n00b_plane_destroy(inp);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running input widget tests...\n");

    test_input_create();
    test_input_type();
    test_input_backspace();
    test_input_set_text();
    test_input_submit();
    test_input_max_length();

    printf("All input tests passed.\n");

    n00b_shutdown();
    return 0;
}
