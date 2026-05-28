/*
 * test_hexdump.c — Unit tests for the hex dump formatting engine.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "n00b.h"
#include "display/hexdump.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "adt/option.h"

// ============================================================================
// Helpers
// ============================================================================

static n00b_buffer_t *
make_buf(const void *data, size_t len)
{
    return n00b_buffer_from_bytes((char *)data, (int64_t)len);
}

static char *
buf_to_str(n00b_buffer_t *buf)
{
    if (!buf) return nullptr;
    int64_t len  = 0;
    char   *data = n00b_buffer_to_c(buf, &len);
    char   *s    = n00b_alloc_array(char, (size_t)len + 1);
    memcpy(s, data, (size_t)len);
    s[len] = '\0';
    return s;
}

// ============================================================================
// Tests
// ============================================================================

static void
test_calc_cpl(void)
{
    // 80 columns should give 16 bytes per line.
    uint32_t cpl = n00b_hexdump_calc_cpl(80);
    assert(cpl == 16);

    // Very wide terminal should give more bytes.
    uint32_t wide = n00b_hexdump_calc_cpl(200);
    assert(wide >= 32);

    // Power of 2.
    assert((wide & (wide - 1)) == 0);

    // Narrow terminal.
    uint32_t narrow = n00b_hexdump_calc_cpl(40);
    assert(narrow >= 1);
    assert((narrow & (narrow - 1)) == 0);

    printf("  [PASS] calc_cpl\n");
}

static void
test_basic_format(void)
{
    const uint8_t data[] = "Hello, world!";
    n00b_buffer_t *buf = make_buf(data, 13);

    n00b_option_t(n00b_buffer_t *) out_opt = n00b_hexdump_buf(buf, .width = 80);
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    assert(s != nullptr);

    // Should contain the offset "00000000".
    assert(strstr(s, "00000000") != nullptr);
    // Should contain the hex for 'H' (0x48).
    assert(strstr(s, "48") != nullptr);
    // Should contain "Hello" in the ASCII column.
    assert(strstr(s, "Hello") != nullptr);

    printf("  [PASS] basic format\n");
}

static void
test_exact_line(void)
{
    // Exactly 16 bytes — should produce exactly one line.
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = (uint8_t)i;

    n00b_buffer_t *buf = make_buf(data, 16);
    n00b_option_t(n00b_buffer_t *) out_opt = n00b_hexdump_buf(buf, .width = 80);
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    // Should have exactly one newline.
    int nl_count = 0;
    char *p;
    for (p = s; *p; p++) {
        if (*p == '\n') nl_count++;
    }
    assert(nl_count == 1);

    // Bytes are grouped in pairs with no intra-pair space (BYTE_SEP=0),
    // one space between pairs (PAIR_EXTRA=1), one between quads (QUAD_EXTRA=1).
    // So bytes 0-1 are "0001", then space, "0203", then space, "0405", etc.
    assert(strstr(s, "0001") != nullptr);
    assert(strstr(s, "0203") != nullptr);
    // ASCII column should show dots for control chars (no pipe delimiters).
    assert(strstr(s, "................") != nullptr);

    printf("  [PASS] exact line\n");
}

static void
test_multi_line(void)
{
    // 33 bytes at 80 cols (16 cpl) → 3 lines.
    uint8_t data[33];
    for (int i = 0; i < 33; i++) data[i] = (uint8_t)(0x40 + i);

    n00b_buffer_t *buf = make_buf(data, 33);
    n00b_option_t(n00b_buffer_t *) out_opt = n00b_hexdump_buf(buf, .width = 80);
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    int nl_count = 0;
    char *p;
    for (p = s; *p; p++) {
        if (*p == '\n') nl_count++;
    }
    assert(nl_count == 3);

    // First line starts at offset 0.
    assert(strstr(s, "00000000") != nullptr);
    // Second line starts at offset 0x10.
    assert(strstr(s, "00000010") != nullptr);
    // Third line starts at offset 0x20.
    assert(strstr(s, "00000020") != nullptr);

    printf("  [PASS] multi line\n");
}

static void
test_streaming(void)
{
    // Feed data in two chunks, verify streaming works.
    n00b_hexdump_t *hd = n00b_hexdump_new(.width = 80);
    assert(hd != nullptr);

    // First chunk: 10 bytes (partial line).
    uint8_t chunk1[10];
    memset(chunk1, 'A', 10);
    n00b_option_t(n00b_buffer_t *) out1_opt =
        n00b_hexdump_feed(hd, make_buf(chunk1, 10));
    // No complete line yet.
    assert(!n00b_option_is_set(out1_opt));

    // Second chunk: 10 bytes (completes first line + 4 bytes into next).
    uint8_t chunk2[10];
    memset(chunk2, 'B', 10);
    n00b_option_t(n00b_buffer_t *) out2_opt =
        n00b_hexdump_feed(hd, make_buf(chunk2, 10));
    assert(n00b_option_is_set(out2_opt));
    n00b_buffer_t *out2 = n00b_option_get(out2_opt);

    char *s2 = buf_to_str(out2);
    // Should have exactly 1 complete line.
    int nl = 0;
    char *p;
    for (p = s2; *p; p++) {
        if (*p == '\n') nl++;
    }
    assert(nl == 1);

    // Flush the remaining 4 bytes.
    n00b_option_t(n00b_buffer_t *) tail_opt = n00b_hexdump_flush(hd);
    assert(n00b_option_is_set(tail_opt));
    n00b_buffer_t *tail = n00b_option_get(tail_opt);

    char *st = buf_to_str(tail);
    assert(strstr(st, "00000010") != nullptr);

    n00b_hexdump_destroy(hd);
    printf("  [PASS] streaming\n");
}

static void
test_start_offset(void)
{
    const uint8_t data[] = "test";
    n00b_buffer_t *buf = make_buf(data, 4);

    n00b_option_t(n00b_buffer_t *) out_opt =
        n00b_hexdump_buf(buf, .width = 80, .offset = 0x1000);
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    assert(strstr(s, "00001000") != nullptr);

    printf("  [PASS] start offset\n");
}

static void
test_empty_input(void)
{
    n00b_buffer_t *buf = n00b_buffer_empty();
    n00b_option_t(n00b_buffer_t *) out_opt = n00b_hexdump_buf(buf);
    assert(!n00b_option_is_set(out_opt));

    printf("  [PASS] empty input\n");
}

static void
test_null_input(void)
{
    n00b_option_t(n00b_buffer_t *) out_opt =
        n00b_hexdump_feed(nullptr, nullptr);
    assert(!n00b_option_is_set(out_opt));

    n00b_option_t(n00b_buffer_t *) out2_opt = n00b_hexdump_flush(nullptr);
    assert(!n00b_option_is_set(out2_opt));

    printf("  [PASS] null input\n");
}

static void
test_set_width(void)
{
    n00b_hexdump_t *hd = n00b_hexdump_new(.width = 80);
    assert(hd != nullptr);
    assert(hd->cpl == 16);

    // Feed partial data.
    uint8_t data[8];
    memset(data, 0xff, 8);
    n00b_option_t(n00b_buffer_t *) out_opt =
        n00b_hexdump_feed(hd, make_buf(data, 8));
    assert(!n00b_option_is_set(out_opt)); // Not complete yet.

    // Change width — should flush the partial line.
    n00b_option_t(n00b_buffer_t *) flushed_opt =
        n00b_hexdump_set_width(hd, 200);
    assert(n00b_option_is_set(flushed_opt));

    // New cpl should be larger.
    assert(hd->cpl >= 32);

    n00b_hexdump_destroy(hd);
    printf("  [PASS] set width\n");
}

static void
test_ascii_sidebar(void)
{
    // Test that printable chars appear as-is and non-printable as '.'.
    uint8_t data[16] = {
        0x00, 0x01, 0x1f, 0x20,  // control, control, control, space
        'A',  'B',  'C',  'D',   // printable
        0x7e, 0x7f, 0x80, 0xff,  // ~, DEL, high bytes
        '\t', '\n', '\r', 0x1b,  // tab, newline, cr, ESC
    };

    n00b_buffer_t *buf = make_buf(data, 16);
    n00b_option_t(n00b_buffer_t *) out_opt = n00b_hexdump_buf(buf, .width = 80);
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    // ASCII column: no pipe delimiters. Printable chars as-is, others as '.'.
    // Expected: "... ABCD~......."
    assert(strstr(s, "... ABCD~.......") != nullptr);

    printf("  [PASS] ascii sidebar\n");
}

static void
test_reset_preserves_start_offset(void)
{
    // Formatter created with start_offset = 0x2000.
    n00b_hexdump_t *hd = n00b_hexdump_new(.width = 80,
                                            .start_offset = 0x2000);
    assert(hd != nullptr);

    // Feed some data and flush.
    uint8_t data[16];
    memset(data, 0xAA, 16);
    n00b_option_t(n00b_buffer_t *) out_opt =
        n00b_hexdump_feed(hd, make_buf(data, 16));
    assert(n00b_option_is_set(out_opt));
    n00b_buffer_t *out = n00b_option_get(out_opt);

    char *s = buf_to_str(out);
    assert(strstr(s, "00002000") != nullptr);

    // Reset should go back to 0x2000, not 0.
    n00b_hexdump_reset(hd);
    assert(hd->display_offset == 0x2000);

    // Feed again — should show 0x2000 again.
    n00b_option_t(n00b_buffer_t *) out2_opt =
        n00b_hexdump_feed(hd, make_buf(data, 16));
    assert(n00b_option_is_set(out2_opt));
    n00b_buffer_t *out2 = n00b_option_get(out2_opt);

    char *s2 = buf_to_str(out2);
    assert(strstr(s2, "00002000") != nullptr);

    n00b_hexdump_destroy(hd);
    printf("  [PASS] reset preserves start offset\n");
}

static void
test_destroy_null(void)
{
    // Destroying nullptr should be a no-op.
    n00b_hexdump_destroy(nullptr);
    printf("  [PASS] destroy null\n");
}

// ============================================================================
// Main
// ============================================================================

int
main(int argc, char *argv[])
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("test_hexdump:\n");

    test_calc_cpl();
    test_basic_format();
    test_exact_line();
    test_multi_line();
    test_streaming();
    test_start_offset();
    test_empty_input();
    test_null_input();
    test_set_width();
    test_ascii_sidebar();
    test_reset_preserves_start_offset();
    test_destroy_null();

    printf("\n");
    n00b_shutdown();
    return 0;
}
