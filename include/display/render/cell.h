/**
 * @file cell.h
 * @brief Render cell type and utility functions.
 *
 * A render cell (`n00b_rcell_t`) is the fundamental unit of the
 * compositing surface.  Each cell holds a single grapheme cluster
 * (up to 16 bytes inline), a style pointer, display width, and flags.
 *
 * ### Memory layout
 *
 * At 32 bytes per cell, a 200x50 terminal uses ~312 KB per plane.
 * The 16-byte inline grapheme avoids pointer indirection for the
 * vast majority of characters.
 *
 * ### Related modules
 *
 * - `render/plane.h` — plane that owns a grid of cells
 * - `render/composite.h` — compositing reads cells from planes
 * - `strings/text_style.h` — style type referenced by cells
 */
#pragma once

#include "n00b.h"
#include "display/render/types.h"

// ====================================================================
// Cell flags
// ====================================================================

/**
 * @brief Status flags for a render cell.
 */
typedef enum : uint8_t {
    N00B_CELL_EMPTY     = 0,
    N00B_CELL_OCCUPIED  = 1 << 0, /**< Cell contains a grapheme. */
    N00B_CELL_WIDE_CONT = 1 << 1, /**< Continuation column of a wide char. */
    N00B_CELL_DIRTY     = 1 << 2, /**< Modified since last render. */
    N00B_CELL_BORDER    = 1 << 3, /**< Auto-generated border decoration. */
    N00B_CELL_PADDING   = 1 << 4, /**< Auto-generated padding fill. */
} n00b_cell_flags_t;

// ====================================================================
// Render cell
// ====================================================================

/**
 * @brief A single cell in the render grid.
 *
 * Stores a UTF-8 grapheme cluster inline (up to 16 bytes, NUL-terminated),
 * a pointer to the resolved style, the display width in columns, and
 * status flags.
 *
 * @pre `grapheme_len <= 15` (one byte reserved for NUL).
 * @post `display_width` is 0 (empty/continuation), 1, or 2.
 */
typedef struct n00b_rcell_t {
    char               grapheme[16]; /**< UTF-8 grapheme cluster, NUL-terminated. */
    n00b_text_style_t *style;        /**< Resolved style (nullptr = default). */
    uint8_t            grapheme_len; /**< Byte length in `grapheme[]`. */
    uint8_t            display_width; /**< Column width: 0, 1, or 2. */
    n00b_cell_flags_t  flags;
    uint8_t            _pad[5];      /**< Pad to 32 bytes. */
} n00b_rcell_t;

// ====================================================================
// Cell utilities
// ====================================================================

/**
 * @brief Clear a cell to empty state.
 * @param cell Cell to clear.
 * @post  Cell is empty with no style.
 */
static inline void
n00b_rcell_clear(n00b_rcell_t *cell)
{
    memset(cell, 0, sizeof(n00b_rcell_t));
}

/**
 * @brief Set a cell to a single ASCII character.
 * @param cell  Cell to set.
 * @param ch    ASCII character.
 * @param style Style to apply (nullptr = default).
 */
static inline void
n00b_rcell_set_ascii(n00b_rcell_t *cell, char ch, n00b_text_style_t *style)
{
    cell->grapheme[0]  = ch;
    cell->grapheme[1]  = '\0';
    cell->grapheme_len = 1;
    cell->display_width = 1;
    cell->style        = style;
    cell->flags        = N00B_CELL_OCCUPIED | N00B_CELL_DIRTY;
}

/**
 * @brief Set a cell to a UTF-8 grapheme cluster.
 * @param cell     Cell to set.
 * @param utf8     Pointer to UTF-8 bytes.
 * @param byte_len Number of bytes (must be <= 15).
 * @param width    Display width in columns (1 or 2).
 * @param style    Style to apply (nullptr = default).
 *
 * @pre `byte_len <= 15`.
 */
static inline void
n00b_rcell_set_grapheme(n00b_rcell_t    *cell,
                         const char      *utf8,
                         uint8_t          byte_len,
                         uint8_t          width,
                         n00b_text_style_t *style)
{
    assert(byte_len <= 15);
    memcpy(cell->grapheme, utf8, byte_len);
    cell->grapheme[byte_len] = '\0';
    cell->grapheme_len  = byte_len;
    cell->display_width = width;
    cell->style         = style;
    cell->flags         = N00B_CELL_OCCUPIED | N00B_CELL_DIRTY;
}

/**
 * @brief Set a cell from a Unicode codepoint.
 * @param cell  Cell to set.
 * @param cp    Unicode codepoint.
 * @param width Display width in columns (1 or 2).
 * @param style Style to apply (nullptr = default).
 */
void n00b_rcell_set_codepoint(n00b_rcell_t      *cell,
                               n00b_codepoint_t   cp,
                               uint8_t            width,
                               n00b_text_style_t *style);

/**
 * @brief Test whether two cells have identical content and style.
 * @param a First cell.
 * @param b Second cell.
 * @return  true if cells match (ignoring dirty flag).
 */
static inline bool
n00b_rcell_equal(const n00b_rcell_t *a, const n00b_rcell_t *b)
{
    if (a->grapheme_len != b->grapheme_len) {
        return false;
    }
    if (a->display_width != b->display_width) {
        return false;
    }
    if (a->style != b->style) {
        return false;
    }
    if (a->grapheme_len > 0 && memcmp(a->grapheme, b->grapheme, a->grapheme_len) != 0) {
        return false;
    }
    return true;
}

/**
 * @brief Clear the dirty flag on a cell.
 * @param cell Cell to clean.
 */
static inline void
n00b_rcell_mark_clean(n00b_rcell_t *cell)
{
    cell->flags = (n00b_cell_flags_t)(cell->flags & ~N00B_CELL_DIRTY);
}

/**
 * @brief Check whether a cell is empty.
 * @param cell Cell to check.
 * @return     true if the cell has no content.
 */
static inline bool
n00b_rcell_is_empty(const n00b_rcell_t *cell)
{
    return (cell->flags & N00B_CELL_OCCUPIED) == 0;
}
