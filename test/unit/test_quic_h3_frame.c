/*
 * test_quic_h3_frame.c — RFC 9114 H3 frame encoder + decoder.
 *
 * Coverage:
 *   1. Round-trip: SETTINGS, HEADERS, DATA, GOAWAY, CANCEL_PUSH,
 *      MAX_PUSH_ID.  Encode → decode → assert equality of body.
 *   2. Truncated input — feed bytes one at a time; parser must
 *      return NEED_MORE_DATA until the full frame is present, then
 *      OK.
 *   3. Oversized — body length > cap → FRAME_TOO_LARGE.
 *   4. Malformed — invalid varint length, reserved frame type
 *      (must error), grease frame type (must round-trip OK; caller
 *      ignores).
 *   5. SETTINGS body — multiple identifiers, duplicate identifier
 *      rejection, unknown identifier ignored, reserved-HTTP-2
 *      identifier rejected.
 *   6. HEADERS body via QPACK end-to-end — encode :method/:scheme/
 *      :authority/:path with QPACK, wrap in HEADERS frame, decode,
 *      verify.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/alloc.h"
#include "net/quic/quic_types.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "net/quic/qpack.h"

#define ASSERT(cond)                                                          \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "ASSERT FAILED at %s:%d: %s\n",                   \
                    __FILE__, __LINE__, #cond);                               \
            abort();                                                          \
        }                                                                     \
    } while (0)

static int total_subtests = 0;
#define PASS(name)                                                            \
    do { total_subtests++; printf("  [PASS] %s\n", (name)); } while (0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

static void
fresh_buffer(n00b_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    n00b_buffer_init(b, .length = 0);
}

/* ============================================================================
 * 1. Round-trip primitive frames
 * ============================================================================ */

static void
test_roundtrip_data_frame(void)
{
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    const char *body = "hello, h3";

    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA,
                                                 (const uint8_t *)body,
                                                 strlen(body));
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_DATA);
    ASSERT(frame.body_len == strlen(body));
    ASSERT(memcmp(frame.body, body, frame.body_len) == 0);
    ASSERT(frame.consumed == (size_t)buf.byte_len);

    PASS("DATA frame round-trip");
}

static void
test_roundtrip_empty_data(void)
{
    n00b_buffer_t buf;
    fresh_buffer(&buf);

    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA,
                                                 nullptr, 0);
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_DATA);
    ASSERT(frame.body_len == 0);
    ASSERT(frame.body == nullptr);

    PASS("empty DATA frame round-trip");
}

static void
test_roundtrip_settings_frame(void)
{
    /* Build a SETTINGS body, then wrap. */
    n00b_buffer_t body;
    fresh_buffer(&body);
    n00b_result_t(bool) br = n00b_h3_settings_emit_body(&body,
        /*qpack_max_table_capacity*/ 4096,
        /*qpack_blocked_streams   */ 8,
        /*max_field_section_size  */ 65536);
    ASSERT(n00b_result_is_ok(br));
    ASSERT(body.byte_len > 0);

    n00b_buffer_t frame_buf;
    fresh_buffer(&frame_buf);
    n00b_result_t(bool) er = n00b_h3_frame_emit(&frame_buf,
        N00B_H3_FRAME_SETTINGS,
        (const uint8_t *)body.data, (size_t)body.byte_len);
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&frame_buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_SETTINGS);
    ASSERT(frame.body_len == (size_t)body.byte_len);

    n00b_h3_settings_t s;
    n00b_result_t(bool) sr = n00b_h3_settings_parse(frame.body,
                                                     frame.body_len, &s);
    ASSERT(n00b_result_is_ok(sr));
    ASSERT(s.received);
    ASSERT(s.qpack_max_table_capacity == 4096);
    ASSERT(s.qpack_blocked_streams == 8);
    ASSERT(s.max_field_section_size == 65536);

    PASS("SETTINGS frame round-trip");
}

static void
test_roundtrip_headers_frame(void)
{
    /* Build a small QPACK field section, then wrap as HEADERS. */
    n00b_qpack_encoder_t *enc = n00b_qpack_encoder_new(0, 0);
    n00b_qpack_decoder_t *dec = n00b_qpack_decoder_new(0, 0);

    n00b_qpack_field_t fields[] = {
        { .name = (uint8_t *)":method", .name_len = 7,
          .value = (uint8_t *)"GET", .value_len = 3 },
        { .name = (uint8_t *)":scheme", .name_len = 7,
          .value = (uint8_t *)"https", .value_len = 5 },
        { .name = (uint8_t *)":authority", .name_len = 10,
          .value = (uint8_t *)"localhost:443", .value_len = 13 },
        { .name = (uint8_t *)":path", .name_len = 5,
          .value = (uint8_t *)"/", .value_len = 1 },
    };

    n00b_buffer_t section;
    fresh_buffer(&section);
    n00b_result_t(bool) qr = n00b_qpack_encode(enc, /*stream_id*/ 0,
                                                 fields, 4, &section, nullptr);
    ASSERT(n00b_result_is_ok(qr));
    ASSERT(section.byte_len > 0);

    n00b_buffer_t frame_buf;
    fresh_buffer(&frame_buf);
    n00b_result_t(bool) er = n00b_h3_frame_emit(&frame_buf,
        N00B_H3_FRAME_HEADERS,
        (const uint8_t *)section.data, (size_t)section.byte_len);
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&frame_buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_HEADERS);
    ASSERT(frame.body_len == (size_t)section.byte_len);

    /* Decode the QPACK section. */
    n00b_qpack_field_t out[8];
    size_t n_out = 0;
    n00b_result_t(bool) dr = n00b_qpack_decode(dec, 0, frame.body,
                                                 frame.body_len, out, 8,
                                                 &n_out, nullptr);
    ASSERT(n00b_result_is_ok(dr));
    ASSERT(n_out == 4);
    ASSERT(out[0].name_len == 7 && memcmp(out[0].name, ":method", 7) == 0);
    ASSERT(out[0].value_len == 3 && memcmp(out[0].value, "GET", 3) == 0);
    ASSERT(out[3].name_len == 5 && memcmp(out[3].name, ":path", 5) == 0);

    n00b_qpack_encoder_close(enc);
    n00b_qpack_decoder_close(dec);
    PASS("HEADERS frame round-trip (QPACK end-to-end)");
}

static void
test_roundtrip_goaway_frame(void)
{
    /* GOAWAY body is a single varint (largest stream id processed). */
    n00b_buffer_t buf;
    fresh_buffer(&buf);

    /* Body: varint(0x1234) = 0x52 0x34. */
    uint8_t body[] = { 0x52, 0x34 };
    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_GOAWAY,
                                                 body, sizeof(body));
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_GOAWAY);
    ASSERT(frame.body_len == 2);

    PASS("GOAWAY frame round-trip");
}

static void
test_roundtrip_cancel_push(void)
{
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    uint8_t body[] = { 0x07 };
    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_CANCEL_PUSH,
                                                 body, sizeof(body));
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_CANCEL_PUSH);

    PASS("CANCEL_PUSH frame round-trip");
}

static void
test_roundtrip_max_push_id(void)
{
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    uint8_t body[] = { 0x40, 0x10 };  /* varint(16) */
    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_MAX_PUSH_ID,
                                                 body, sizeof(body));
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_MAX_PUSH_ID);

    PASS("MAX_PUSH_ID frame round-trip");
}

/* ============================================================================
 * 2. Truncated input — incremental parsing
 * ============================================================================ */

static void
test_truncated_incremental(void)
{
    /* Build a complete frame, then feed it to the parser one byte at
     * a time; the parser must return NEED_MORE_DATA until the last
     * byte arrives, then OK. */
    n00b_buffer_t complete;
    fresh_buffer(&complete);
    const char *body = "incremental-parse-test";
    n00b_result_t(bool) er = n00b_h3_frame_emit(&complete, N00B_H3_FRAME_DATA,
                                                 (const uint8_t *)body,
                                                 strlen(body));
    ASSERT(n00b_result_is_ok(er));

    size_t total = (size_t)complete.byte_len;
    ASSERT(total > 1);

    /* Feed 0..total-1 bytes — must always return NEED_MORE_DATA. */
    for (size_t k = 0; k < total; k++) {
        n00b_h3_frame_t frame;
        n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
            (const uint8_t *)complete.data, k, &frame);
        ASSERT(n00b_result_is_err(pr));
        ASSERT(n00b_result_get_err(pr) == N00B_QUIC_ERR_NEED_MORE_DATA);
    }
    /* The full slice must succeed. */
    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(
        (const uint8_t *)complete.data, total, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == N00B_H3_FRAME_DATA);
    ASSERT(frame.body_len == strlen(body));

    PASS("truncated input returns NEED_MORE_DATA, full input OK");
}

/* ============================================================================
 * 3. Oversized — body length > cap
 * ============================================================================ */

static void
test_oversized_rejected(void)
{
    /* Forge a frame whose advertised length is just above the
     * default cap (16 MiB).  Don't allocate the actual body — the
     * parser must reject before reading anything beyond the header. */

    /* type 0x00 (DATA) + varint(17 MiB).
     * 17 MiB = 0x01100000 = 17825792.  That fits in a 4-byte varint
     * (high 2 bits = 10).  4-byte varint encodes:
     *   first byte = 0x80 | (val >> 24)
     *   then 3 bytes of (val >> 16) & 0xff, etc.
     * value = 17825792 = 0x01100000.  Bytes: 0x80 | 0x01 = 0x81,
     *   then 0x10, 0x00, 0x00. */
    uint8_t hdr[] = { 0x00, 0x81, 0x10, 0x00, 0x00 };

    /* Default max is 16 MiB; we're at 17 MiB → reject. */
    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(hdr, sizeof(hdr),
                                                        &frame);
    ASSERT(n00b_result_is_err(pr));
    ASSERT(n00b_result_get_err(pr) == N00B_QUIC_ERR_FRAME_TOO_LARGE);

    PASS("17 MiB frame body rejected (FRAME_TOO_LARGE)");
}

static void
test_custom_cap_rejected(void)
{
    /* Caller sets max_size = 100; submit a frame with body_len = 200. */
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    /* Build a 200-byte body. */
    uint8_t body[200];
    memset(body, 0xab, sizeof(body));
    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA,
                                                 body, sizeof(body),
                                                 .max_size = 1000);
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame,
                                                  .max_size = 100);
    ASSERT(n00b_result_is_err(pr));
    ASSERT(n00b_result_get_err(pr) == N00B_QUIC_ERR_FRAME_TOO_LARGE);

    /* Same frame parses fine when the cap is bumped. */
    n00b_result_t(bool) pr2 = n00b_h3_frame_parse(&buf, 0, &frame,
                                                   .max_size = 1000);
    ASSERT(n00b_result_is_ok(pr2));
    ASSERT(frame.body_len == 200);

    PASS("custom max_size cap honored");
}

/* ============================================================================
 * 4. Malformed — reserved type / grease type / unknown
 * ============================================================================ */

static void
test_reserved_frame_type_rejected(void)
{
    /* Reserved types per RFC 9114 § 7.2.8: 0x02, 0x06, 0x08, 0x09. */
    uint8_t types[] = { 0x02, 0x06, 0x08, 0x09 };
    for (size_t i = 0; i < sizeof(types); i++) {
        /* Just type + zero-length body. */
        uint8_t bytes[2] = { types[i], 0x00 };
        n00b_h3_frame_t frame;
        n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(bytes, 2, &frame);
        ASSERT(n00b_result_is_err(pr));
        ASSERT(n00b_result_get_err(pr) == N00B_QUIC_ERR_PROTOCOL);
    }
    PASS("reserved frame types rejected (PROTOCOL)");
}

static void
test_grease_frame_type_parsed(void)
{
    /* Grease pattern: 0x1f * N + 0x21 (RFC 9114 § 7.2.8). */
    uint64_t grease0 = 0x21;          /* N=0 */
    ASSERT(n00b_h3_frame_type_is_grease(grease0));
    uint64_t grease1 = 0x21 + 0x1f;   /* N=1 */
    ASSERT(n00b_h3_frame_type_is_grease(grease1));

    /* Build a frame with type=0x21, body=3 bytes "abc".  Type fits
     * in 1 byte (varint < 0x40 → 1 byte). */
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    n00b_result_t(bool) er = n00b_h3_frame_emit(&buf, grease0,
                                                 (const uint8_t *)"abc", 3);
    ASSERT(n00b_result_is_ok(er));

    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse(&buf, 0, &frame);
    ASSERT(n00b_result_is_ok(pr));
    ASSERT(frame.type == grease0);

    PASS("grease frame parsed (caller ignores)");
}

/* ============================================================================
 * 5. SETTINGS body — duplicate / unknown / reserved
 * ============================================================================ */

static void
test_settings_duplicate_rejected(void)
{
    /* Two QPACK_MAX_TABLE_CAPACITY entries — duplicate. */
    uint8_t body[] = { 0x01, 0x10, 0x01, 0x20 };
    n00b_h3_settings_t s;
    n00b_result_t(bool) sr = n00b_h3_settings_parse(body, sizeof(body), &s);
    ASSERT(n00b_result_is_err(sr));
    ASSERT(n00b_result_get_err(sr) == N00B_QUIC_ERR_PROTOCOL);
    PASS("SETTINGS duplicate identifier rejected");
}

static void
test_settings_unknown_ignored(void)
{
    /* Unknown identifier (0x21 — 1-byte varint = 33) followed by a
     * known one.  0x21 is the first grease value; not a reserved
     * identifier; the parser's "ignore unknown" path applies. */
    uint8_t body[] = {
        0x21, 0x10,    /* unknown=33, value=0x10 — ignored */
        0x07, 0x05,    /* QPACK_BLOCKED_STREAMS=5 */
    };
    n00b_h3_settings_t s;
    n00b_result_t(bool) sr = n00b_h3_settings_parse(body, sizeof(body), &s);
    ASSERT(n00b_result_is_ok(sr));
    ASSERT(s.qpack_blocked_streams == 5);
    PASS("SETTINGS unknown identifier ignored");
}

static void
test_settings_reserved_h2_rejected(void)
{
    /* Reserved-from-HTTP/2 identifiers (0x02, 0x03, 0x04, 0x05) MUST
     * be a connection error of type H3_SETTINGS_ERROR. */
    uint8_t bodies[][2] = {
        { 0x02, 0x01 },
        { 0x03, 0x01 },
        { 0x04, 0x01 },
        { 0x05, 0x01 },
    };
    for (size_t i = 0; i < 4; i++) {
        n00b_h3_settings_t s;
        n00b_result_t(bool) sr = n00b_h3_settings_parse(bodies[i], 2, &s);
        ASSERT(n00b_result_is_err(sr));
        ASSERT(n00b_result_get_err(sr) == N00B_QUIC_ERR_PROTOCOL);
    }
    PASS("SETTINGS reserved-from-HTTP/2 identifiers rejected");
}

static void
test_settings_orphan_identifier_rejected(void)
{
    /* Single identifier byte without paired value. */
    uint8_t body[] = { 0x07 };  /* QPACK_BLOCKED_STREAMS, no value */
    n00b_h3_settings_t s;
    n00b_result_t(bool) sr = n00b_h3_settings_parse(body, sizeof(body), &s);
    ASSERT(n00b_result_is_err(sr));
    ASSERT(n00b_result_get_err(sr) == N00B_QUIC_ERR_PROTOCOL);
    PASS("SETTINGS orphan identifier rejected");
}

/* ============================================================================
 * 6. Multiple frames in one buffer
 * ============================================================================ */

static void
test_multiple_frames_one_buffer(void)
{
    n00b_buffer_t buf;
    fresh_buffer(&buf);
    n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA, (const uint8_t *)"a", 1);
    n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA, (const uint8_t *)"bcd", 3);
    n00b_h3_frame_emit(&buf, N00B_H3_FRAME_DATA, (const uint8_t *)"", 0);

    size_t off = 0;
    n00b_h3_frame_t f1, f2, f3;

    n00b_result_t(bool) p1 = n00b_h3_frame_parse(&buf, off, &f1);
    ASSERT(n00b_result_is_ok(p1));
    off += f1.consumed;

    n00b_result_t(bool) p2 = n00b_h3_frame_parse(&buf, off, &f2);
    ASSERT(n00b_result_is_ok(p2));
    off += f2.consumed;

    n00b_result_t(bool) p3 = n00b_h3_frame_parse(&buf, off, &f3);
    ASSERT(n00b_result_is_ok(p3));
    off += f3.consumed;

    ASSERT(off == (size_t)buf.byte_len);
    ASSERT(f1.body_len == 1 && memcmp(f1.body, "a", 1) == 0);
    ASSERT(f2.body_len == 3 && memcmp(f2.body, "bcd", 3) == 0);
    ASSERT(f3.body_len == 0);

    PASS("multiple frames in one buffer");
}

/* ============================================================================
 * 7. Empty input + null-arg paths
 * ============================================================================ */

static void
test_empty_input_need_more(void)
{
    n00b_h3_frame_t frame;
    n00b_result_t(bool) pr = n00b_h3_frame_parse_bytes(nullptr, 0, &frame);
    ASSERT(n00b_result_is_err(pr));
    ASSERT(n00b_result_get_err(pr) == N00B_QUIC_ERR_NEED_MORE_DATA);

    /* Null out: invalid arg. */
    uint8_t bytes[] = { 0x00, 0x00 };
    n00b_result_t(bool) pr2 = n00b_h3_frame_parse_bytes(bytes, 2, nullptr);
    ASSERT(n00b_result_is_err(pr2));
    ASSERT(n00b_result_get_err(pr2) == N00B_QUIC_ERR_NULL_ARG);

    PASS("empty input + null-arg paths");
}

/* ============================================================================
 * 8. Settings emit + parse round-trip with no max_field_section_size
 * ============================================================================ */

static void
test_settings_no_max_field_size(void)
{
    n00b_buffer_t body;
    fresh_buffer(&body);
    /* max_field_section_size = 0 means "do not advertise". */
    n00b_result_t(bool) br = n00b_h3_settings_emit_body(&body, 1024, 4, 0);
    ASSERT(n00b_result_is_ok(br));

    n00b_h3_settings_t s;
    n00b_result_t(bool) sr = n00b_h3_settings_parse(
        (const uint8_t *)body.data, (size_t)body.byte_len, &s);
    ASSERT(n00b_result_is_ok(sr));
    ASSERT(s.qpack_max_table_capacity == 1024);
    ASSERT(s.qpack_blocked_streams == 4);
    ASSERT(s.max_field_section_size == 0);  /* not seen */

    PASS("SETTINGS round-trip with no max_field_section_size");
}

/* ============================================================================ */

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running quic_h3_frame tests...\n");

    test_roundtrip_data_frame();
    test_roundtrip_empty_data();
    test_roundtrip_settings_frame();
    test_roundtrip_headers_frame();
    test_roundtrip_goaway_frame();
    test_roundtrip_cancel_push();
    test_roundtrip_max_push_id();
    test_truncated_incremental();
    test_oversized_rejected();
    test_custom_cap_rejected();
    test_reserved_frame_type_rejected();
    test_grease_frame_type_parsed();
    test_settings_duplicate_rejected();
    test_settings_unknown_ignored();
    test_settings_reserved_h2_rejected();
    test_settings_orphan_identifier_rejected();
    test_multiple_frames_one_buffer();
    test_empty_input_need_more();
    test_settings_no_max_field_size();

    printf("All quic_h3_frame tests passed. (%d sub-tests)\n", total_subtests);

    n00b_shutdown();
    return 0;
}
