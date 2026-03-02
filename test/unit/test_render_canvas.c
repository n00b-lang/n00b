#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "adt/array.h"
#include "adt/list.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/canvas.h"
#include "display/render/plane.h"
#include "display/render/draw_cmd.h"
#include "display/render/box.h"
#include "display/render/composite.h"
#include "display/render/types.h"
#include "text/strings/text_style.h"
#include "text/strings/string_ops.h"

// Stream backend test helpers (defined in backend_stream.c).
extern n00b_string_t *n00b_stream_backend_get_buffer(void *ctx);
extern size_t        n00b_stream_backend_get_length(void *ctx);
extern void          n00b_stream_backend_set_size(void *ctx,
                                                   n00b_isize_t rows,
                                                   n00b_isize_t cols);

// ====================================================================
// Tests
// ====================================================================

static void
test_canvas_new_destroy(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
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
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_plane_t  *p = n00b_new_kargs(n00b_plane_t, plane);

    n00b_canvas_add_plane(c, p);
    assert(n00b_list_len(c->planes) == 1);

    bool removed = n00b_canvas_remove_plane(c, p);
    assert(removed);
    assert(n00b_list_len(c->planes) == 0);

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
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // Empty frame should produce only spaces/newlines.

    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render empty\n");
}

static void
test_canvas_render_single_plane(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

    // Set stream backend to small size.
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 5;
    n00b_plane_draw_text(p, 0, 0, n00b_string_from_cstr("Hi"));

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // First two chars of first line should be "Hi".
    assert(buf->data[0] == 'H');
    assert(buf->data[1] == 'i');

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas render single plane\n");
}

static void
test_canvas_z_order(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Back plane (z=0) writes 'B' at (0,0).
    n00b_plane_t *back = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    back->width = 10;
    back->height = 5;
    n00b_plane_draw_glyph(back, 0, 0, 'B');

    // Front plane (z=1) writes 'F' at (0,0).
    n00b_plane_t *front = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    front->width = 10;
    front->height = 5;
    n00b_plane_draw_glyph(front, 0, 0, 'F');

    n00b_canvas_add_plane(c, back);
    n00b_canvas_add_plane(c, front);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Front plane should overwrite back plane.
    assert(buf->data[0] == 'F');

    n00b_plane_destroy(back);
    n00b_plane_destroy(front);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas z-order compositing\n");
}

static void
test_canvas_plane_offset(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 20);
    n00b_canvas_resize(c, 5, 20);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 5;
    p->height = 3;
    n00b_plane_move(p, 3, 1);  // x=3, y=1
    n00b_plane_draw_glyph(p, 0, 0, 'X');

    n00b_canvas_add_plane(c, p);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' should be at frame position (row=1, col=3).
    assert(n00b_unicode_str_contains(buf, r"X"));

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas plane offset\n");
}

static void
test_canvas_invalidate(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);

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
    n00b_plane_t *p1 = n00b_new_kargs(n00b_plane_t, plane, .z = 2);
    p1->width = 10;
    p1->height = 5;
    n00b_plane_t *p2 = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    p2->width = 10;
    p2->height = 5;
    n00b_plane_t *p3 = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    p3->width = 10;
    p3->height = 5;

    n00b_plane_t *planes[] = { p1, p2, p3 };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 3, 1, 1);

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
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane);
    p->width = 10;
    p->height = 5;
    n00b_plane_draw_glyph(p, 0, 0, 'S');

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
    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    assert(n00b_stream_backend_get_length(c->backend_ctx) > 0);

    n00b_plane_destroy(p);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas widget state styling\n");
}

static void
test_canvas_nested_planes(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent at (2,1).
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 15;
    parent->height = 8;
    n00b_plane_move(parent, 2, 1);
    n00b_plane_draw_glyph(parent, 0, 0, 'P');

    // Child at (3,2) relative to parent -> absolute (5,3).
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane);
    child->width = 5;
    child->height = 3;
    n00b_plane_draw_glyph(child, 0, 0, 'C');
    n00b_plane_add_child(parent, child, 3, 2);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    assert(buf->data != nullptr);
    // Both 'P' and 'C' should appear.
    assert(n00b_unicode_str_contains(buf, r"P"));
    assert(n00b_unicode_str_contains(buf, r"C"));

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested planes\n");
}

static void
test_canvas_nested_z_order(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 5, 10);
    n00b_canvas_resize(c, 5, 10);

    // Parent with child that overlaps at same position.
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 10;
    parent->height = 5;
    n00b_plane_draw_glyph(parent, 0, 0, 'P');

    // Child at (0,0), z=1 higher, overwrites parent.
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    child->width = 10;
    child->height = 5;
    n00b_plane_draw_glyph(child, 0, 0, 'C');
    n00b_plane_add_child(parent, child, 0, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // Child (z=1) overwrites parent (z=0) at position (0,0).
    assert(buf->data[0] == 'C');

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas nested z-order\n");
}

static void
test_canvas_child_clipping(void)
{
    n00b_canvas_t *c = n00b_new_kargs(n00b_canvas_t, canvas, .vtable = &n00b_renderer_stream);
    n00b_stream_backend_set_size(c->backend_ctx, 10, 20);
    n00b_canvas_resize(c, 10, 20);

    // Parent plane, positioned at (0,0).
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane);
    parent->width = 10;
    parent->height = 5;

    // Child plane positioned at (8,0) relative to parent.
    // This means the child extends from absolute col 8 to col 17.
    // But parent only goes to col 9 -- child should be clipped.
    n00b_plane_t *child = n00b_new_kargs(n00b_plane_t, plane);
    child->width = 10;
    child->height = 5;
    n00b_plane_draw_glyph(child, 0, 0, 'X');
    n00b_plane_add_child(parent, child, 8, 0);

    n00b_canvas_add_plane(c, parent);
    n00b_canvas_render(c);

    n00b_string_t *buf = n00b_stream_backend_get_buffer(c->backend_ctx);
    // 'X' at child relative (0,0) maps to absolute (8,0).
    // Parent clip is (0,0)-(10,5), so (8,0) is in bounds.
    assert(buf->data != nullptr);

    n00b_plane_destroy(child);
    n00b_plane_destroy(parent);
    n00b_canvas_destroy(c);
    printf("  [PASS] canvas child clipping\n");
}

static void
test_composite_flatten_nested(void)
{
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane, .z = 0);
    parent->width = 20;
    parent->height = 10;
    n00b_plane_move(parent, 5, 5);

    n00b_plane_t *child1 = n00b_new_kargs(n00b_plane_t, plane, .z = 1);
    child1->width = 5;
    child1->height = 3;
    n00b_plane_add_child(parent, child1, 2, 1);

    n00b_plane_t *child2 = n00b_new_kargs(n00b_plane_t, plane, .z = 2);
    child2->width = 5;
    child2->height = 3;
    n00b_plane_add_child(parent, child2, 8, 4);

    n00b_plane_t *planes[] = { parent };

    n00b_array_t(n00b_composite_entry_t) entries =
        n00b_composite_flatten(planes, 1, 1, 1);

    assert(n00b_array_len(entries) == 3); // parent + 2 children.

    // Sorted by z: parent(0), child1(0+1+1=2), child2(0+1+2=3).
    // Children get parent's abs_z + 1 as base, plus their own z offset.
    assert(entries.data[0].abs_z == 0);
    assert(entries.data[0].plane == parent);
    assert(entries.data[0].abs_x == 5);
    assert(entries.data[0].abs_y == 5);

    assert(entries.data[1].abs_z == 2);  // parent(0) + 1 + child1.z(1)
    assert(entries.data[1].plane == child1);
    assert(entries.data[1].abs_x == 7);  // 5 + 2
    assert(entries.data[1].abs_y == 6);  // 5 + 1

    assert(entries.data[2].abs_z == 3);  // parent(0) + 1 + child2.z(2)
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
    test_canvas_nested_planes();
    test_canvas_nested_z_order();
    test_canvas_child_clipping();
    test_composite_flatten_nested();

    printf("All render canvas tests passed.\n");
    n00b_shutdown();
    return 0;
}
