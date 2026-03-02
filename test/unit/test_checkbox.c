/*
 * Unit tests for the checkbox widget.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/plane.h"
#include "display/render/cell.h"
#include "display/widget.h"
#include "display/render/backend.h"
#include "display/widgets/checkbox.h"
#include "display/event.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static bool g_last_checked = false;
static int  g_change_count = 0;

static void
test_change_handler(n00b_plane_t *plane, bool checked, void *data)
{
    (void)plane;
    (void)data;
    g_last_checked = checked;
    g_change_count++;
}

// -------------------------------------------------------------------
// Test 1: Checkbox creation
// -------------------------------------------------------------------

static void
test_checkbox_create(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Enable");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    assert(cb != nullptr);
    assert(cb->widget_vtable == &n00b_widget_checkbox);
    assert(n00b_widget_can_focus(cb));
    assert(!n00b_checkbox_is_checked(cb));

    printf("  [PASS] checkbox create\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 2: Space toggles checkbox
// -------------------------------------------------------------------

static void
test_checkbox_toggle(void)
{
    g_change_count = 0;

    n00b_string_t *label = n00b_string_from_cstr("Toggle");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .on_change = test_change_handler);

    assert(!n00b_checkbox_is_checked(cb));

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = ' ', .mods = N00B_MOD_NONE },
    };

    // Toggle on.
    bool consumed = n00b_widget_handle_event(cb, &event);
    assert(consumed);
    assert(n00b_checkbox_is_checked(cb));
    assert(g_last_checked == true);
    assert(g_change_count == 1);

    // Toggle off.
    consumed = n00b_widget_handle_event(cb, &event);
    assert(consumed);
    assert(!n00b_checkbox_is_checked(cb));
    assert(g_last_checked == false);
    assert(g_change_count == 2);

    printf("  [PASS] checkbox toggle\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 3: Programmatic set_checked
// -------------------------------------------------------------------

static void
test_checkbox_set_checked(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Option");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    assert(!n00b_checkbox_is_checked(cb));

    n00b_checkbox_set_checked(cb, true);
    assert(n00b_checkbox_is_checked(cb));

    n00b_checkbox_set_checked(cb, false);
    assert(!n00b_checkbox_is_checked(cb));

    printf("  [PASS] checkbox set_checked\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 4: Other keys not consumed
// -------------------------------------------------------------------

static void
test_checkbox_other_key(void)
{
    n00b_string_t *label = n00b_string_from_cstr("X");
    n00b_plane_t  *cb    = n00b_checkbox_new(label);

    n00b_event_t event = {
        .type = N00B_EVENT_KEY,
        .key  = { .key = 'a', .mods = N00B_MOD_NONE },
    };

    bool consumed = n00b_widget_handle_event(cb, &event);
    assert(!consumed);

    printf("  [PASS] checkbox other key\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 5: Explicit BALLOT resolves correctly
// -------------------------------------------------------------------

static void
test_checkbox_indicator_ballot(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_BALLOT);

    n00b_checkbox_t *data = (n00b_checkbox_t *)cb->widget_data;
    assert(data->glyphs.unchecked == 0x2610);
    assert(data->glyphs.checked   == 0x2611);
    assert(data->glyphs.indicator_width == 1);

    printf("  [PASS] checkbox indicator ballot\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 6: AUTO + UNICODE caps → BALLOT
// -------------------------------------------------------------------

static void
test_checkbox_auto_unicode(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_AUTO,
                                               .caps      = N00B_RCAP_UNICODE);

    n00b_checkbox_t *data = (n00b_checkbox_t *)cb->widget_data;
    assert(data->glyphs.unchecked == 0x2610); // ballot
    assert(data->glyphs.indicator_width == 1);

    printf("  [PASS] checkbox auto + unicode → ballot\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 7: AUTO + NONE caps → ASCII
// -------------------------------------------------------------------

static void
test_checkbox_auto_none(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_AUTO,
                                               .caps      = N00B_RCAP_NONE);

    n00b_checkbox_t *data = (n00b_checkbox_t *)cb->widget_data;
    assert(data->glyphs.unchecked == 0); // ASCII path
    assert(data->glyphs.indicator_width == 3);

    printf("  [PASS] checkbox auto + none → ascii\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 8: AUTO + GUI_EXT caps → GUI
// -------------------------------------------------------------------

static void
test_checkbox_auto_gui(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_AUTO,
                                               .caps      = N00B_RCAP_GUI_EXT);

    n00b_checkbox_t *data = (n00b_checkbox_t *)cb->widget_data;
    // GUI uses same codepoints as ballot.
    assert(data->glyphs.unchecked == 0x2610);
    assert(data->indicator == N00B_CB_STYLE_AUTO);

    printf("  [PASS] checkbox auto + gui_ext → gui\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 9: set_indicator changes glyphs
// -------------------------------------------------------------------

static void
test_checkbox_set_indicator(void)
{
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb    = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_ASCII);

    n00b_checkbox_t *data = (n00b_checkbox_t *)cb->widget_data;
    assert(data->glyphs.unchecked == 0); // ASCII
    assert(data->glyphs.indicator_width == 3);

    n00b_checkbox_set_indicator(cb, N00B_CB_STYLE_CIRCLE, N00B_RCAP_UNICODE);

    assert(data->glyphs.unchecked == 0x25CB); // ○
    assert(data->glyphs.checked   == 0x25CF); // ●
    assert(data->glyphs.indicator_width == 1);
    assert(data->indicator == N00B_CB_STYLE_CIRCLE);

    printf("  [PASS] checkbox set_indicator\n");
    n00b_plane_destroy(cb);
}

// -------------------------------------------------------------------
// Test 10: Measure reflects style width
// -------------------------------------------------------------------

static void
test_checkbox_measure_width(void)
{
    // ASCII: focus(1) + indicator(3) + space(1) + label(2) = 7
    n00b_string_t *label = n00b_string_from_cstr("Hi");
    n00b_plane_t  *cb_a  = n00b_checkbox_new(label,
                                               .indicator = N00B_CB_STYLE_ASCII,
                                               .caps      = N00B_RCAP_NONE);

    n00b_isize_t pc, pr, mc, mr;
    n00b_widget_measure(cb_a, &pc, &pr, &mc, &mr);
    assert(pc == 7); // 1 + 3 + 1 + 2

    // BALLOT: focus(1) + indicator(1) + space(1) + label(2) = 5
    n00b_plane_t *cb_b = n00b_checkbox_new(label,
                                             .indicator = N00B_CB_STYLE_BALLOT);

    n00b_widget_measure(cb_b, &pc, &pr, &mc, &mr);
    assert(pc == 5); // 1 + 1 + 1 + 2

    printf("  [PASS] checkbox measure width\n");
    n00b_plane_destroy(cb_a);
    n00b_plane_destroy(cb_b);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running checkbox widget tests...\n");

    test_checkbox_create();
    test_checkbox_toggle();
    test_checkbox_set_checked();
    test_checkbox_other_key();
    test_checkbox_indicator_ballot();
    test_checkbox_auto_unicode();
    test_checkbox_auto_none();
    test_checkbox_auto_gui();
    test_checkbox_set_indicator();
    test_checkbox_measure_width();

    printf("All checkbox tests passed.\n");

    n00b_shutdown();
    return 0;
}
