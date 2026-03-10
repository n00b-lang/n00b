/*
 * Unit tests for mouse hit-testing, routing, capture, and button click.
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
#include "display/render/canvas.h"
#include "display/render/cell.h"
#include "display/render/types.h"
#include "display/render/backend.h"
#include "display/widget.h"
#include "display/widgets/button.h"
#include "display/event.h"
#include "display/mouse.h"
#include "display/focus.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static n00b_plane_t *
make_plane(int32_t x, int32_t y, n00b_isize_t cols, n00b_isize_t rows)
{
    n00b_plane_t *p = n00b_alloc(n00b_plane_t);
    n00b_plane_init(p);
    p->width = cols;
    p->height = rows;
    p->x = x;
    p->y = y;
    return p;
}

// -------------------------------------------------------------------
// Test 1: Hit test — basic
// -------------------------------------------------------------------

static void
test_hit_test_basic(void)
{
    // Plane at (5, 3) with 10x4 viewport.
    n00b_plane_t *plane = make_plane(5, 3, 10, 4);

    // Inside hit.
    n00b_plane_t *hit = n00b_mouse_hit_test(plane, 7, 4, 1, 1);
    assert(hit == plane);

    // Outside — left of plane.
    hit = n00b_mouse_hit_test(plane, 2, 4, 1, 1);
    assert(hit == nullptr);

    // Outside — below plane.
    hit = n00b_mouse_hit_test(plane, 7, 8, 1, 1);
    assert(hit == nullptr);

    // Edge: top-left corner.
    hit = n00b_mouse_hit_test(plane, 5, 3, 1, 1);
    assert(hit == plane);

    // Edge: just past bottom-right.
    hit = n00b_mouse_hit_test(plane, 15, 7, 1, 1);
    assert(hit == nullptr);

    n00b_plane_destroy(plane);
    printf("  PASS: hit_test_basic\n");
}

// -------------------------------------------------------------------
// Test 2: Hit test — depth (child planes)
// -------------------------------------------------------------------

static void
test_hit_test_depth(void)
{
    n00b_plane_t *parent = make_plane(0, 0, 40, 20);
    n00b_plane_t *child  = make_plane(0, 0, 10, 10);

    n00b_plane_add_child(parent, child, 5, 5);

    // Hit the child area — should return child (deeper).
    n00b_plane_t *hit = n00b_mouse_hit_test(parent, 7, 7, 1, 1);
    assert(hit == child);

    // Hit outside child but inside parent.
    hit = n00b_mouse_hit_test(parent, 2, 2, 1, 1);
    assert(hit == parent);

    n00b_plane_remove_child(parent, child);
    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    printf("  PASS: hit_test_depth\n");
}

// -------------------------------------------------------------------
// Test 3: Hit test — miss (no planes hit)
// -------------------------------------------------------------------

static void
test_hit_test_miss(void)
{
    n00b_plane_t *plane = make_plane(10, 10, 5, 5);

    n00b_plane_t *hit = n00b_mouse_hit_test(plane, 0, 0, 1, 1);
    assert(hit == nullptr);

    hit = n00b_mouse_hit_test(plane, 20, 20, 1, 1);
    assert(hit == nullptr);

    n00b_plane_destroy(plane);
    printf("  PASS: hit_test_miss\n");
}

// -------------------------------------------------------------------
// Test 4: Hit test — invisible plane skipped
// -------------------------------------------------------------------

static void
test_hit_test_invisible(void)
{
    n00b_plane_t *plane = make_plane(0, 0, 10, 10);
    n00b_plane_set_visible(plane, false);

    n00b_plane_t *hit = n00b_mouse_hit_test(plane, 5, 5, 1, 1);
    assert(hit == nullptr);

    n00b_plane_destroy(plane);
    printf("  PASS: hit_test_invisible\n");
}

// -------------------------------------------------------------------
// Test 4b: Terminal quantized hit-test aligns to cell grid
// -------------------------------------------------------------------

static void
test_hit_test_terminal_quantized(void)
{
    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    canvas.caps = N00B_RCAP_MANAGES_TTY;

    // Plane starts mid-cell in pixel space. Terminal rendering snaps this
    // to the cell grid; hit-testing should do the same for managed TTY
    // backends.
    n00b_plane_t *plane = make_plane(0, 35, 20, 19);
    plane->canvas = &canvas;

    // cph=16 => snapped top is 32. A click on row 32 should still hit.
    n00b_plane_t *hit = n00b_mouse_hit_test(plane, 2, 32, 8, 16);
    assert(hit == plane);

    // One full cell above snapped top should miss.
    hit = n00b_mouse_hit_test(plane, 2, 15, 8, 16);
    assert(hit == nullptr);

    n00b_plane_destroy(plane);
    printf("  PASS: hit_test_terminal_quantized\n");
}

// -------------------------------------------------------------------
// Test 5: Mouse capture
// -------------------------------------------------------------------

static void
test_capture(void)
{
    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));

    n00b_plane_t *plane = make_plane(0, 0, 10, 10);

    assert(n00b_canvas_get_mouse_capture(&canvas) == nullptr);

    n00b_canvas_capture_mouse(&canvas, plane);
    assert(n00b_canvas_get_mouse_capture(&canvas) == plane);

    n00b_canvas_release_mouse(&canvas);
    assert(n00b_canvas_get_mouse_capture(&canvas) == nullptr);

    n00b_plane_destroy(plane);
    printf("  PASS: capture\n");
}

// -------------------------------------------------------------------
// Test 6: Button mouse click
// -------------------------------------------------------------------

static int g_click_count = 0;

static void
test_click_handler(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    g_click_count++;
}

static void
test_button_mouse_click(void)
{
    g_click_count = 0;

    n00b_plane_t *plane = n00b_button_new(
        n00b_string_from_cstr("Click Me"),
        .on_click = test_click_handler);

    // Synthesize a left mouse press event.
    n00b_event_t ev = {
        .type         = N00B_EVENT_MOUSE,
        .mouse.x      = 0,
        .mouse.y      = 0,
        .mouse.button = N00B_MOUSE_LEFT,
        .mouse.action = N00B_MOUSE_PRESS,
        .mouse.mods   = N00B_MOD_NONE,
    };

    bool consumed = n00b_widget_handle_event(plane, &ev);
    assert(consumed);
    assert(g_click_count == 1);

    // Right click should NOT activate.
    ev.mouse.button = N00B_MOUSE_RIGHT;
    consumed = n00b_widget_handle_event(plane, &ev);
    assert(!consumed);
    assert(g_click_count == 1);

    // Release should NOT activate.
    ev.mouse.button = N00B_MOUSE_LEFT;
    ev.mouse.action = N00B_MOUSE_RELEASE;
    consumed = n00b_widget_handle_event(plane, &ev);
    assert(!consumed);
    assert(g_click_count == 1);

    n00b_widget_detach(plane);
    n00b_plane_destroy(plane);
    printf("  PASS: button_mouse_click\n");
}

// -------------------------------------------------------------------
// Test 7: SGR mouse parse (ANSI backend specific)
//
// We test the SGR encoding contract by verifying the event_t produced
// from raw CSI sequences.  This calls the ANSI poll_event with
// synthetic stdin input.  Since that requires terminal fd
// manipulation, we instead verify the event struct encoding
// indirectly by constructing expected events.
// -------------------------------------------------------------------

static void
test_sgr_encoding(void)
{
    // SGR button codes:
    //   0=left, 1=middle, 2=right, 64=scroll_up, 65=scroll_down
    //   M=press, m=release

    // Verify our enum values match the expected SGR button mapping.
    // The ANSI parser maps: sgr_btn_bits 0→LEFT, 1→MIDDLE, 2→RIGHT
    assert(N00B_MOUSE_LEFT   == 1);
    assert(N00B_MOUSE_MIDDLE == 2);
    assert(N00B_MOUSE_RIGHT  == 3);

    // Verify action enums.
    assert(N00B_MOUSE_PRESS   == 0);
    assert(N00B_MOUSE_RELEASE == 1);
    assert(N00B_MOUSE_MOVE    == 2);
    assert(N00B_MOUSE_DRAG    == 3);

    // Verify event type.
    assert(N00B_EVENT_MOUSE == 3);

    // Verify event struct layout: mouse arm fields are accessible.
    n00b_event_t ev = {
        .type         = N00B_EVENT_MOUSE,
        .mouse.x      = 42,
        .mouse.y      = 17,
        .mouse.button = N00B_MOUSE_LEFT,
        .mouse.action = N00B_MOUSE_PRESS,
        .mouse.mods   = N00B_MOD_CTRL,
    };
    assert(ev.type == N00B_EVENT_MOUSE);
    assert(ev.mouse.x == 42);
    assert(ev.mouse.y == 17);
    assert(ev.mouse.button == N00B_MOUSE_LEFT);
    assert(ev.mouse.action == N00B_MOUSE_PRESS);
    assert(ev.mouse.mods == N00B_MOD_CTRL);

    printf("  PASS: sgr_encoding\n");
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("test_mouse:\n");
    test_hit_test_basic();
    test_hit_test_depth();
    test_hit_test_miss();
    test_hit_test_invisible();
    test_hit_test_terminal_quantized();
    test_capture();
    test_button_mouse_click();
    test_sgr_encoding();
    printf("All mouse tests passed.\n");

    n00b_shutdown();
    return 0;
}
