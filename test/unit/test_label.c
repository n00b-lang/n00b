/*
 * Unit tests for the label widget.
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
#include "display/render/types.h"
#include "display/widget.h"
#include "display/widgets/label.h"
#include "text/strings/text_style.h"
#include "text/strings/string_style.h"
#include "text/unicode/properties.h"

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

static char
cell_char(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    n00b_option_t(n00b_const_rcell_ptr_t) opt = n00b_plane_get_cell(p, row, col);
    if (!n00b_option_is_set(opt)) {
        return '\0';
    }
    const n00b_rcell_t *cell = n00b_option_get(opt);
    if (cell->grapheme_len == 0) {
        return '\0';
    }
    return cell->grapheme[0];
}

static const n00b_rcell_t *
get_cell(n00b_plane_t *p, n00b_isize_t row, n00b_isize_t col)
{
    n00b_option_t(n00b_const_rcell_ptr_t) opt = n00b_plane_get_cell(p, row, col);
    assert(n00b_option_is_set(opt));
    return n00b_option_get(opt);
}

// -------------------------------------------------------------------
// Test 1: Plain text label
// -------------------------------------------------------------------

static void
test_label_plain_text(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Hello");
    n00b_plane_t  *lbl  = n00b_label_new(text);

    assert(lbl != nullptr);
    assert(lbl->widget_vtable == &n00b_widget_label);

    // Check that the plane is auto-sized to text width.
    assert(lbl->total_cols == 5);
    assert(lbl->total_rows == 1);

    // Verify cell content.
    assert(cell_char(lbl, 0, 0) == 'H');
    assert(cell_char(lbl, 0, 1) == 'e');
    assert(cell_char(lbl, 0, 2) == 'l');
    assert(cell_char(lbl, 0, 3) == 'l');
    assert(cell_char(lbl, 0, 4) == 'o');

    printf("  [PASS] plain text label\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 2: Styled text label
// -------------------------------------------------------------------

static void
test_label_styled_text(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Bold");

    n00b_text_style_t bold_style = {
        .bold = N00B_TRI_YES,
        .fg_rgb = n00b_color_make(0xFF0000),
    };

    // Allocate a persistent copy of the style for the string.
    n00b_text_style_t *style_ptr = n00b_alloc(n00b_text_style_t);
    *style_ptr = bold_style;

    text = n00b_str_set_base_style(text, style_ptr);

    n00b_plane_t *lbl = n00b_label_new(text);
    assert(lbl != nullptr);

    // Verify cell content exists.
    assert(cell_char(lbl, 0, 0) == 'B');

    // Verify style was applied to cells.
    const n00b_rcell_t *cell = get_cell(lbl, 0, 0);
    assert(cell->style != nullptr);
    assert(cell->style->bold == N00B_TRI_YES);
    assert(n00b_color_is_set(cell->style->fg_rgb));
    assert(n00b_color_rgb(cell->style->fg_rgb) == 0xFF0000);

    printf("  [PASS] styled text label\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 3: CENTER alignment
// -------------------------------------------------------------------

static void
test_label_center_align(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Hi");
    n00b_plane_t  *lbl  = n00b_label_new(text,
                                          .cols      = 10,
                                          .alignment = N00B_ALIGN_CENTER);

    assert(lbl != nullptr);
    assert(lbl->total_cols == 10);

    // "Hi" is 2 chars, content is 10 cols, offset should be 4.
    // Cells 0-3 should be empty, 4-5 should be 'H','i'.
    assert(cell_char(lbl, 0, 0) == '\0');
    assert(cell_char(lbl, 0, 3) == '\0');
    assert(cell_char(lbl, 0, 4) == 'H');
    assert(cell_char(lbl, 0, 5) == 'i');
    assert(cell_char(lbl, 0, 6) == '\0');

    printf("  [PASS] center alignment\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 4: RIGHT alignment
// -------------------------------------------------------------------

static void
test_label_right_align(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Hi");
    n00b_plane_t  *lbl  = n00b_label_new(text,
                                          .cols      = 10,
                                          .alignment = N00B_ALIGN_RIGHT);

    assert(lbl != nullptr);

    // "Hi" at right: offset = 10 - 2 = 8.
    assert(cell_char(lbl, 0, 7) == '\0');
    assert(cell_char(lbl, 0, 8) == 'H');
    assert(cell_char(lbl, 0, 9) == 'i');

    printf("  [PASS] right alignment\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 5: Wrap mode
// -------------------------------------------------------------------

static void
test_label_wrap(void)
{
    // 10 chars into 5 columns should produce 2 rows.
    n00b_string_t *text = n00b_string_from_cstr("ABCDEFGHIJ");
    n00b_plane_t  *lbl  = n00b_label_new(text,
                                          .cols = 5,
                                          .wrap = true);

    assert(lbl != nullptr);
    assert(lbl->total_rows == 2);
    assert(lbl->total_cols == 5);

    // First row: ABCDE
    assert(cell_char(lbl, 0, 0) == 'A');
    assert(cell_char(lbl, 0, 4) == 'E');

    // Second row: FGHIJ
    assert(cell_char(lbl, 1, 0) == 'F');
    assert(cell_char(lbl, 1, 4) == 'J');

    printf("  [PASS] wrap mode\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 6: set_text updates content
// -------------------------------------------------------------------

static void
test_label_set_text(void)
{
    n00b_string_t *text1 = n00b_string_from_cstr("AAAA");
    n00b_plane_t  *lbl   = n00b_label_new(text1, .cols = 10);

    assert(cell_char(lbl, 0, 0) == 'A');

    n00b_string_t *text2 = n00b_string_from_cstr("BBBB");
    n00b_label_set_text(lbl, text2);

    // Should now show B's.
    assert(cell_char(lbl, 0, 0) == 'B');
    assert(cell_char(lbl, 0, 3) == 'B');
    // Old content should be cleared.
    assert(cell_char(lbl, 0, 4) == '\0');

    // get_text should return the new string.
    assert(n00b_label_get_text(lbl) == text2);

    printf("  [PASS] set_text\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Test 7: Widget measure
// -------------------------------------------------------------------

static void
test_label_measure(void)
{
    n00b_string_t *text = n00b_string_from_cstr("Hello World");
    n00b_plane_t  *lbl  = n00b_label_new(text);

    n00b_isize_t pref_cols, pref_rows, min_cols, min_rows;
    n00b_widget_measure(lbl, &pref_cols, &pref_rows, &min_cols, &min_rows);

    assert(pref_cols == 11); // "Hello World" = 11 display columns
    assert(pref_rows == 1);
    assert(min_cols == 1);
    assert(min_rows == 1);

    printf("  [PASS] widget measure\n");
    n00b_plane_destroy(lbl);
}

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running label widget tests...\n");

    test_label_plain_text();
    test_label_styled_text();
    test_label_center_align();
    test_label_right_align();
    test_label_wrap();
    test_label_set_text();
    test_label_measure();

    printf("All label tests passed.\n");

    n00b_shutdown();
    return 0;
}
