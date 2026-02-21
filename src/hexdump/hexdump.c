/*
 * hexdump.c — Hex dump formatting engine.
 *
 * Formats byte data into text lines with offset + hex + ASCII columns.
 * Supports streaming (partial lines buffered between feeds) and
 * dynamic width calculation with power-of-2 aligned bytes-per-line.
 *
 * Layout math ported from slop's xform_hexdump.c (h4x0r engine).
 */

#include "hexdump/hexdump.h"
#include "core/alloc.h"
#include "core/buffer.h"

#include <assert.h>
#include <string.h>

// ============================================================================
// Internal helpers
// ============================================================================

static const char hex_lc[16] = {
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

// Round down to the largest power of 2 <= n.
static inline uint32_t
align_pow2_floor(uint32_t v)
{
    if (v == 0) return 0;
    return 1u << (31 - __builtin_clz(v));
}

// Column offset of the nth byte within the hex section.
// Uses bitwise decomposition for the hierarchical grouping:
//   bits 3+  → full octets
//   bit  2   → quad within octet
//   bit  1   → pair within quad
//   bit  0   → byte within pair
static inline uint32_t
hex_byte_col(uint32_t ix)
{
    uint32_t n      = ix >> 3;
    uint32_t result = n * N00B_HEX_OCTET_WIDTH;

    if (ix & 0x4) result += N00B_HEX_QUAD_WIDTH;
    if (ix & 0x2) result += N00B_HEX_PAIR_WIDTH;
    if (ix & 0x1) result += N00B_HEX_CHR_WIDTH;

    return result;
}

// Padding after byte `ix` (0-based).
static inline uint8_t
hex_pad_after(uint32_t ix)
{
    uint8_t result = N00B_HEX_BYTE_SEP;
    ix++;
    if (!(ix % 2)) result += N00B_HEX_PAIR_EXTRA;
    if (!(ix % 4)) result += N00B_HEX_QUAD_EXTRA;
    if (!(ix % 8)) result += N00B_HEX_OCTET_EXTRA;
    return result;
}

// Width of the hex column for `cpl` bytes.
static inline uint32_t
hex_col_width(uint32_t cpl)
{
    return N00B_HEX_OCTET_WIDTH * (cpl / 8);
}

// Offset column width: 8 hex digits (4 bytes) normally,
// 16 hex digits (8 bytes) when cpl >= 16.
static inline uint32_t
offset_width(uint32_t cpl)
{
    uint32_t bytes = sizeof(uint64_t); // 8 hex digits
    if (cpl >= N00B_HEX_LARGE_CPL) {
        bytes *= 2; // 16 hex digits
    }
    return bytes;
}

// Base column (where hex data starts).
static inline uint32_t
base_col(uint32_t cpl)
{
    return offset_width(cpl) + N00B_HEX_OFFSET_PAD;
}

// Column where the ASCII sidebar starts.
static inline uint32_t
ascii_col(uint32_t cpl)
{
    return base_col(cpl) + hex_col_width(cpl) + N00B_HEX_ASCII_PAD;
}

// Total line width including newline.
static inline uint32_t
total_line_width(uint32_t cpl)
{
    return ascii_col(cpl) + cpl + 1; // +1 for newline
}

// Recalculate layout fields from width.
static void
recalc_layout(n00b_hexdump_t *hd)
{
    hd->cpl         = n00b_hexdump_calc_cpl(hd->width);
    hd->offset_cols = offset_width(hd->cpl);
    hd->hex_start   = base_col(hd->cpl);
    hd->ascii_start = ascii_col(hd->cpl);
    hd->line_width  = total_line_width(hd->cpl);
}

// Write the offset into `line` at position 0.
static void
write_offset(char *line, uint32_t offset_cols, int64_t offset)
{
    for (int i = (int)offset_cols - 1; i >= 0; i--) {
        line[i] = hex_lc[offset & 0xf];
        offset >>= 4;
    }
}

// Format one complete line of `cpl` bytes from `data` into `out`.
// `nbytes` may be < cpl for the last (partial) line.
void
n00b_hexdump_format_line(n00b_hexdump_t *hd, const uint8_t *data,
                          uint32_t nbytes, char *out)
{
    assert(hd);
    assert(data);
    assert(out);
    assert(nbytes <= hd->cpl);

    uint32_t lw = hd->line_width;

    // Fill with spaces first.
    memset(out, ' ', lw);

    // Offset.
    write_offset(out, hd->offset_cols, hd->display_offset);

    // Hex bytes.
    for (uint32_t i = 0; i < nbytes; i++) {
        uint32_t col = hd->hex_start + hex_byte_col(i);
        out[col]     = hex_lc[data[i] >> 4];
        out[col + 1] = hex_lc[data[i] & 0xf];
    }

    // ASCII sidebar.
    uint32_t astart = hd->ascii_start;
    for (uint32_t i = 0; i < nbytes; i++) {
        uint8_t c = data[i];
        out[astart + i] = (c >= 0x20 && c <= 0x7e) ? (char)c : '.';
    }

    // Newline.
    out[lw - 1] = '\n';
}

// ============================================================================
// Public API
// ============================================================================

uint32_t
n00b_hexdump_calc_cpl(uint32_t width)
{
    if (width < N00B_HEX_MIN_WIDTH) {
        width = N00B_HEX_MIN_WIDTH;
    }

    // Available columns after subtracting fixed overhead.
    // The max offset is 16 hex digits; we always reserve that much
    // so the layout doesn't shift as offsets grow.
    uint32_t avail = width
        - (N00B_HEX_MAX_OFFSET * 2
           + N00B_HEX_OFFSET_PAD
           + N00B_HEX_ASCII_PAD);

    // Try to fit as many full octets as possible.
    int64_t sets = (int64_t)avail / N00B_HEX_CHRS_PER_OCTET;
    if (sets > 0) {
        return 8 * align_pow2_floor((uint32_t)sets);
    }

    // Too narrow for even one octet; try quads.
    avail += 4; // reclaim offset bytes for small mode
    sets = (int64_t)avail / N00B_HEX_CHRS_PER_QUAD;
    if (sets > 0) {
        return 4 * align_pow2_floor((uint32_t)sets);
    }

    return 2;
}

n00b_hexdump_t *
n00b_hexdump_new()
    _kargs {
        uint32_t          width        = 0;
        int64_t           start_offset = 0;
        bool              sequential   = true;
        n00b_allocator_t *allocator    = nullptr;
    }
{
    n00b_hexdump_t *hd = n00b_alloc_with_opts(n00b_hexdump_t, &(n00b_alloc_opts_t){.allocator = allocator});
    if (!hd) return nullptr;

    hd->allocator      = allocator;
    hd->width          = (width > 0) ? width : N00B_HEX_DEFAULT_WIDTH;
    hd->display_offset = start_offset;
    hd->start_offset   = start_offset;
    hd->sequential     = sequential;
    hd->line_offset    = 0;

    recalc_layout(hd);

    hd->line_buf = n00b_alloc_array_with_opts(char, hd->cpl, &(n00b_alloc_opts_t){.allocator = allocator});

    return hd;
}

void
n00b_hexdump_destroy(n00b_hexdump_t *hd)
{
    if (!hd) return;

    if (hd->line_buf) {
        n00b_free(hd->line_buf);
    }

    n00b_free(hd);
}

n00b_buffer_t *
n00b_hexdump_feed(n00b_hexdump_t *hd, n00b_buffer_t *buf)
{
    if (!hd || !buf) return nullptr;

    int64_t  in_len  = 0;
    char    *in_data = n00b_buffer_to_c(buf, &in_len);
    if (in_len <= 0) return nullptr;

    const uint8_t *p   = (const uint8_t *)in_data;
    const uint8_t *end = p + in_len;

    // Working buffer for one formatted line.
    char *line_out = n00b_alloc_array_with_opts(char, hd->line_width + 1,
                                                &(n00b_alloc_opts_t){.allocator = hd->allocator});

    n00b_buffer_t *out = nullptr;

    while (p < end) {
        // Fill partial line buffer.
        uint32_t need  = hd->cpl - hd->line_offset;
        uint32_t avail = (uint32_t)(end - p);
        uint32_t take  = (avail < need) ? avail : need;

        memcpy(hd->line_buf + hd->line_offset, p, take);
        hd->line_offset += take;
        p += take;

        if (hd->line_offset == hd->cpl) {
            // Complete line — format and append.
            n00b_hexdump_format_line(hd, hd->line_buf, hd->cpl, line_out);
            n00b_buffer_t *piece = n00b_buffer_from_bytes(
                line_out, (int64_t)hd->line_width);

            if (!out) {
                out = piece;
            }
            else {
                out = n00b_buffer_add(out, piece);
            }

            hd->display_offset += hd->cpl;
            hd->line_offset = 0;
        }
    }

    return out;
}

n00b_buffer_t *
n00b_hexdump_flush(n00b_hexdump_t *hd)
{
    if (!hd || hd->line_offset == 0) return nullptr;

    char *line_out = n00b_alloc_array_with_opts(char, hd->line_width + 1,
                                                &(n00b_alloc_opts_t){.allocator = hd->allocator});
    n00b_hexdump_format_line(hd, hd->line_buf, hd->line_offset, line_out);

    n00b_buffer_t *out = n00b_buffer_from_bytes(
        line_out, (int64_t)hd->line_width);

    hd->display_offset += hd->line_offset;
    hd->line_offset = 0;

    return out;
}

void
n00b_hexdump_reset(n00b_hexdump_t *hd)
{
    if (!hd) return;
    hd->line_offset    = 0;
    hd->display_offset = hd->start_offset;
}

n00b_buffer_t *
n00b_hexdump_set_width(n00b_hexdump_t *hd, uint32_t width)
{
    if (!hd) return nullptr;

    // Flush partial line before changing layout.
    n00b_buffer_t *flushed = n00b_hexdump_flush(hd);

    hd->width = (width > 0) ? width : N00B_HEX_DEFAULT_WIDTH;
    recalc_layout(hd);

    // Reallocate line buffer for new cpl.
    n00b_free(hd->line_buf);
    hd->line_buf = n00b_alloc_array_with_opts(char, hd->cpl, &(n00b_alloc_opts_t){.allocator = hd->allocator});

    return flushed;
}

n00b_buffer_t *
n00b_hexdump_buf(n00b_buffer_t *buf)
    _kargs {
        uint32_t width  = 0;
        int64_t  offset = 0;
    }
{
    if (!buf || n00b_buffer_len(buf) == 0) return nullptr;

    n00b_hexdump_t *hd = n00b_hexdump_new(
        .width = width, .start_offset = offset, .sequential = true);
    if (!hd) return nullptr;

    n00b_buffer_t *lines = n00b_hexdump_feed(hd, buf);
    n00b_buffer_t *tail  = n00b_hexdump_flush(hd);

    n00b_hexdump_destroy(hd);

    if (!lines && !tail) return nullptr;
    if (!lines) return tail;
    if (!tail) return lines;

    return n00b_buffer_add(lines, tail);
}
