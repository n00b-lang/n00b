/*
 * Render cell utility functions.
 */

#include "n00b.h"
#include "render/cell.h"

void
n00b_rcell_set_codepoint(n00b_rcell_t      *cell,
                          n00b_codepoint_t   cp,
                          uint8_t            width,
                          n00b_text_style_t *style)
{
    uint8_t len = 0;

    if (cp < 0x80) {
        cell->grapheme[0] = (char)cp;
        len               = 1;
    }
    else if (cp < 0x800) {
        cell->grapheme[0] = (char)(0xC0 | (cp >> 6));
        cell->grapheme[1] = (char)(0x80 | (cp & 0x3F));
        len               = 2;
    }
    else if (cp < 0x10000) {
        cell->grapheme[0] = (char)(0xE0 | (cp >> 12));
        cell->grapheme[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        cell->grapheme[2] = (char)(0x80 | (cp & 0x3F));
        len               = 3;
    }
    else if (cp < 0x110000) {
        cell->grapheme[0] = (char)(0xF0 | (cp >> 18));
        cell->grapheme[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        cell->grapheme[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        cell->grapheme[3] = (char)(0x80 | (cp & 0x3F));
        len               = 4;
    }
    else {
        // Invalid codepoint; replace with U+FFFD.
        cell->grapheme[0] = (char)0xEF;
        cell->grapheme[1] = (char)0xBF;
        cell->grapheme[2] = (char)0xBD;
        len               = 3;
    }

    cell->grapheme[len] = '\0';
    cell->grapheme_len   = len;
    cell->display_width  = width;
    cell->style          = style;
    cell->flags          = N00B_CELL_OCCUPIED | N00B_CELL_DIRTY;
}
