#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/buffer.h"
#include "core/runtime.h"
#include "net/quic/quic_types.h"
#include "net/quic/framer.h"

/* ============================================================================
 * 1. n00b_quic_varint_size — class boundaries
 * ============================================================================ */

static void
test_varint_size(void)
{
    /* 1-byte form: 0..63 */
    assert(n00b_quic_varint_size(0) == 1);
    assert(n00b_quic_varint_size(63) == 1);
    /* 2-byte form: 64..16383 */
    assert(n00b_quic_varint_size(64) == 2);
    assert(n00b_quic_varint_size(16383) == 2);
    /* 4-byte form: 16384..2^30 - 1 */
    assert(n00b_quic_varint_size(16384) == 4);
    assert(n00b_quic_varint_size((UINT64_C(1) << 30) - 1) == 4);
    /* 8-byte form: 2^30..2^62 - 1 */
    assert(n00b_quic_varint_size(UINT64_C(1) << 30) == 8);
    assert(n00b_quic_varint_size(N00B_QUIC_VARINT_MAX) == 8);
    /* Out of range */
    assert(n00b_quic_varint_size(N00B_QUIC_VARINT_MAX + 1) == 0);
    printf("  [PASS] varint_size class boundaries\n");
}

/* ============================================================================
 * 2. Varint encode/decode round-trip across each length class
 * ============================================================================ */

static void
expect_roundtrip(uint64_t value, size_t expected_size)
{
    uint8_t buf[16] = {0};

    n00b_result_t(size_t) er = n00b_quic_varint_encode(buf, sizeof(buf), value);
    assert(n00b_result_is_ok(er));
    assert(n00b_result_get(er) == expected_size);

    uint64_t decoded = 0xdeadbeefULL;
    n00b_result_t(n00b_option_t(size_t)) dr =
        n00b_quic_varint_decode(buf, expected_size, &decoded);
    assert(n00b_result_is_ok(dr));
    n00b_option_t(size_t) consumed_opt = n00b_result_get(dr);
    assert(n00b_option_is_set(consumed_opt));
    assert(n00b_option_get(consumed_opt) == expected_size);
    assert(decoded == value);
}

static void
test_varint_roundtrip(void)
{
    expect_roundtrip(0, 1);
    expect_roundtrip(1, 1);
    expect_roundtrip(63, 1);
    expect_roundtrip(64, 2);
    expect_roundtrip(16383, 2);
    expect_roundtrip(16384, 4);
    expect_roundtrip((UINT64_C(1) << 30) - 1, 4);
    expect_roundtrip(UINT64_C(1) << 30, 8);
    expect_roundtrip(UINT64_C(0xdeadbeefcafe), 8);
    expect_roundtrip(N00B_QUIC_VARINT_MAX, 8);
    printf("  [PASS] varint encode/decode roundtrip\n");
}

/* ============================================================================
 * 3. Varint encode rejects values past the 62-bit limit
 * ============================================================================ */

static void
test_varint_encode_rejects_oversize(void)
{
    uint8_t buf[8] = {0};
    n00b_result_t(size_t) r =
        n00b_quic_varint_encode(buf, sizeof(buf), N00B_QUIC_VARINT_MAX + 1);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_INVALID_ARG);
    printf("  [PASS] varint_encode rejects oversize\n");
}

/* ============================================================================
 * 4. Varint encode rejects insufficient output buffer
 * ============================================================================ */

static void
test_varint_encode_rejects_short_buf(void)
{
    uint8_t buf[1] = {0};
    /* 16384 needs 4 bytes; only 1 available. */
    n00b_result_t(size_t) r = n00b_quic_varint_encode(buf, 1, 16384);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_FRAME_TOO_LARGE);
    printf("  [PASS] varint_encode rejects short output buffer\n");
}

/* ============================================================================
 * 5. Varint decode reports truncation as None (not an error)
 * ============================================================================ */

static void
test_varint_decode_truncated(void)
{
    /* "01..." prefix → 2-byte form, but only first byte present. */
    uint8_t in[1] = {0x40};
    uint64_t v;
    n00b_result_t(n00b_option_t(size_t)) r =
        n00b_quic_varint_decode(in, 1, &v);
    assert(n00b_result_is_ok(r));
    n00b_option_t(size_t) consumed = n00b_result_get(r);
    assert(!n00b_option_is_set(consumed));

    /* Empty input also → None. */
    r = n00b_quic_varint_decode(in, 0, &v);
    assert(n00b_result_is_ok(r));
    consumed = n00b_result_get(r);
    assert(!n00b_option_is_set(consumed));

    printf("  [PASS] varint_decode truncated → None\n");
}

/* ============================================================================
 * 6. Frame emit / parse round-trip
 * ============================================================================ */

static void
test_frame_roundtrip(void)
{
    static const uint8_t payload[] = "hello, n00b/quic";
    const size_t         plen      = sizeof(payload) - 1;

    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);

    n00b_result_t(bool) er = n00b_quic_frame_emit(&buf,
                                                  N00B_QUIC_FRAME_T_TRANSPORT_LO,
                                                  payload,
                                                  plen);
    assert(n00b_result_is_ok(er));
    assert(n00b_result_get(er) == true);
    /* 1-byte varint (plen < 64) + 1 type byte + plen */
    assert((size_t)buf.byte_len == 1 + 1 + plen);

    n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr =
        n00b_quic_frame_parse(&buf, 0);
    assert(n00b_result_is_ok(pr));
    n00b_option_t(n00b_quic_frame_t) opt = n00b_result_get(pr);
    assert(n00b_option_is_set(opt));
    n00b_quic_frame_t f = n00b_option_get(opt);
    assert(f.type == N00B_QUIC_FRAME_T_TRANSPORT_LO);
    assert(f.payload_len == plen);
    assert(f.consumed == 1 + 1 + plen);
    assert(memcmp(f.payload, payload, plen) == 0);

    printf("  [PASS] frame roundtrip\n");
}

/* ============================================================================
 * 7. Multiple back-to-back frames in one buffer
 * ============================================================================ */

static void
test_frame_multi(void)
{
    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);

    static const uint8_t a[] = "first";
    static const uint8_t b[] = "second-frame";
    static const uint8_t c[] = "";  /* empty payload */

    assert(n00b_result_get(n00b_quic_frame_emit(&buf, 0x10, a, sizeof(a) - 1)));
    assert(n00b_result_get(n00b_quic_frame_emit(&buf, 0x11, b, sizeof(b) - 1)));
    assert(n00b_result_get(n00b_quic_frame_emit(&buf, 0x12, c, 0)));

    size_t off = 0;
    n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr;
    n00b_option_t(n00b_quic_frame_t) opt;
    n00b_quic_frame_t f;

    pr  = n00b_quic_frame_parse(&buf, off);
    assert(n00b_result_is_ok(pr));
    opt = n00b_result_get(pr);
    assert(n00b_option_is_set(opt));
    f   = n00b_option_get(opt);
    assert(f.type == 0x10 && f.payload_len == sizeof(a) - 1);
    off += f.consumed;

    pr  = n00b_quic_frame_parse(&buf, off);
    opt = n00b_result_get(pr);
    f   = n00b_option_get(opt);
    assert(f.type == 0x11 && f.payload_len == sizeof(b) - 1);
    off += f.consumed;

    pr  = n00b_quic_frame_parse(&buf, off);
    opt = n00b_result_get(pr);
    f   = n00b_option_get(opt);
    assert(f.type == 0x12 && f.payload_len == 0);
    /* For an empty payload the slice pointer is nullptr by contract. */
    assert(f.payload == nullptr);
    off += f.consumed;

    assert(off == (size_t)buf.byte_len);
    printf("  [PASS] frame multi back-to-back\n");
}

/* ============================================================================
 * 8. Frame parse on truncated input → None (not an error)
 * ============================================================================ */

static void
test_frame_parse_truncated(void)
{
    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);

    static const uint8_t payload[] = "truncate-me";
    assert(n00b_result_get(n00b_quic_frame_emit(&buf, 0x20, payload,
                                                sizeof(payload) - 1)));

    /* Hand the parser a strict prefix of the encoded frame.  Each prefix
     * shorter than the full frame should yield None.  */
    for (size_t prefix = 0; prefix + 1 < (size_t)buf.byte_len; prefix++) {
        n00b_buffer_t cut;
        memset(&cut, 0, sizeof(cut));
        n00b_buffer_init(&cut, .length = 0);
        for (size_t i = 0; i < prefix; i++) {
            n00b_buffer_resize(&cut, (uint64_t)(i + 1));
            cut.data[i] = buf.data[i];
        }
        n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr =
            n00b_quic_frame_parse(&cut, 0);
        assert(n00b_result_is_ok(pr));
        assert(!n00b_option_is_set(n00b_result_get(pr)));
    }
    printf("  [PASS] frame parse truncated → None\n");
}

/* ============================================================================
 * 9. Frame emit rejects oversize via .max_size
 * ============================================================================ */

static void
test_frame_emit_max_size(void)
{
    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);

    /* Payload of 100 bytes; cap of 50 → must reject. */
    uint8_t big[100];
    memset(big, 0xab, sizeof(big));
    n00b_result_t(bool) r =
        n00b_quic_frame_emit(&buf, 0x30, big, sizeof(big), .max_size = 50);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_FRAME_TOO_LARGE);
    /* Buffer must remain unchanged on failure. */
    assert((size_t)buf.byte_len == 0);
    printf("  [PASS] frame_emit honors max_size\n");
}

/* ============================================================================
 * 10. Frame parse rejects oversize via .max_size
 * ============================================================================ */

static void
test_frame_parse_max_size(void)
{
    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);

    /* Build a frame with payload_len = 1000.  Then parse with cap 100. */
    uint8_t payload[1000];
    memset(payload, 0xcd, sizeof(payload));
    assert(n00b_result_get(n00b_quic_frame_emit(&buf, 0x31, payload,
                                                sizeof(payload))));

    n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr =
        n00b_quic_frame_parse(&buf, 0, .max_size = 100);
    assert(n00b_result_is_err(pr));
    assert(n00b_result_get_err(pr) == N00B_QUIC_ERR_FRAME_TOO_LARGE);
    printf("  [PASS] frame_parse honors max_size\n");
}

/* ============================================================================
 * 11. Frame emit null-arg checks
 * ============================================================================ */

static void
test_frame_emit_null_arg(void)
{
    n00b_result_t(bool) r;

    /* Null buffer. */
    r = n00b_quic_frame_emit(nullptr, 0x00, nullptr, 0);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_NULL_ARG);

    /* Non-zero len with null payload. */
    n00b_buffer_t buf;
    memset(&buf, 0, sizeof(buf));
    n00b_buffer_init(&buf, .length = 0);
    r = n00b_quic_frame_emit(&buf, 0x00, nullptr, 5);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_QUIC_ERR_NULL_ARG);

    /* Zero len with null payload is fine. */
    r = n00b_quic_frame_emit(&buf, 0x00, nullptr, 0);
    assert(n00b_result_is_ok(r));

    printf("  [PASS] frame_emit null-arg behavior\n");
}

/* ============================================================================
 * 12. Error string sanity (one line of coverage; full set tested elsewhere)
 * ============================================================================ */

static void
test_err_str(void)
{
    assert(n00b_quic_err_str(N00B_QUIC_OK) != nullptr);
    assert(n00b_quic_err_str(N00B_QUIC_ERR_FRAME_TOO_LARGE) != nullptr);
    assert(n00b_quic_err_str((n00b_quic_err_t)42) != nullptr);
    /* Out-of-range falls through to "unknown" */
    assert(strcmp(n00b_quic_err_str((n00b_quic_err_t)42), "unknown") == 0);
    printf("  [PASS] err_str sanity\n");
}

/* ============================================================================ */

int
main(int argc, char **argv)
{
    n00b_runtime_t runtime;
    n00b_init(&runtime, argc, argv);

    printf("Running quic framer tests...\n");
    test_varint_size();
    test_varint_roundtrip();
    test_varint_encode_rejects_oversize();
    test_varint_encode_rejects_short_buf();
    test_varint_decode_truncated();
    test_frame_roundtrip();
    test_frame_multi();
    test_frame_parse_truncated();
    test_frame_emit_max_size();
    test_frame_parse_max_size();
    test_frame_emit_null_arg();
    test_err_str();
    printf("All quic framer tests passed.\n");

    n00b_shutdown();
    return 0;
}
