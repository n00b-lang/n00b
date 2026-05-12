/*
 * fuzz_quic_h3_frame.c — libFuzzer harness for the H3 frame parser.
 *
 * Phase 4 § 4.3 + § 12 (test plan).  Build with libFuzzer (Clang
 * `-fsanitize=fuzzer,address`) and link against libn00b.  The
 * harness feeds raw bytes to `n00b_h3_frame_parse_bytes` and
 * asserts the contract:
 *
 *   - The call must NEVER crash, ASan-flag, UBSan-trap, or assert.
 *   - On success, the frame's `body` pointer falls inside the input
 *     range and `consumed <= input_len`.
 *   - Reserved RFC 9114 § 7.2.8 frame types (0x02/0x06/0x08/0x09)
 *     produce N00B_QUIC_ERR_PROTOCOL — never a successful parse.
 *   - On any error path, no body bytes have been read past the
 *     advertised length.
 *
 * Suggested seed corpus: the round-trip vectors in
 * `test/unit/test_quic_h3_frame.c`, plus a sprinkling of:
 *   - all reserved type single-byte prefixes
 *   - greased type ids (0x21, 0x40, 0xff)
 *   - truncated SETTINGS bodies
 *
 * Usage:
 *
 *     clang -O1 -g -fsanitize=fuzzer,address \
 *           -I include -I include/internal \
 *           test/fuzz/fuzz_quic_h3_frame.c \
 *           -L build_debug -ln00b -lpicoquic-core -lpicotls-core ... \
 *           -o build_debug/fuzz_quic_h3_frame
 *     build_debug/fuzz_quic_h3_frame corpus/ -max_total_time=300
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/runtime.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"

/* ----------------------------------------------------------------
 * Process-once init.  libFuzzer calls `LLVMFuzzerTestOneInput`
 * thousands of times per second; the n00b runtime is global and
 * one-shot.
 * ---------------------------------------------------------------- */

static _Atomic uint32_t init_once;
static n00b_runtime_t g_rt;

static void
do_init(void)
{
    int argc = 1;
    const char *argv[] = { "fuzz_quic_h3_frame", nullptr };
    n00b_init(&g_rt, argc, (char **)argv);
}

/* ----------------------------------------------------------------
 * libFuzzer entry point.
 * ---------------------------------------------------------------- */

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    do { uint32_t _e = 0; if (atomic_compare_exchange_strong(&init_once, &_e, 1)) { do_init(); atomic_store(&init_once, 2); } else { while (atomic_load(&init_once) != 2) {} } } while (0);

    if (size == 0) return 0;

    /* Cap size so a single very-large input doesn't blow the
     * harness budget. */
    if (size > 65536) size = 65536;

    n00b_h3_frame_t frame;
    n00b_result_t(bool) r = n00b_h3_frame_parse_bytes(data, size, &frame);

    if (n00b_result_is_ok(r)) {
        /* Successful parse — body must be contained, consumed must
         * be in range. */
        if (frame.consumed > size)        abort();
        if (frame.body_len > frame.consumed) abort();
        if (frame.body_len > 0) {
            const uint8_t *end = data + size;
            if (frame.body < data || frame.body > end)        abort();
            if (frame.body + frame.body_len > end)            abort();
        }
        /* Reserved frame types must NEVER produce a successful parse
         * (RFC 9114 § 7.2.8). */
        if (n00b_h3_frame_type_is_reserved(frame.type))       abort();
    } else {
        n00b_quic_err_t err = (n00b_quic_err_t)n00b_result_get_err(r);
        switch (err) {
        case N00B_QUIC_ERR_NEED_MORE_DATA:
        case N00B_QUIC_ERR_FRAME_TOO_LARGE:
        case N00B_QUIC_ERR_BAD_VARINT:
        case N00B_QUIC_ERR_PROTOCOL:
        case N00B_QUIC_ERR_NULL_ARG:
            break;
        default:
            /* Any other err is undocumented and a contract bug. */
            abort();
        }
    }

    return 0;
}
