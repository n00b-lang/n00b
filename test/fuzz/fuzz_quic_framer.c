/*
 * fuzz_quic_framer.c — libFuzzer harness for the QUIC wire framer.
 *
 * Phase 1 § 7.3 (test plan).  Build with libFuzzer (Clang
 * `-fsanitize=fuzzer,address`) and link against libn00b.  The harness
 * feeds raw bytes to `n00b_quic_frame_parse` and asserts the contract
 * laid out in framer.h:
 *
 *   - The call must NEVER crash, ASan-flag, UBSan-trap, or assert.
 *   - On ok+some(frame): `consumed <= input_len`,
 *     `payload_len < consumed`, and `payload` (if any) is a borrowed
 *     slice strictly inside the input buffer.
 *   - On ok+none: caller would wait for more bytes — no further
 *     constraints.
 *   - On err: error code is in the documented set
 *     (BAD_VARINT, FRAME_TOO_LARGE, NULL_ARG, INVALID_ARG).
 *
 * When parse succeeds, we re-emit the same frame and verify the
 * second parse round-trips byte-for-byte (size, type, payload).
 *
 * Suggested seed corpus:
 *   - The round-trip vectors in `test/unit/test_quic_framer.c`
 *     (capture buffers via gdb / a small dump pass, save to corpus/).
 *   - One-byte inputs `\x00` through `\xff` (boundary types).
 *   - Edge-case varint encodings: 0x00, 0x3f, 0x40, 0x7f,
 *     0x4000, 0x80000000-class transitions.
 *   - Maliciously-large varints (`\xc0\xff\xff\xff\xff\xff\xff\xff`
 *     advertising ~2^62 bytes of payload).
 *
 * Usage:
 *
 *     clang -O1 -g -fsanitize=fuzzer,address \
 *           -I include -I include/internal \
 *           test/fuzz/fuzz_quic_framer.c \
 *           -L build_debug -ln00b -lpicoquic-core -lpicotls-core ... \
 *           -o build_debug/fuzz_quic_framer
 *     build_debug/fuzz_quic_framer corpus/ -max_total_time=300
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "adt/option.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/framer.h"

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
    const char *argv[] = { "fuzz_quic_framer", nullptr };
    n00b_init(&g_rt, argc, (char **)argv);
}

/* Validate an error code is in the documented contract set. */
static bool
err_ok(int32_t e)
{
    return e == N00B_QUIC_ERR_BAD_VARINT
        || e == N00B_QUIC_ERR_FRAME_TOO_LARGE
        || e == N00B_QUIC_ERR_NULL_ARG
        || e == N00B_QUIC_ERR_INVALID_ARG;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    do {
        uint32_t _e = 0;
        if (atomic_compare_exchange_strong(&init_once, &_e, 1)) {
            do_init();
            atomic_store(&init_once, 2);
        } else {
            while (atomic_load(&init_once) != 2) { /* spin */ }
        }
    } while (0);

    /* Cap the input size: framer's default max is 16 MiB; 64 KiB is
     * plenty to exercise every varint length-class transition and
     * still leaves room for an oversize-claim test case. */
    if (size > 65536) size = 65536;

    /* Wrap input bytes into a borrowed-style n00b_buffer_t. */
    n00b_buffer_t in;
    memset(&in, 0, sizeof(in));
    n00b_buffer_init(&in, .length = (int64_t)size);
    if (size > 0) {
        memcpy(in.data, data, size);
        in.byte_len = (int64_t)size;
    }

    n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr =
        n00b_quic_frame_parse(&in, 0);

    if (n00b_result_is_err(pr)) {
        if (!err_ok(n00b_result_get_err(pr))) abort();
        return 0;
    }

    n00b_option_t(n00b_quic_frame_t) opt = n00b_result_get(pr);
    if (!n00b_option_is_set(opt)) {
        /* ok+none: needs more data.  No further invariants. */
        return 0;
    }

    n00b_quic_frame_t f = n00b_option_get(opt);

    /* Invariants on the returned frame. */
    if (f.consumed > size)                  abort();
    if (f.payload_len >= f.consumed)        abort();
    if (f.consumed < 1 + f.payload_len)     abort();  /* varint >= 1 byte + type */
    if (f.payload_len > 0) {
        const uint8_t *base = (const uint8_t *)in.data;
        if (f.payload < base)                            abort();
        if (f.payload + f.payload_len > base + size)     abort();
    }

    /* Round-trip: re-emit and verify the second parse matches. */
    n00b_buffer_t rt;
    memset(&rt, 0, sizeof(rt));
    n00b_buffer_init(&rt, .length = 0);
    n00b_result_t(bool) er = n00b_quic_frame_emit(&rt, f.type,
                                                  f.payload, f.payload_len);
    if (n00b_result_is_err(er))            abort();
    if ((size_t)rt.byte_len != f.consumed) abort();

    n00b_result_t(n00b_option_t(n00b_quic_frame_t)) pr2 =
        n00b_quic_frame_parse(&rt, 0);
    if (n00b_result_is_err(pr2))           abort();
    n00b_option_t(n00b_quic_frame_t) opt2 = n00b_result_get(pr2);
    if (!n00b_option_is_set(opt2))         abort();
    n00b_quic_frame_t f2 = n00b_option_get(opt2);
    if (f2.type != f.type)                 abort();
    if (f2.payload_len != f.payload_len)   abort();
    if (f2.consumed != f.consumed)         abort();
    if (f.payload_len > 0
        && memcmp(f.payload, f2.payload, f.payload_len) != 0) {
        abort();
    }

    return 0;
}
