#pragma once

#include "n00b.h"
#include "display/hexdump.h"

typedef struct n00b_hexdump_line_regions_t {
    uint32_t offset_start;
    uint32_t offset_end;
    uint32_t hex_start;
    uint32_t hex_end;
    uint32_t ascii_start;
    uint32_t ascii_end;
} n00b_hexdump_line_regions_t;

extern void n00b_hexdump_describe_line_regions(const n00b_hexdump_t *hd,
                                                uint32_t nbytes,
                                                n00b_hexdump_line_regions_t *out);
