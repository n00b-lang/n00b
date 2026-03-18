#include "n00b.h"
#include "display/hexdump.h"
#include "internal/display/hexdump_contracts.h"

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
    out->ascii_start  = hd->ascii_start;

    if (nbytes == 0) {
        out->ascii_end = hd->ascii_start;
        return;
    }

    uint32_t ascii_end = hd->ascii_start + nbytes;
    uint32_t line_end  = hd->line_width > 0 ? hd->line_width - 1 : hd->ascii_start;
    if (ascii_end > line_end) {
        ascii_end = line_end;
    }
    out->ascii_end = ascii_end;
}
