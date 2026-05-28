/*
 * Unit tests for the focus manager.
 */

#include <stdio.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/string.h"
#include "adt/option.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/types.h"
#include "display/widget.h"
#include "display/event.h"
#include "display/focus.h"

// -------------------------------------------------------------------
// Dummy focusable widget
// -------------------------------------------------------------------

static void
dummy_render(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
}

static bool
dummy_handle_event(n00b_plane_t *plane, void *data, const n00b_event_t *event)
{
    (void)plane;
    (void)data;
    (void)event;
    return false;
}

static bool
dummy_can_focus(n00b_plane_t *plane, void *data)
{
    (void)plane;
    (void)data;
    return true;
}

static const n00b_widget_vtable_t focusable_vtable = {
    .kind         = "dummy_focusable",
    .render       = dummy_render,
    .handle_event = dummy_handle_event,
    .can_focus    = dummy_can_focus,
};

// Non-focusable widget (no can_focus).
static const n00b_widget_vtable_t nonfocusable_vtable = {
    .kind   = "dummy_nonfocusable",
    .render = dummy_render,
};

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static n00b_plane_t *
make_focusable_plane(const char *name)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 1;
    n00b_widget_attach(p, &focusable_vtable, nullptr);
    (void)name;
    return p;
}

static n00b_plane_t *
make_nonfocusable_plane(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 1;
    n00b_widget_attach(p, &nonfocusable_vtable, nullptr);
    return p;
}

// -------------------------------------------------------------------
// Test 1: Empty canvas has no focused widget
// -------------------------------------------------------------------

static void
test_focus_empty(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);

    assert(fm->count == 0);
    assert(!n00b_option_is_set(n00b_focus_mgr_current(fm)));
    assert(!n00b_option_is_set(n00b_focus_mgr_next(fm)));
    assert(!n00b_option_is_set(n00b_focus_mgr_prev(fm)));

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_deinit(&canvas);

    printf("  [PASS] empty canvas focus\n");
}

// -------------------------------------------------------------------
// Test 2: Tab cycles through focusable widgets
// -------------------------------------------------------------------

static void
test_focus_tab_cycle(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    // Create a root with 3 focusable and 1 non-focusable child.
    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = 80;
    root->height = 25;

    n00b_plane_t *f1 = make_focusable_plane("f1");
    n00b_plane_t *nf = make_nonfocusable_plane();
    n00b_plane_t *f2 = make_focusable_plane("f2");
    n00b_plane_t *f3 = make_focusable_plane("f3");

    n00b_plane_add_child(root, f1, 0, 0);
    n00b_plane_add_child(root, nf, 0, 1);
    n00b_plane_add_child(root, f2, 0, 2);
    n00b_plane_add_child(root, f3, 0, 3);

    n00b_canvas_add_plane(&canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);

    // Should have 3 focusable planes.
    assert(fm->count == 3);

    // Auto-focused on first.
    n00b_option_t(n00b_plane_t *) cur_opt = n00b_focus_mgr_current(fm);
    assert(n00b_option_is_set(cur_opt));
    assert(n00b_option_get(cur_opt) == f1);
    assert(f1->widget_state == N00B_WSTATE_FOCUSED);

    // Tab to second.
    n00b_option_t(n00b_plane_t *) next_opt = n00b_focus_mgr_next(fm);
    assert(n00b_option_is_set(next_opt));
    assert(n00b_option_get(next_opt) == f2);
    cur_opt = n00b_focus_mgr_current(fm);
    assert(n00b_option_is_set(cur_opt));
    assert(n00b_option_get(cur_opt) == f2);
    assert(f1->widget_state == N00B_WSTATE_NORMAL);
    assert(f2->widget_state == N00B_WSTATE_FOCUSED);

    // Tab to third.
    next_opt = n00b_focus_mgr_next(fm);
    assert(n00b_option_is_set(next_opt));
    assert(n00b_option_get(next_opt) == f3);
    assert(f3->widget_state == N00B_WSTATE_FOCUSED);

    // Tab wraps to first.
    next_opt = n00b_focus_mgr_next(fm);
    assert(n00b_option_is_set(next_opt));
    assert(n00b_option_get(next_opt) == f1);
    assert(f1->widget_state == N00B_WSTATE_FOCUSED);
    assert(f3->widget_state == N00B_WSTATE_NORMAL);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_deinit(&canvas);

    printf("  [PASS] tab cycle\n");
}

// -------------------------------------------------------------------
// Test 3: Shift+Tab cycles backwards
// -------------------------------------------------------------------

static void
test_focus_shift_tab(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = 80;
    root->height = 25;

    n00b_plane_t *f1 = make_focusable_plane("f1");
    n00b_plane_t *f2 = make_focusable_plane("f2");

    n00b_plane_add_child(root, f1, 0, 0);
    n00b_plane_add_child(root, f2, 0, 1);
    n00b_canvas_add_plane(&canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);

    n00b_option_t(n00b_plane_t *) cur_opt = n00b_focus_mgr_current(fm);
    assert(n00b_option_is_set(cur_opt));
    assert(n00b_option_get(cur_opt) == f1);

    // Prev wraps to last.
    n00b_option_t(n00b_plane_t *) prev_opt = n00b_focus_mgr_prev(fm);
    assert(n00b_option_is_set(prev_opt));
    assert(n00b_option_get(prev_opt) == f2);
    assert(f2->widget_state == N00B_WSTATE_FOCUSED);
    assert(f1->widget_state == N00B_WSTATE_NORMAL);

    // Prev again → back to first.
    prev_opt = n00b_focus_mgr_prev(fm);
    assert(n00b_option_is_set(prev_opt));
    assert(n00b_option_get(prev_opt) == f1);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_deinit(&canvas);

    printf("  [PASS] shift-tab\n");
}

// -------------------------------------------------------------------
// Test 4: focus_mgr_set focuses a specific widget
// -------------------------------------------------------------------

static void
test_focus_set(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    root->width = 80;
    root->height = 25;

    n00b_plane_t *f1 = make_focusable_plane("f1");
    n00b_plane_t *f2 = make_focusable_plane("f2");

    n00b_plane_add_child(root, f1, 0, 0);
    n00b_plane_add_child(root, f2, 0, 1);
    n00b_canvas_add_plane(&canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);

    // Set focus to f2 directly.
    bool ok = n00b_focus_mgr_set(fm, f2);
    assert(ok);
    n00b_option_t(n00b_plane_t *) cur_opt = n00b_focus_mgr_current(fm);
    assert(n00b_option_is_set(cur_opt));
    assert(n00b_option_get(cur_opt) == f2);
    assert(f2->widget_state == N00B_WSTATE_FOCUSED);
    assert(f1->widget_state == N00B_WSTATE_NORMAL);

    // Try to set focus to a non-focusable plane.
    n00b_plane_t *nf = make_nonfocusable_plane();
    ok = n00b_focus_mgr_set(fm, nf);
    assert(!ok);
    cur_opt = n00b_focus_mgr_current(fm);
    assert(n00b_option_is_set(cur_opt));
    assert(n00b_option_get(cur_opt) == f2);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_deinit(&canvas);
    n00b_plane_destroy(nf);

    printf("  [PASS] focus set\n");
}

// -------------------------------------------------------------------
// Test 5: focus manager owns canvas registration lifecycle
// -------------------------------------------------------------------

static void
test_focus_mgr_registers_with_canvas(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    assert(canvas.focus == nullptr);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);
    assert(canvas.focus == fm);

    n00b_focus_mgr_destroy(fm);
    assert(canvas.focus == nullptr);

    n00b_canvas_deinit(&canvas);

    printf("  [PASS] focus manager registers with canvas\n");
}

// -------------------------------------------------------------------
// Test 6: rebuild blurs focus that drops out of the visible tree
// -------------------------------------------------------------------

static void
test_focus_rebuild_blurs_hidden_plane(void)
{
    n00b_runtime_t *rt = n00b_get_runtime();
    auto *stdout_topic =
        (n00b_conduit_topic_t(n00b_buffer_t *) *)rt->stdout_topic;

    n00b_canvas_t canvas;
    memset(&canvas, 0, sizeof(canvas));
    n00b_canvas_init(&canvas,
                      .vtable = &n00b_renderer_stream,
                      .output = stdout_topic);

    n00b_plane_t *root = n00b_new_kargs(n00b_plane_t, plane);
    n00b_plane_t *f1 = make_focusable_plane("f1");
    n00b_plane_t *f2 = make_focusable_plane("f2");

    root->width = 80;
    root->height = 25;

    n00b_plane_add_child(root, f1, 0, 0);
    n00b_plane_add_child(root, f2, 0, 1);
    n00b_canvas_add_plane(&canvas, root);

    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(&canvas);
    assert(n00b_focus_mgr_current_plane(fm) == f1);
    assert(n00b_focus_mgr_set(fm, f2));
    assert(n00b_focus_mgr_current_plane(fm) == f2);
    assert(f2->widget_state == N00B_WSTATE_FOCUSED);

    n00b_plane_set_visible(f2, false);
    n00b_focus_mgr_rebuild(fm);

    assert(n00b_focus_mgr_current_plane(fm) == f1);
    assert(f1->widget_state == N00B_WSTATE_FOCUSED);
    assert(f2->widget_state == N00B_WSTATE_NORMAL);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_deinit(&canvas);

    printf("  [PASS] focus rebuild blurs hidden plane\n");
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running focus manager tests...\n");

    test_focus_empty();
    test_focus_tab_cycle();
    test_focus_shift_tab();
    test_focus_set();
    test_focus_mgr_registers_with_canvas();
    test_focus_rebuild_blurs_hidden_plane();

    printf("All focus tests passed.\n");

    n00b_shutdown();
    return 0;
}
