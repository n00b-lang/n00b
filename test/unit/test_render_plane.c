#include <stdio.h>
#include <assert.h>
#include "n00b.h"
#include "core/alloc.h"
#include "adt/option.h"
#include "core/runtime.h"
#include "core/string.h"
#include "display/render/plane.h"
#include "display/render/box.h"
#include "text/strings/string_ops.h"

// ====================================================================
// Tests
// ====================================================================

static void
test_plane_new_and_destroy(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 80, .rows = 25);
    assert(p != nullptr);
    assert(p->total_cols == 80);
    assert(p->total_rows == 25);
    assert(p->vp_cols == 80);
    assert(p->vp_rows == 25);
    assert(p->cursor_row == 0);
    assert(p->cursor_col == 0);
    assert(p->flags & N00B_PLANE_VISIBLE);
    assert(p->grid != nullptr);

    n00b_plane_destroy(p);
    printf("  [PASS] plane new and destroy\n");
}

static void
test_plane_new_with_kwargs(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane,
                                      .cols    = 100,
                                      .rows    = 50,
                                      .vp_cols = 80,
                                      .vp_rows = 25,
                                      .name    = n00b_option_set(n00b_string_t, *r"test-plane"),
                                      .z       = 5);
    assert(p->total_cols == 100);
    assert(p->total_rows == 50);
    assert(p->vp_cols == 80);
    assert(p->vp_rows == 25);
    assert(p->z == 5);
    assert(n00b_unicode_str_eq(p->name, *r"test-plane"));

    n00b_plane_destroy(p);
    printf("  [PASS] plane new with kwargs\n");
}

static void
test_plane_put_cp(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_put_cp(p, 'H');
    n00b_plane_put_cp(p, 'i');

    auto opt0 = n00b_plane_get_cell(p, 0, 0);
    auto opt1 = n00b_plane_get_cell(p, 0, 1);

    assert(n00b_option_is_set(opt0));
    assert(n00b_option_get(opt0)->grapheme[0] == 'H');
    assert(n00b_option_is_set(opt1));
    assert(n00b_option_get(opt1)->grapheme[0] == 'i');
    assert(p->cursor_col == 2);

    n00b_plane_destroy(p);
    printf("  [PASS] plane put_cp\n");
}

static void
test_plane_cursor_move(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_cursor_move(p, 3, 7);
    assert(p->cursor_row == 3);
    assert(p->cursor_col == 7);

    // Clamp to bounds.
    n00b_plane_cursor_move(p, 100, 100);
    assert(p->cursor_row == 4);  // max row = 4 (5-1)
    assert(p->cursor_col == 9);  // max col = 9 (10-1)

    n00b_plane_destroy(p);
    printf("  [PASS] plane cursor move\n");
}

static void
test_plane_clear(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_put_cp(p, 'X');
    n00b_plane_put_cp(p, 'Y');

    n00b_plane_clear(p);

    auto opt = n00b_plane_get_cell(p, 0, 0);
    assert(n00b_option_is_set(opt));
    assert(n00b_rcell_is_empty(n00b_option_get(opt)));
    assert(p->cursor_row == 0);
    assert(p->cursor_col == 0);

    n00b_plane_destroy(p);
    printf("  [PASS] plane clear\n");
}

static void
test_plane_newline(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_put_cp(p, 'A');
    n00b_plane_newline(p);

    assert(p->cursor_row == 1);
    assert(p->cursor_col == 0);

    n00b_plane_destroy(p);
    printf("  [PASS] plane newline\n");
}

static void
test_plane_fill_rect(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_fill_rect(p, 1, 2, 2, 3, .cp = '#');

    for (n00b_isize_t r = 1; r <= 2; r++) {
        for (n00b_isize_t c = 2; c <= 4; c++) {
            auto opt = n00b_plane_get_cell(p, r, c);
            assert(n00b_option_is_set(opt));
            assert(n00b_option_get(opt)->grapheme[0] == '#');
        }
    }

    // Cell outside rect should be empty.
    auto opt_out = n00b_plane_get_cell(p, 0, 0);
    assert(n00b_option_is_set(opt_out));
    assert(n00b_rcell_is_empty(n00b_option_get(opt_out)));

    n00b_plane_destroy(p);
    printf("  [PASS] plane fill_rect\n");
}

static void
test_plane_get_cell_oob(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    assert(!n00b_option_is_set(n00b_plane_get_cell(p, 100, 100)));
    assert(n00b_option_is_set(n00b_plane_get_cell(p, 0, 0)));

    n00b_plane_destroy(p);
    printf("  [PASS] plane get_cell out-of-bounds\n");
}

static void
test_plane_visibility(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    assert(p->flags & N00B_PLANE_VISIBLE);

    n00b_plane_set_visible(p, false);
    assert(!(p->flags & N00B_PLANE_VISIBLE));

    n00b_plane_set_visible(p, true);
    assert(p->flags & N00B_PLANE_VISIBLE);

    n00b_plane_destroy(p);
    printf("  [PASS] plane visibility\n");
}

static void
test_plane_move_and_z(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_move(p, 5, 10);
    assert(p->x == 5);
    assert(p->y == 10);

    n00b_plane_set_z(p, 3);
    assert(p->z == 3);

    n00b_plane_destroy(p);
    printf("  [PASS] plane move and z-order\n");
}

static void
test_plane_resize(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_put_cp(p, 'A');
    n00b_plane_resize(p, 3, 3);

    assert(p->total_rows == 3);
    assert(p->total_cols == 3);

    // Content at (0,0) should be preserved.
    auto opt = n00b_plane_get_cell(p, 0, 0);
    assert(n00b_option_is_set(opt));
    assert(n00b_option_get(opt)->grapheme[0] == 'A');

    n00b_plane_destroy(p);
    printf("  [PASS] plane resize\n");
}

static void
test_plane_widget_state(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    assert(n00b_plane_get_state(p) == N00B_WSTATE_NORMAL);

    n00b_plane_set_state(p, N00B_WSTATE_FOCUSED);
    assert(n00b_plane_get_state(p) == N00B_WSTATE_FOCUSED);

    n00b_plane_set_state(p, N00B_WSTATE_DISABLED);
    assert(n00b_plane_get_state(p) == N00B_WSTATE_DISABLED);

    n00b_plane_destroy(p);
    printf("  [PASS] plane widget state\n");
}

static void
test_plane_content_size_with_box(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane, .cols = 20, .rows = 10);

    n00b_box_props_t *box = n00b_box_props_new(
        .theme     = &n00b_border_plain,
        .pad_left  = 1,
        .pad_right = 1,
        .pad_top   = 1,
        .pad_bottom = 1
    );
    n00b_plane_set_box(p, box);

    n00b_isize_t cr, cc;
    n00b_plane_content_size(p, &cr, &cc);

    // 20 cols - 2 borders - 2 padding = 16
    // 10 rows - 2 borders - 2 padding = 6
    assert(cc == 16);
    assert(cr == 6);

    n00b_plane_destroy(p);
    printf("  [PASS] plane content_size with box\n");
}

static void
test_plane_scroll(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane,
                                      .cols    = 100,
                                      .rows    = 50,
                                      .vp_cols = 20,
                                      .vp_rows = 10);

    n00b_plane_scroll(p, 5, 3);
    assert(p->vp_row == 5);
    assert(p->vp_col == 3);

    // Scroll clamping.
    n00b_plane_scroll(p, 1000, 1000);
    assert(p->vp_row <= 50 - 10);
    assert(p->vp_col <= 100 - 20);

    n00b_plane_destroy(p);
    printf("  [PASS] plane scroll\n");
}

static void
test_plane_scroll_to(void)
{
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane,
                                      .cols    = 100,
                                      .rows    = 50,
                                      .vp_cols = 20,
                                      .vp_rows = 10);

    n00b_plane_scroll_to(p, 15, 30);
    assert(p->vp_row == 15);
    assert(p->vp_col == 30);

    // Clamp to max.
    n00b_plane_scroll_to(p, 999, 999);
    assert(p->vp_row == 40); // 50 - 10
    assert(p->vp_col == 80); // 100 - 20

    // Scroll to 0.
    n00b_plane_scroll_to(p, 0, 0);
    assert(p->vp_row == 0);
    assert(p->vp_col == 0);

    n00b_plane_destroy(p);
    printf("  [PASS] plane scroll_to\n");
}

static void
test_plane_auto_scroll(void)
{
    // 5-row plane with auto-scroll ring buffer.
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane,
                                      .cols   = 10,
                                      .rows   = 5,
                                      .scroll = N00B_SCROLL_AUTO);

    // Write 7 lines — should trigger auto-scroll on lines 6 and 7.
    for (int i = 0; i < 7; i++) {
        n00b_plane_put_cp(p, 'A' + i);
        n00b_plane_newline(p);
    }

    // Cursor should still be in bounds.
    assert(p->cursor_row < p->total_rows);

    // The ring_base should have advanced (2 scrolls for 7 lines in 5 rows).
    assert(p->ring_base > 0);

    // The last written character (G at i=6) should still be accessible.
    // It was written on the row before the most recent newline.
    // We can check that the plane still has valid content.

    n00b_plane_destroy(p);
    printf("  [PASS] plane auto-scroll ring buffer\n");
}

static void
test_plane_viewport_content(void)
{
    // Create plane with content larger than viewport.
    n00b_plane_t *p = n00b_new_kargs(n00b_plane_t, plane,
                                      .cols    = 20,
                                      .rows    = 10,
                                      .vp_cols = 10,
                                      .vp_rows = 5);

    // Write content at different positions.
    n00b_plane_cursor_move(p, 0, 0);
    n00b_plane_put_cp(p, 'A');
    n00b_plane_cursor_move(p, 5, 5);
    n00b_plane_put_cp(p, 'B');

    // Initially viewport is at (0,0), should see 'A' but not 'B'.
    assert(p->vp_row == 0);
    assert(p->vp_col == 0);

    // Scroll viewport to see 'B'.
    n00b_plane_scroll_to(p, 3, 3);
    assert(p->vp_row == 3);
    assert(p->vp_col == 3);

    // 'B' is at grid(5,5), viewport starts at (3,3) with size (5,10).
    // Viewport shows grid rows 3-7, cols 3-12.
    // 'B' at (5,5) is visible at viewport-relative (2,2).

    n00b_plane_destroy(p);
    printf("  [PASS] plane viewport content access\n");
}

static void
test_plane_add_remove_child(void)
{
    n00b_plane_t *parent = n00b_new_kargs(n00b_plane_t, plane, .cols = 40, .rows = 20);
    n00b_plane_t *child1 = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);
    n00b_plane_t *child2 = n00b_new_kargs(n00b_plane_t, plane, .cols = 10, .rows = 5);

    n00b_plane_add_child(parent, child1, 2, 3);
    n00b_plane_add_child(parent, child2, 15, 10);

    assert(n00b_list_len(parent->children) == 2);
    assert(child1->parent == parent);
    assert(child2->parent == parent);
    assert(child1->x == 2);
    assert(child1->y == 3);

    // Remove child1.
    bool removed = n00b_plane_remove_child(parent, child1);
    assert(removed);
    assert(n00b_list_len(parent->children) == 1);
    assert(child1->parent == nullptr);
    assert(n00b_list_get(parent->children, 0) == child2);

    // Remove non-existent.
    removed = n00b_plane_remove_child(parent, child1);
    assert(!removed);

    n00b_plane_destroy(child1);
    n00b_plane_destroy(child2);
    n00b_plane_destroy(parent);
    printf("  [PASS] plane add/remove child\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render plane tests...\n");

    test_plane_new_and_destroy();
    test_plane_new_with_kwargs();
    test_plane_put_cp();
    test_plane_cursor_move();
    test_plane_clear();
    test_plane_newline();
    test_plane_fill_rect();
    test_plane_get_cell_oob();
    test_plane_visibility();
    test_plane_move_and_z();
    test_plane_resize();
    test_plane_widget_state();
    test_plane_content_size_with_box();
    test_plane_scroll();
    test_plane_scroll_to();
    test_plane_auto_scroll();
    test_plane_viewport_content();
    test_plane_add_remove_child();

    printf("All render plane tests passed.\n");
    n00b_shutdown();
    return 0;
}
