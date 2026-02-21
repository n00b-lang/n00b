#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "render/box.h"

// ====================================================================
// Tests
// ====================================================================

static void
test_box_props_new_defaults(void)
{
    n00b_box_props_t *box = n00b_box_props_new();

    assert(box != nullptr);
    assert(box->borders == N00B_BORDER_ALL);
    assert(box->alignment == N00B_ALIGN_IGNORE);
    assert(box->overflow == N00B_OVERFLOW_CLIP);
    assert(box->pad_top == 0);
    assert(box->pad_bottom == 0);
    assert(box->pad_left == 0);
    assert(box->pad_right == 0);
    assert(box->border_theme == nullptr);

    printf("  [PASS] box_props_new defaults\n");
}

static void
test_box_props_new_with_kwargs(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme      = &n00b_border_plain,
        .borders    = N00B_BORDER_SIDES,
        .pad_left   = 2,
        .pad_right  = 2,
        .alignment  = N00B_ALIGN_CENTER
    );

    assert(box->border_theme == &n00b_border_plain);
    assert(box->borders == N00B_BORDER_SIDES);
    assert(box->pad_left == 2);
    assert(box->pad_right == 2);
    assert(box->alignment == N00B_ALIGN_CENTER);

    printf("  [PASS] box_props_new with kwargs\n");
}

static void
test_box_insets_no_box(void)
{
    n00b_isize_t t, b, l, r;
    n00b_box_insets(nullptr, &t, &b, &l, &r);

    assert(t == 0 && b == 0 && l == 0 && r == 0);
    printf("  [PASS] box insets with nullptr\n");
}

static void
test_box_insets_all_borders(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme      = &n00b_border_plain,
        .pad_top    = 1,
        .pad_bottom = 1,
        .pad_left   = 2,
        .pad_right  = 2
    );

    n00b_isize_t t, b, l, r;
    n00b_box_insets(box, &t, &b, &l, &r);

    // 1 border + padding
    assert(t == 2);  // 1 border + 1 pad
    assert(b == 2);
    assert(l == 3);  // 1 border + 2 pad
    assert(r == 3);

    printf("  [PASS] box insets with all borders + padding\n");
}

static void
test_box_insets_partial_borders(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme   = &n00b_border_plain,
        .borders = N00B_BORDER_TOP | N00B_BORDER_LEFT
    );

    n00b_isize_t t, b, l, r;
    n00b_box_insets(box, &t, &b, &l, &r);

    assert(t == 1);  // Top border
    assert(b == 0);  // No bottom border
    assert(l == 1);  // Left border
    assert(r == 0);  // No right border

    printf("  [PASS] box insets with partial borders\n");
}

static void
test_box_outer_size(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme       = &n00b_border_plain,
        .pad_left    = 1,
        .pad_right   = 1,
        .pad_top     = 1,
        .pad_bottom  = 1,
        .margin_left = 2,
        .margin_right = 2,
        .margin_top  = 1,
        .margin_bottom = 1
    );

    n00b_isize_t out_rows, out_cols;
    n00b_box_outer_size(box, 10, 20, &out_rows, &out_cols);

    // content 10 + border 2 + padding 2 + margin 2 = 16 rows
    // content 20 + border 2 + padding 2 + margin 4 = 28 cols
    assert(out_rows == 16);
    assert(out_cols == 28);

    printf("  [PASS] box outer size\n");
}

static void
test_box_stamp_full(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme = &n00b_border_plain
    );

    // 6x6 grid to hold a 6x6 box (all borders, no padding, 4x4 content).
    n00b_rcell_t grid[36];
    memset(grid, 0, sizeof(grid));

    n00b_box_stamp(box, grid, 6, 0, 0, 6, 6, nullptr, nullptr);

    // Check corners.
    assert(grid[0 * 6 + 0].flags & N00B_CELL_BORDER);  // upper-left
    assert(grid[0 * 6 + 5].flags & N00B_CELL_BORDER);  // upper-right
    assert(grid[5 * 6 + 0].flags & N00B_CELL_BORDER);  // lower-left
    assert(grid[5 * 6 + 5].flags & N00B_CELL_BORDER);  // lower-right

    // Check top border horizontal.
    assert(grid[0 * 6 + 3].flags & N00B_CELL_BORDER);

    // Check left border vertical.
    assert(grid[2 * 6 + 0].flags & N00B_CELL_BORDER);

    // Check interior (should be untouched).
    assert(!(grid[2 * 6 + 2].flags & N00B_CELL_BORDER));

    printf("  [PASS] box stamp full borders\n");
}

static void
test_box_stamp_with_padding(void)
{
    n00b_box_props_t *box = n00b_box_props_new(
        .theme     = &n00b_border_plain,
        .pad_top   = 1,
        .pad_left  = 1
    );

    // 8x8 grid.
    n00b_rcell_t grid[64];
    memset(grid, 0, sizeof(grid));

    n00b_box_stamp(box, grid, 8, 0, 0, 8, 8, nullptr, nullptr);

    // Row 1, cols 1..6 should be padding (top padding row).
    assert(grid[1 * 8 + 1].flags & N00B_CELL_PADDING);
    assert(grid[1 * 8 + 6].flags & N00B_CELL_PADDING);

    // Col 1, rows 2..6 should be padding (left padding col).
    assert(grid[3 * 8 + 1].flags & N00B_CELL_PADDING);

    printf("  [PASS] box stamp with padding\n");
}

static void
test_border_themes_valid(void)
{
    // Verify predefined themes have non-zero characters.
    assert(n00b_border_plain.horizontal != 0);
    assert(n00b_border_plain.vertical != 0);
    assert(n00b_border_plain.upper_left != 0);

    assert(n00b_border_bold.horizontal != n00b_border_plain.horizontal);
    assert(n00b_border_double.horizontal != n00b_border_plain.horizontal);
    assert(n00b_border_ascii.horizontal == '-');
    assert(n00b_border_ascii.vertical == '|');

    assert(n00b_border_rounded.upper_left == 0x256D);
    assert(n00b_border_rounded.lower_right == 0x256F);

    printf("  [PASS] predefined border themes valid\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render box tests...\n");

    test_box_props_new_defaults();
    test_box_props_new_with_kwargs();
    test_box_insets_no_box();
    test_box_insets_all_borders();
    test_box_insets_partial_borders();
    test_box_outer_size();
    test_box_stamp_full();
    test_box_stamp_with_padding();
    test_border_themes_valid();

    printf("All render box tests passed.\n");
    n00b_shutdown();
    return 0;
}
