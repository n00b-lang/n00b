#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/runtime.h"
#include "display/focus.h"
#include "display/mouse.h"
#include "display/render/backend.h"
#include "display/render/canvas.h"
#include "display/render/draw_cmd.h"
#include "display/render/plane.h"
#include "display/widget.h"
#include "display/widgets/scroll.h"
#include "display/widgets/text.h"
#include "internal/display/scene_contracts.h"
#include "text/strings/string_ops.h"

extern void n00b_stream_backend_set_size(void        *ctx,
                                          n00b_isize_t rows,
                                          n00b_isize_t cols);

static n00b_canvas_t *
make_stream_canvas(n00b_isize_t rows, n00b_isize_t cols)
{
    n00b_canvas_t *canvas = n00b_new_kargs(n00b_canvas_t, canvas,
                                            .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(canvas->backend_ctx, rows, cols);
    n00b_canvas_resize(canvas, rows, cols);
    return canvas;
}

static void
destroy_plane_tree(n00b_plane_t *plane)
{
    if (!plane) {
        return;
    }

    while (plane->children.data && plane->children.len > 0) {
        n00b_plane_t *child = plane->children.data[plane->children.len - 1];
        (void)n00b_plane_remove_child(plane, child);
        destroy_plane_tree(child);
    }

    n00b_widget_detach(plane);
    n00b_plane_destroy(plane);
}

static void
layout_and_render(n00b_canvas_t *canvas, n00b_plane_t *plane, n00b_rect_t bounds)
{
    n00b_widget_layout(plane, bounds);
    n00b_display_scene_mark_all_dirty(canvas);
    n00b_display_scene_rerender_dirty(canvas);
    n00b_canvas_render(canvas);
}

static void
route_mouse(n00b_canvas_t       *canvas,
            n00b_focus_mgr_t    *fm,
            int32_t              x,
            int32_t              y,
            n00b_mouse_button_t  button,
            n00b_mouse_action_t  action)
{
    n00b_event_t event = {
        .type = N00B_EVENT_MOUSE,
        .mouse = {
            .x      = x,
            .y      = y,
            .button = button,
            .action = action,
            .mods   = N00B_MOD_NONE,
        },
    };

    n00b_mouse_route_event(canvas, fm, &event);
}

static void
assert_string_eq(n00b_string_t *actual, const char *expected)
{
    assert(actual != nullptr);
    assert(n00b_unicode_str_eq(actual, n00b_string_from_cstr(expected)));
}

static void
test_text_create_and_api(void)
{
    n00b_string_t *initial = n00b_string_from_cstr("hello");
    n00b_plane_t  *text    = n00b_text_new(initial);
    n00b_text_t   *state   = (n00b_text_t *)text->widget_data;
    n00b_plane_t  *plain   = n00b_new_kargs(n00b_plane_t, plane);
    n00b_string_t *updated = n00b_string_from_cstr("updated");

    assert(text != nullptr);
    assert(text->widget_vtable == &n00b_widget_text);
    assert(state != nullptr);
    assert(state->text == initial);
    assert(state->alignment == N00B_ALIGN_LEFT);
    assert(state->wrap);
    assert(state->hang_indent_cols == 0);
    assert(!state->selectable);
    assert(state->copy_on_release);
    assert(!state->selection.active);
    assert(n00b_text_get_text(text) == initial);
    assert(!n00b_text_has_selection(text));
    assert(n00b_text_get_selection(text) == nullptr);
    assert(!n00b_text_copy_selection(text));
    assert(n00b_text_get_wrapped_line_count(text) == 1);
    assert(!n00b_widget_can_focus(text));

    n00b_text_set_alignment(text, N00B_ALIGN_RIGHT);
    assert(state->alignment == N00B_ALIGN_RIGHT);

    n00b_text_set_hang_indent(text, -5);
    assert(state->hang_indent_cols == 0);
    n00b_text_set_hang_indent(text, 3);
    assert(state->hang_indent_cols == 3);

    n00b_text_set_selectable(text, true);
    assert(state->selectable);
    assert(n00b_widget_can_focus(text));
    n00b_text_set_selectable(text, false);
    assert(!state->selectable);
    assert(!state->selection.active);

    n00b_text_set_text(text, updated);
    assert(n00b_text_get_text(text) == updated);
    assert(!n00b_text_has_selection(text));

    n00b_text_set_text(plain, updated);
    n00b_text_set_alignment(plain, N00B_ALIGN_CENTER);
    n00b_text_set_hang_indent(plain, 2);
    n00b_text_set_selectable(plain, true);
    n00b_text_clear_selection(plain);
    assert(n00b_text_get_text(plain) == nullptr);
    assert(!n00b_text_has_selection(plain));
    assert(n00b_text_get_selection(plain) == nullptr);
    assert(!n00b_text_copy_selection(plain));
    assert(n00b_text_get_wrapped_line_count(plain) == 0);

    n00b_plane_destroy(plain);
    n00b_widget_detach(text);
    n00b_plane_destroy(text);
    printf("  [PASS] text create and api\n");
}

static void
test_text_wrap_count_changes_with_width(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(20, 40);
    n00b_plane_t  *text = n00b_text_new(
        n00b_string_from_cstr("alpha beta gamma delta epsilon zeta"),
        .hang_indent_cols = 2,
        .canvas = canvas);
    int32_t narrow_lines;
    int32_t wide_lines;

    n00b_canvas_add_plane(canvas, text);

    layout_and_render(canvas, text, (n00b_rect_t){ .x = 0, .y = 0, .width = 10, .height = 12 });
    narrow_lines = n00b_text_get_wrapped_line_count(text);
    assert(narrow_lines > 1);
    assert(text->draw_list.count >= 2);
    assert(text->draw_list.cmds[0].type == N00B_DRAW_TEXT);
    assert(text->draw_list.cmds[1].type == N00B_DRAW_TEXT);
    assert(text->draw_list.cmds[1].text.x == 2);

    layout_and_render(canvas, text, (n00b_rect_t){ .x = 0, .y = 0, .width = 24, .height = 12 });
    wide_lines = n00b_text_get_wrapped_line_count(text);
    assert(narrow_lines > wide_lines);

    n00b_canvas_remove_plane(canvas, text);
    destroy_plane_tree(text);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] text wrap count changes with width\n");
}

static void
test_text_selection_extract_and_autocopy(void)
{
    n00b_canvas_t    *canvas = make_stream_canvas(8, 24);
    n00b_plane_t     *text = n00b_text_new(
        n00b_string_from_cstr("Athens text\nwidget"),
        .selectable = true,
        .copy_on_release = true,
        .canvas = canvas);
    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    n00b_string_t    *selection;
    n00b_string_t    *clipboard;

    n00b_canvas_add_plane(canvas, text);
    layout_and_render(canvas, text, (n00b_rect_t){ .x = 0, .y = 0, .width = 16, .height = 4 });

    assert(n00b_stream_backend_get_clipboard(canvas->backend_ctx) == nullptr);

    route_mouse(canvas, fm, 0, 0, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    route_mouse(canvas, fm, 6, 1, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG);
    route_mouse(canvas, fm, 6, 1, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE);

    selection = n00b_text_get_selection(text);
    clipboard = n00b_stream_backend_get_clipboard(canvas->backend_ctx);
    assert_string_eq(selection, "Athens text\nwidget");
    assert_string_eq(clipboard, "Athens text\nwidget");
    assert(n00b_canvas_get_mouse_capture(canvas) == nullptr);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, text);
    destroy_plane_tree(text);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] text selection extract and autocopy\n");
}

static void
test_text_selection_updates_on_move_while_dragging(void)
{
    n00b_canvas_t    *canvas = make_stream_canvas(6, 24);
    n00b_plane_t     *text = n00b_text_new(
        n00b_string_from_cstr("Athens text"),
        .selectable = true,
        .copy_on_release = false,
        .canvas = canvas);
    n00b_focus_mgr_t *fm = n00b_focus_mgr_new(canvas);
    n00b_text_t      *state;

    n00b_canvas_add_plane(canvas, text);
    layout_and_render(canvas, text, (n00b_rect_t){ .x = 0, .y = 0, .width = 16, .height = 2 });
    state = (n00b_text_t *)text->widget_data;

    route_mouse(canvas, fm, 0, 0, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    route_mouse(canvas, fm, 6, 0, N00B_MOUSE_NONE, N00B_MOUSE_MOVE);

    assert(state->selection.active);
    assert(state->selection.start_line == 0);
    assert(state->selection.end_line == 0);
    assert(state->selection.end_col > state->selection.start_col);
    assert(n00b_text_has_selection(text));
    assert(n00b_canvas_get_mouse_capture(canvas) == text);

    n00b_focus_mgr_destroy(fm);
    n00b_canvas_remove_plane(canvas, text);
    destroy_plane_tree(text);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] text selection updates on move while dragging\n");
}

static void
test_text_ctrl_c_copy(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(6, 24);
    n00b_plane_t  *text = n00b_text_new(
        n00b_string_from_cstr("Athens text"),
        .selectable = true,
        .copy_on_release = false,
        .canvas = canvas);
    n00b_text_t   *state;
    n00b_event_t   ctrl_c = {
        .type = N00B_EVENT_KEY,
        .key = {
            .key  = 'c',
            .mods = N00B_MOD_CTRL,
        },
    };
    n00b_string_t *clipboard;

    n00b_canvas_add_plane(canvas, text);
    layout_and_render(canvas, text, (n00b_rect_t){ .x = 0, .y = 0, .width = 16, .height = 2 });

    state = (n00b_text_t *)text->widget_data;
    state->selection.active     = true;
    state->selection.start_line = 0;
    state->selection.start_col  = 0;
    state->selection.end_line   = 0;
    state->selection.end_col    = 6;

    assert(n00b_widget_handle_event(text, &ctrl_c));
    clipboard = n00b_stream_backend_get_clipboard(canvas->backend_ctx);
    assert_string_eq(clipboard, "Athens");

    n00b_canvas_remove_plane(canvas, text);
    destroy_plane_tree(text);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] text ctrl-c copy\n");
}

static void
test_text_inside_scroll_smoke(void)
{
    n00b_canvas_t *canvas = make_stream_canvas(10, 20);
    n00b_plane_t  *text = n00b_text_new(
        n00b_string_from_cstr("line0\nline1\nline2\nline3\nline4"),
        .selectable = true,
        .copy_on_release = true,
        .canvas = canvas);
    n00b_plane_t  *scroll = n00b_scroll_new(text, .canvas = canvas);
    n00b_string_t *clipboard;

    n00b_canvas_add_plane(canvas, scroll);
    layout_and_render(canvas, scroll, (n00b_rect_t){ .x = 0, .y = 0, .width = 8, .height = 3 });

    n00b_scroll_by(scroll, 0, 2);
    n00b_display_scene_rerender_dirty(canvas);
    n00b_canvas_render(canvas);
    assert(n00b_scroll_get_offset_y(scroll) == 2);

    route_mouse(canvas, nullptr, 0, 0, N00B_MOUSE_LEFT, N00B_MOUSE_PRESS);
    route_mouse(canvas, nullptr, 5, 0, N00B_MOUSE_LEFT, N00B_MOUSE_DRAG);
    route_mouse(canvas, nullptr, 5, 0, N00B_MOUSE_LEFT, N00B_MOUSE_RELEASE);

    clipboard = n00b_stream_backend_get_clipboard(canvas->backend_ctx);
    assert(n00b_scroll_get_offset_y(scroll) == 2);
    assert_string_eq(clipboard, "line2");

    n00b_canvas_remove_plane(canvas, scroll);
    destroy_plane_tree(scroll);
    n00b_canvas_destroy(canvas);
    printf("  [PASS] text inside scroll smoke\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running text widget tests...\n");
    test_text_create_and_api();
    test_text_wrap_count_changes_with_width();
    test_text_selection_extract_and_autocopy();
    test_text_selection_updates_on_move_while_dragging();
    test_text_ctrl_c_copy();
    test_text_inside_scroll_smoke();
    printf("Text widget tests passed.\n");

    n00b_shutdown();
    return 0;
}
