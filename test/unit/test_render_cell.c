#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "display/render/cell.h"
#include "text/strings/text_style.h"

// ====================================================================
// Tests
// ====================================================================

static void
test_cell_clear(void)
{
    n00b_rcell_t cell;
    n00b_rcell_set_ascii(&cell, 'X', nullptr);
    assert(cell.flags & N00B_CELL_OCCUPIED);

    n00b_rcell_clear(&cell);
    assert(n00b_rcell_is_empty(&cell));
    assert(cell.grapheme_len == 0);
    assert(cell.display_width == 0);
    assert(cell.style == nullptr);
    printf("  [PASS] cell clear\n");
}

static void
test_cell_set_ascii(void)
{
    n00b_rcell_t cell = {};
    n00b_rcell_set_ascii(&cell, 'A', nullptr);

    assert(cell.grapheme[0] == 'A');
    assert(cell.grapheme[1] == '\0');
    assert(cell.grapheme_len == 1);
    assert(cell.display_width == 1);
    assert(cell.flags & N00B_CELL_OCCUPIED);
    assert(cell.flags & N00B_CELL_DIRTY);
    assert(!n00b_rcell_is_empty(&cell));
    printf("  [PASS] cell set ASCII\n");
}

static void
test_cell_set_grapheme(void)
{
    n00b_rcell_t cell = {};
    // UTF-8 for U+00E9 (é) = 0xC3 0xA9
    const char utf8[] = "\xC3\xA9";
    n00b_rcell_set_grapheme(&cell, utf8, 2, 1, nullptr);

    assert(cell.grapheme_len == 2);
    assert(cell.display_width == 1);
    assert(memcmp(cell.grapheme, utf8, 2) == 0);
    assert(cell.grapheme[2] == '\0');
    printf("  [PASS] cell set grapheme\n");
}

static void
test_cell_set_codepoint_ascii(void)
{
    n00b_rcell_t cell = {};
    n00b_rcell_set_codepoint(&cell, 'Z', 1, nullptr);

    assert(cell.grapheme[0] == 'Z');
    assert(cell.grapheme_len == 1);
    assert(cell.display_width == 1);
    printf("  [PASS] cell set codepoint (ASCII)\n");
}

static void
test_cell_set_codepoint_2byte(void)
{
    n00b_rcell_t cell = {};
    // U+00E9 (é) → 0xC3 0xA9
    n00b_rcell_set_codepoint(&cell, 0x00E9, 1, nullptr);

    assert(cell.grapheme_len == 2);
    assert((unsigned char)cell.grapheme[0] == 0xC3);
    assert((unsigned char)cell.grapheme[1] == 0xA9);
    printf("  [PASS] cell set codepoint (2-byte UTF-8)\n");
}

static void
test_cell_set_codepoint_3byte(void)
{
    n00b_rcell_t cell = {};
    // U+2500 (─) → 0xE2 0x94 0x80
    n00b_rcell_set_codepoint(&cell, 0x2500, 1, nullptr);

    assert(cell.grapheme_len == 3);
    assert((unsigned char)cell.grapheme[0] == 0xE2);
    assert((unsigned char)cell.grapheme[1] == 0x94);
    assert((unsigned char)cell.grapheme[2] == 0x80);
    printf("  [PASS] cell set codepoint (3-byte UTF-8)\n");
}

static void
test_cell_set_codepoint_4byte(void)
{
    n00b_rcell_t cell = {};
    // U+1F600 (😀) → 0xF0 0x9F 0x98 0x80
    n00b_rcell_set_codepoint(&cell, 0x1F600, 2, nullptr);

    assert(cell.grapheme_len == 4);
    assert((unsigned char)cell.grapheme[0] == 0xF0);
    assert((unsigned char)cell.grapheme[1] == 0x9F);
    assert((unsigned char)cell.grapheme[2] == 0x98);
    assert((unsigned char)cell.grapheme[3] == 0x80);
    assert(cell.display_width == 2);
    printf("  [PASS] cell set codepoint (4-byte UTF-8 / wide)\n");
}

static void
test_cell_equal(void)
{
    n00b_rcell_t a = {}, b = {};
    n00b_rcell_set_ascii(&a, 'X', nullptr);
    n00b_rcell_set_ascii(&b, 'X', nullptr);

    assert(n00b_rcell_equal(&a, &b));

    n00b_rcell_set_ascii(&b, 'Y', nullptr);
    assert(!n00b_rcell_equal(&a, &b));
    printf("  [PASS] cell equality\n");
}

static void
test_cell_dirty_flag(void)
{
    n00b_rcell_t cell = {};
    n00b_rcell_set_ascii(&cell, 'D', nullptr);
    assert(cell.flags & N00B_CELL_DIRTY);

    n00b_rcell_mark_clean(&cell);
    assert(!(cell.flags & N00B_CELL_DIRTY));
    assert(cell.flags & N00B_CELL_OCCUPIED);
    printf("  [PASS] dirty flag management\n");
}

static void
test_cell_with_style(void)
{
    n00b_text_style_t style = {
        .bold = N00B_TRI_YES,
    };
    n00b_rcell_t cell = {};
    n00b_rcell_set_ascii(&cell, 'S', &style);

    assert(cell.style == &style);
    assert(cell.style->bold == N00B_TRI_YES);
    printf("  [PASS] cell with style\n");
}

static void
test_cell_size(void)
{
    assert(sizeof(n00b_rcell_t) == 32);
    printf("  [PASS] cell size is 32 bytes\n");
}

// ====================================================================
// Main
// ====================================================================

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running render cell tests...\n");

    test_cell_clear();
    test_cell_set_ascii();
    test_cell_set_grapheme();
    test_cell_set_codepoint_ascii();
    test_cell_set_codepoint_2byte();
    test_cell_set_codepoint_3byte();
    test_cell_set_codepoint_4byte();
    test_cell_equal();
    test_cell_dirty_flag();
    test_cell_with_style();
    test_cell_size();

    printf("All render cell tests passed.\n");
    n00b_shutdown();
    return 0;
}
