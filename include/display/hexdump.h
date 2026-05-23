/**
 * @file hexdump.h
 * @brief Hex dump formatting engine.
 *
 * Formats byte data into hex dump output with configurable layout,
 * hierarchical byte grouping (pair / quad / octet), 64-bit offsets,
 * and an ASCII sidebar. Supports sequential streaming across multiple
 * input buffers.
 *
 * ### Layout
 *
 * Bytes are grouped in a 2-4-8 hierarchy with increasing inter-group
 * spacing. The offset column auto-sizes to 8 or 16 hex digits based
 * on the bytes-per-line count. The bytes-per-line count is
 * power-of-2 aligned and computed from the terminal width.
 *
 * ### Usage
 *
 * ```c
 * n00b_hexdump_t *hd = n00b_hexdump_new(.width = 80);
 * n00b_option_t(n00b_buffer_t *) out  = n00b_hexdump_feed(hd, data_buf);
 * n00b_option_t(n00b_buffer_t *) tail = n00b_hexdump_flush(hd);
 * ```
 */
#pragma once

#include "core/buffer.h"
#include "adt/option.h"

// ============================================================================
// Layout constants (ported from slop/h4x0r)
// ============================================================================

/** @brief Inter-byte spacing within a pair (none). */
#define N00B_HEX_BYTE_SEP     0
/** @brief Extra space between byte pairs. */
#define N00B_HEX_PAIR_EXTRA   1
/** @brief Extra space between quads (4-byte groups). */
#define N00B_HEX_QUAD_EXTRA   1
/** @brief Extra space between octets (8-byte groups). */
#define N00B_HEX_OCTET_EXTRA  0

/** @brief Hex columns per byte (2 hex digits). */
#define N00B_HEX_BYTE_HEX_COLS    2
/** @brief ASCII columns per byte (1 character). */
#define N00B_HEX_BYTE_ASCII_COLS  1

/** @brief Width of one byte in hex (hex digits + separator). */
#define N00B_HEX_CHR_WIDTH   (N00B_HEX_BYTE_HEX_COLS + N00B_HEX_BYTE_SEP)
/** @brief Width of one pair (2 bytes) in hex. */
#define N00B_HEX_PAIR_WIDTH  (2 * N00B_HEX_CHR_WIDTH + N00B_HEX_PAIR_EXTRA)
/** @brief Width of one quad (4 bytes) in hex. */
#define N00B_HEX_QUAD_WIDTH  (2 * N00B_HEX_PAIR_WIDTH + N00B_HEX_QUAD_EXTRA)
/** @brief Width of one octet (8 bytes) in hex. */
#define N00B_HEX_OCTET_WIDTH (2 * N00B_HEX_QUAD_WIDTH + N00B_HEX_OCTET_EXTRA)

/** @brief Total columns per byte (hex + ASCII + separators). */
#define N00B_HEX_BASE_CHRS_PER_BYTE \
    ((N00B_HEX_BYTE_HEX_COLS + N00B_HEX_BYTE_ASCII_COLS) + N00B_HEX_BYTE_SEP)
/** @brief Total columns per pair (hex + ASCII + separators). */
#define N00B_HEX_CHRS_PER_PAIR \
    (N00B_HEX_BASE_CHRS_PER_BYTE * 2 + N00B_HEX_PAIR_EXTRA)
/** @brief Total columns per quad (hex + ASCII + separators). */
#define N00B_HEX_CHRS_PER_QUAD \
    (N00B_HEX_CHRS_PER_PAIR * 2 + N00B_HEX_QUAD_EXTRA)
/** @brief Total columns per octet (hex + ASCII + separators). */
#define N00B_HEX_CHRS_PER_OCTET \
    (N00B_HEX_CHRS_PER_QUAD * 2 + N00B_HEX_OCTET_EXTRA)

/** @brief Right-padding after offset column. */
#define N00B_HEX_OFFSET_PAD   1
/** @brief Left-padding before ASCII column. */
#define N00B_HEX_ASCII_PAD    1

/** @brief Maximum offset field width in bytes (8 bytes = 16 hex digits). */
#define N00B_HEX_MAX_OFFSET   8
/** @brief Threshold: use large (16-digit) offsets when cpl >= this. */
#define N00B_HEX_LARGE_CPL    16

/** @brief Default terminal width. */
#define N00B_HEX_DEFAULT_WIDTH 138
/** @brief Minimum supported terminal width. */
#define N00B_HEX_MIN_WIDTH     40

// ============================================================================
// State
// ============================================================================

/**
 * @brief Hex dump formatting state.
 *
 * Maintained across calls to `n00b_hexdump_feed` for streaming.
 */
typedef struct n00b_hexdump {
    uint8_t  *line_buf;      /**< Partial-line accumulator. */
    uint32_t  line_offset;   /**< Bytes accumulated in current line. */
    int64_t   display_offset;/**< Display address for next line. */
    int64_t   start_offset;  /**< Original starting address (for reset). */
    uint32_t  width;         /**< Terminal width in columns. */
    bool      sequential;    /**< Keep offsets incrementing across feeds. */
    n00b_allocator_t *allocator; /**< Allocator (nullptr = runtime default). */

    // --- Internal layout (do not modify directly) ---
    uint32_t  cpl;           /**< Bytes per hex line (power of 2). */
    uint32_t  offset_cols;   /**< Offset column width (8 or 16). */
    uint32_t  hex_start;     /**< Column where hex data begins. */
    uint32_t  ascii_start;   /**< Column where ASCII sidebar begins. */
    uint32_t  line_width;    /**< Total columns per output line. */
} n00b_hexdump_t;

// ============================================================================
// API
// ============================================================================

/**
 * @brief Create a new hex dump formatter.
 *
 * @kw width          Terminal width in columns (0 = default 138).
 * @kw start_offset   Starting display address.
 * @kw sequential     If true, offsets keep incrementing across feeds.
 * @kw allocator      Allocator (nullptr = runtime default).
 *
 * @return Caller-owned formatter; free with `n00b_hexdump_destroy`.
 */
extern n00b_hexdump_t *
n00b_hexdump_new() _kargs {
    uint32_t          width        = 0;
    int64_t           start_offset = 0;
    bool              sequential   = true;
    n00b_allocator_t *allocator    = nullptr;
};

/**
 * @brief Destroy a hex dump formatter and free all owned resources.
 * @param hd Formatter to destroy (nullptr is a no-op).
 */
extern void
n00b_hexdump_destroy(n00b_hexdump_t *hd);

/**
 * @brief Feed a buffer of bytes into the hex dump formatter.
 *
 * Complete lines are emitted into the returned buffer. Partial
 * lines are buffered internally until the next feed or flush.
 *
 * @param hd  Hex dump state.
 * @param buf Input data.
 * @return    Some(caller-owned buffer with complete formatted lines),
 *            or None if no complete lines were produced.
 */
extern n00b_option_t(n00b_buffer_t *)
n00b_hexdump_feed(n00b_hexdump_t *hd, n00b_buffer_t *buf);

/**
 * @brief Flush any buffered partial line.
 *
 * @param hd Hex dump state.
 * @return   Some(caller-owned buffer with the final line), or
 *           None if nothing was buffered.
 */
extern n00b_option_t(n00b_buffer_t *)
n00b_hexdump_flush(n00b_hexdump_t *hd);

/**
 * @brief Reset the formatter to its initial state.
 *
 * Clears any buffered partial line and resets the display offset
 * to the original `start_offset` passed at construction.
 *
 * @param hd Hex dump state.
 */
extern void
n00b_hexdump_reset(n00b_hexdump_t *hd);

/**
 * @brief Change the terminal width.
 *
 * Flushes any partial line before recalculating layout.
 *
 * @param hd    Hex dump state.
 * @param width New terminal width (0 = default).
 * @return      Some(flushed partial line), or None.
 */
extern n00b_option_t(n00b_buffer_t *)
n00b_hexdump_set_width(n00b_hexdump_t *hd, uint32_t width);

/**
 * @brief Compute bytes-per-line for a given terminal width.
 *
 * @param width Terminal width in columns.
 * @return      Bytes per line (power-of-2 aligned, minimum 2).
 */
extern uint32_t
n00b_hexdump_calc_cpl(uint32_t width);

/**
 * @brief Format a buffer as a complete hex dump.
 *
 * Convenience: creates a temporary formatter, feeds, flushes, and
 * destroys it internally.
 *
 * @param buf   Data to format.
 * @kw width    Terminal width (0 = default).
 * @kw offset   Starting display address.
 * @return      Some(caller-owned buffer with the complete hex dump),
 *              or None if @p buf is empty.
 */
extern n00b_option_t(n00b_buffer_t *)
n00b_hexdump_buf(n00b_buffer_t *buf) _kargs {
    uint32_t width   = 0;
    int64_t  offset  = 0;
}
;

/**
 * @brief Format one line of hex dump output into a caller-supplied buffer.
 *
 * Writes `hd->line_width` bytes into @p out: offset, hex columns,
 * ASCII sidebar, and a trailing newline.  @p nbytes may be less
 * than `hd->cpl` for partial (last) lines.
 *
 * @param hd     Hex dump state (provides layout parameters).
 * @param data   Byte data to format.
 * @param nbytes Number of bytes to format (1 .. hd->cpl).
 * @param out    Output buffer, must be at least `hd->line_width` bytes.
 *
 * @pre  @p hd is initialized via `n00b_hexdump_new`.
 * @pre  @p out has at least `hd->line_width` bytes of space.
 * @pre  `nbytes <= hd->cpl`.
 *
 * @note This is a low-level function for direct line formatting.
 *       Prefer `n00b_hexdump_feed` / `n00b_hexdump_flush` for
 *       typical usage.
 */
extern void
n00b_hexdump_format_line(n00b_hexdump_t *hd, const uint8_t *data,
                          uint32_t nbytes, char *out);
