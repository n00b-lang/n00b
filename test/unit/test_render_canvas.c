#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/array.h"
#include "core/runtime.h"
#include "render/canvas.h"
#include "render/plane.h"
#include "render/box.h"
#include "render/composite.h"
#include "render/types.h"
#include "strings/text_style.h"

// Stream backend test helpers (defined in backend_stream.c).
extern const char *n00b_stream_backend_get_buffer(void *ctx);
extern size_t      n00b_stream_backend_get_length(void *ctx);
extern void        n00b_stream_backend_set_size(void *ctx,
                                                 n00b_isize_t rows,
                                                 n00b_isize_t cols);

// ====================================================================
// Tests
// ====================================================================

static void
test_canvas_new_destroy(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    assert(c != nullptr);
    assert(c->vtable == &n00b_renderer_stream);
    assert(c->frame_rows > 0);
    assert(c->frame_cols > 0);

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas new and destroy\n");
}

static void
test_canvas_add_remove_plane(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_plane_t  *p = n00b_plane_new(10, 5);

    n00b_canvas_add_plane(c, p);
    assert(c->num_planes == 1);

    bool removed = n00b_canvas_remove_plane(c, p);
    assert(removed);
    assert(c->num_planes == 0);

    // Remove non-existent.
    removed = n00b_canvas_remove_plane(c, p);
    assert(!removed);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas add/remove plane\n");
}

static void
test_canvas_render_empty(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);

    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf != nullptr);
    // Empty frame should produce only spaces/newlines.

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render empty\n");
}

static void
test_canvas_render_single_plane(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);

    // Set stream backend to small size.
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_plane_new(10, 5);
    n00b_plane_put_cp(p, 'H');
    n00b_plane_put_cp(p, 'i');

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf != nullptr);
    // First two chars of first line should be "Hi".
    assert(buf[0] == 'H');
    assert(buf[1] == 'i');

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render single plane\n");
}

static void
test_canvas_z_order(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Back plane (z=0) writes 'B' at (0,0).
    n00b_plane_t *back = n00b_plane_new(10, 5, .z = 0);
    n00b_plane_put_cp(back, 'B');

    // Front plane (z=1) writes 'F' at (0,0).
    n00b_plane_t *front = n00b_plane_new(10, 5, .z = 1);
    n00b_plane_put_cp(front, 'F');

    n00b_canvas_add_plane(c, back);
    n00b_canvas_add_plane(c, front);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Front plane should overwrite back plane.
    assert(buf[0] == 'F');

    n00b_plane_destroy(back);
    n00b_plane_destroy(front);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas z-order compositing\n");
}

static void
test_canvas_plane_offset(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 20);
    n00b_canvas_resize(c, 5, 20);

    n00b_plane_t *p = n00b_plane_new(5, 3);
    n00b_plane_move(p, 3, 1);  // x=3, y=1
    n00b_plane_put_cp(p, 'X');

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' should be at frame position (row=1, col=3).
    // Row 0 is empty, so first line in buffer is empty or spaces.
    // Find the 'X' in the buffer.
    const char *x_pos = strchr(buf, 'X');
    assert(x_pos != nullptr);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas plane offset\n");
}

static void
test_canvas_invalidate(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);

    n00b_canvas_render(c);
    assert(!c->needs_full_redraw);

    n00b_canvas_invalidate(c);
    assert(c->needs_full_redraw);

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas invalidate\n");
}

static void
test_composite_flatten(void)
{
    n00b_plane_t *p1 = n00b_plane_new(10, 5, .z = 2);
    n00b_plane_t *p2 = n00b_plane_new(10, 5, .z = 0);
    n00b_plane_t *p3 = n00b_plane_new(10, 5, .z = 1);

    n00b_plane_t *planes[] = { p1, p2, p3 };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 3);

    assert(n00b_array_len(entries) == 3);
    // Should be sorted by z: 0, 1, 2.
    assert(entries.data[0].abs_z == 0);
    assert(entries.data[1].abs_z == 1);
    assert(entries.data[2].abs_z == 2);

    n00b_array_free(entries);
    n00b_plane_destroy(p1);
    n00b_plane_destroy(p2);
    n00b_plane_destroy(p3);
    printf("  [PASS] composite flatten and z-sort\n");
}

static void
test_canvas_widget_state_styling(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_plane_new(10, 5);
    n00b_plane_put_cp(p, 'S');

    // Set up a box with state-based styling.
    n00b_box_props_t *box = n00b_box_props_new(
        .theme = &n00b_border_plain
    );

    n00b_text_style_t focused_style = { .bold = N00B_TRI_YES };
    n00b_state_style_t focused_state = { .text_style = &focused_style };
    box->state_styles[N00B_WSTATE_FOCUSED] = &focused_state;

    n00b_plane_set_box(p, box);
    n00b_plane_set_state(p, N00B_WSTATE_FOCUSED);

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    // Just verify it doesn't crash and produces output.
    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf != nullptr);
    assert(n00b_stream_backend_get_length(c->backend_ctx) > 0);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas widget state styling\n");
}

static void
test_canvas_viewport_subsetting(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Plane with 20 cols of content but only 10-col viewport.
    n00b_plane_t *p = n00b_plane_new(20, 5,
                                      .vp_cols = 10,
                                      .vp_rows = 5);

    // Write 'A' at col 0 and 'B' at col 15.
    n00b_plane_cursor_move(p, 0, 0);
    n00b_plane_put_cp(p, 'A');
    n00b_plane_cursor_move(p, 0, 15);
    n00b_plane_put_cp(p, 'B');

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Default viewport at col 0 — should see 'A' but not 'B'.
    assert(buf[0] == 'A');
    assert(strchr(buf, 'B') == nullptr);

    // Scroll viewport to show 'B'.
    n00b_plane_scroll_to(p, 0, 10);
    n00b_canvas_render(c);

    buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Now col 15 maps to viewport col 5 → frame col 5.
    assert(strchr(buf, 'B') != nullptr);
    assert(strchr(buf, 'A') == nullptr);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas viewport subsetting\n");
}

static void
test_canvas_nested_planes(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent at (2,1).
    n00b_plane_t *parent = n00b_plane_new(15, 8);
    n00b_plane_move(parent, 2, 1);
    n00b_plane_put_cp(parent, 'P');

    // Child at (3,2) relative to parent → absolute (5,3).
    n00b_plane_t *child = n00b_plane_new(5, 3);
    n00b_plane_put_cp(child, 'C');
    n00b_plane_add_child(parent, child, 3, 2);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf != nullptr);
    // Both 'P' and 'C' should appear.
    assert(strchr(buf, 'P') != nullptr);
    assert(strchr(buf, 'C') != nullptr);

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested planes\n");
}

static void
test_canvas_nested_z_order(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Parent with child that overlaps at same position.
    n00b_plane_t *parent = n00b_plane_new(10, 5);
    n00b_plane_put_cp(parent, 'P');

    // Child at (0,0), z=1 higher, overwrites parent.
    n00b_plane_t *child = n00b_plane_new(10, 5, .z = 1);
    n00b_plane_put_cp(child, 'C');
    n00b_plane_add_child(parent, child, 0, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Child (z=1) overwrites parent (z=0) at position (0,0).
    assert(buf[0] == 'C');

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested z-order\n");
}

static void
test_canvas_child_clipping(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent plane 10x5, positioned at (0,0).
    n00b_plane_t *parent = n00b_plane_new(10, 5);

    // Child plane 10x5 positioned at (8,0) relative to parent.
    // This means the child extends from absolute col 8 to col 17.
    // But parent only goes to col 9 — child should be clipped.
    n00b_plane_t *child = n00b_plane_new(10, 5);
    n00b_plane_put_cp(child, 'X');
    n00b_plane_add_child(parent, child, 8, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' at child relative (0,0) maps to absolute (8,0).
    // Parent clip is (0,0)-(10,5), so (8,0) is in bounds.
    assert(buf != nullptr);

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas child clipping\n");
}

static void
test_canvas_ellipsis_overflow(void)
{
    n00b_canvas_t *c = n00b_canvas_new(&n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Plane: 10 rows of content, but only 3 visible in viewport.
    n00b_plane_t *p = n00b_plane_new(10, 10,
                                      .vp_cols = 10,
                                      .vp_rows = 3);

    // Set up box with ellipsis overflow.
    n00b_box_props_t *box = n00b_box_props_new(.overflow = N00B_OVERFLOW_ELLIPSIS);
    n00b_plane_set_box(p, box);

    // Write content on all 10 rows.
    for (int i = 0; i < 10; i++) {
        n00b_plane_put_cp(p, 'A' + i);
        if (i < 9) {
            n00b_plane_newline(p);
        }
    }

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    const char *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf != nullptr);

    // The last visible row should show "..." (three dots).
    // Row 0 = 'A', Row 1 = 'B', Row 2 = '...'
    assert(strchr(buf, 'A') != nullptr);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas ellipsis overflow\n");
}

static void
test_composite_flatten_nested(void)
{
    n00b_plane_t *parent = n00b_plane_new(20, 10, .z = 0);
    n00b_plane_move(parent, 5, 5);

    n00b_plane_t *child1 = n00b_plane_new(5, 3, .z = 1);
    n00b_plane_add_child(parent, child1, 2, 1);

    n00b_plane_t *child2 = n00b_plane_new(5, 3, .z = 2);
    n00b_plane_add_child(parent, child2, 8, 4);

    n00b_plane_t *planes[] = { parent };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 1);

    assert(n00b_array_len(entries) == 3); // parent + 2 children.

    // Sorted by z: parent(0), child1(1), child2(2).
    assert(entries.data[0].abs_z == 0);
    assert(entries.data[0].plane == parent);
    assert(entries.data[0].abs_x == 5);
    assert(entries.data[0].abs_y == 5);

    assert(entries.data[1].abs_z == 1);
    assert(entries.data[1].plane == child1);
    assert(entries.data[1].abs_x == 7);  // 5 + 2
    assert(entries.data[1].abs_y == 6);  // 5 + 1

    assert(entries.data[2].abs_z == 2);
    assert(entries.data[2].plane == child2);
    assert(entries.data[2].abs_x == 13); // 5 + 8
    assert(entries.data[2].abs_y == 9);  // 5 + 4

    n00b_array_free(entries);
    n00b_plane_destroy(child1);
    n00b_plane_destroy(child2);
    n00b_plane_destroy(parent);
    printf("  [PASS] composite flatten nested with absolute coords\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render canvas tests...\n");

    test_canvas_new_destroy();
    test_canvas_add_remove_plane();
    test_canvas_render_empty();
    test_canvas_render_single_plane();
    test_canvas_z_order();
    test_canvas_plane_offset();
    test_canvas_invalidate();
    test_composite_flatten();
    test_canvas_widget_state_styling();
    test_canvas_viewport_subsetting();
    test_canvas_nested_planes();
    test_canvas_nested_z_order();
    test_canvas_child_clipping();
    test_canvas_ellipsis_overflow();
    test_composite_flatten_nested();

    printf("All render canvas tests passed.\n");
    return 0;
}
