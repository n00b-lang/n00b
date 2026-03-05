#include "n00b.h"
#include "display/hexdump.h"
#include "internal/display/hexdump_contracts.h"

static uint32_t
hex_byte_col(uint32_t ix)
{
    uint32_t n      = ix >> 3;
    uint32_t result = n * N00B_HEX_OCTET_WIDTH;

    if (ix & 0x4) result += N00B_HEX_QUAD_WIDTH;
    if (ix & 0x2) result += N00B_HEX_PAIR_WIDTH;
    if (ix & 0x1) result += N00B_HEX_CHR_WIDTH;

    return result;
}

void
n00b_hexdump_describe_line_regions(const n00b_hexdump_t *hd,
                                    uint32_t nbytes,
                                    n00b_hexdump_line_regions_t *out)
{
    if (!out) {
        return;
    }

    *out = (n00b_hexdump_line_regions_t){};

    if (!hd) {
        return;
    }

    if (nbytes > hd->cpl) {
        nbytes = hd->cpl;
    }

    out->offset_start = 0;
    out->offset_end   = hd->offset_cols;
    out->hex_start    = hd->hex_start;
    out->ascii_start  = hd->ascii_start;

    if (nbytes == 0) {
        out->hex_end   = hd->hex_start;
        out->ascii_end = hd->ascii_start;
        return;
    }

    uint32_t hex_end = hd->hex_start
                     + hex_byte_col(nbytes - 1)
                     + N00B_HEX_BYTE_HEX_COLS;
    if (hex_end > hd->ascii_start) {
        hex_end = hd->ascii_start;
    }
    out->hex_end = hex_end;

    uint32_t ascii_end = hd->ascii_start + nbytes;
    uint32_t line_end  = hd->line_width > 0 ? hd->line_width - 1 : hd->ascii_start;
    if (ascii_end > line_end) {
        ascii_end = line_end;
    }
    out->ascii_end = ascii_end;
}
