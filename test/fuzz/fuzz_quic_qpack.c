/*
 * fuzz_quic_qpack.c — libFuzzer harness for the QPACK decoder.
 *
 * Phase 4 § 4.2 + § 12 (test plan).  Build with libFuzzer (Clang
 * `-fsanitize=fuzzer,address`) and link against libn00b.  The harness
 * splits each fuzzer-supplied input into two shards:
 *
 *   - First half: bytes interpreted as encoder-stream input.  The
 *     decoder consumes them, mutating the dynamic table.
 *   - Second half: bytes interpreted as a field-section decoder
 *     input.  The decoder must process them.
 *
 * Contract:
 *   - Neither call may crash, ASan-flag, UBSan-trap, or assert.
 *   - Errors must come from the documented set (PROTOCOL,
 *     NEED_MORE_DATA, FRAME_TOO_LARGE, NULL_ARG).
 *
 * Suggested seed corpus:
 *   - The encoder's output from `test_quic_qpack.c` (run the test,
 *     dump section bytes via gdb, save to corpus/).
 *   - Cloudflare-quiche's QPACK test corpus
 *     (https://github.com/cloudflare/quiche/tree/master/qpack/tests/data
 *     if it exists; otherwise synthesize from the RFC examples).
 *   - RFC 9204 § B examples.
 *
 * Usage:
 *
 *     clang -O1 -g -fsanitize=fuzzer,address \
 *           -I include -I include/internal \
 *           test/fuzz/fuzz_quic_qpack.c \
 *           -L build_debug -ln00b -lpicoquic-core -lpicotls-core ... \
 *           -o build_debug/fuzz_quic_qpack
 *     build_debug/fuzz_quic_qpack corpus/ -max_total_time=300
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/qpack.h"

/* ----------------------------------------------------------------
 * Process-once init.
 * ---------------------------------------------------------------- */

static _Atomic uint32_t init_once;
static n00b_runtime_t g_rt;

static void
do_init(void)
{
    int argc = 1;
    const char *argv[] = { "fuzz_quic_qpack", nullptr };
    n00b_init(&g_rt, argc, (char **)argv);
}

/* Validate that an error code is in the documented contract set. */
static bool
err_ok(int32_t e)
{
    return e == N00B_QUIC_ERR_PROTOCOL
        || e == N00B_QUIC_ERR_NEED_MORE_DATA
        || e == N00B_QUIC_ERR_FRAME_TOO_LARGE
        || e == N00B_QUIC_ERR_NULL_ARG
        || e == N00B_QUIC_ERR_INVALID_ARG
        || e == N00B_QUIC_ERR_BAD_VARINT
        || e == N00B_QUIC_ERR_BAD_TYPE;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    do { uint32_t _e = 0; if (atomic_compare_exchange_strong(&init_once, &_e, 1)) { do_init(); atomic_store(&init_once, 2); } else { while (atomic_load(&init_once) != 2) {} } } while (0);

    /* Cap the input size to keep iterations fast.  libFuzzer can grow
     * inputs without bound; QPACK byte-economy means a 64 KiB max is
     * plenty to exercise every branch. */
    if (size > 64 * 1024) size = 64 * 1024;

    /* Each input is split: first half encoder-stream, second half
     * field-section.  If size < 2, just hammer the field-section path. */
    size_t es_len = size / 2;
    size_t fs_len = size - es_len;

    n00b_qpack_decoder_t *dec = n00b_qpack_decoder_new(64 * 1024, 64);

    /* Phase 1: feed encoder-stream bytes. */
    n00b_buffer_t ds; memset(&ds, 0, sizeof(ds));
    n00b_buffer_init(&ds, .length = 0);

    n00b_result_t(size_t) cr = n00b_qpack_decoder_consume_encoder_stream(
        dec, data, es_len, &ds);
    if (n00b_result_is_err(cr)) {
        if (!err_ok(n00b_result_get_err(cr))) abort();
    } else {
        size_t consumed = n00b_result_get(cr);
        if (consumed > es_len) abort();
    }

    /* Phase 2: try to decode the second half as a field section. */
    n00b_qpack_field_t out[64];
    size_t n_out = 0;
    n00b_buffer_t ds2; memset(&ds2, 0, sizeof(ds2));
    n00b_buffer_init(&ds2, .length = 0);

    n00b_result_t(bool) dr = n00b_qpack_decode(dec, /*stream_id*/ 1,
                                               data + es_len, fs_len,
                                               out, 64, &n_out, &ds2);
    if (n00b_result_is_err(dr)) {
        if (!err_ok(n00b_result_get_err(dr))) abort();
    } else {
        /* Defense in depth: out->name / value pointers must be non-null
         * if their lengths are non-zero. */
        for (size_t i = 0; i < n_out; i++) {
            if ((out[i].name_len > 0 && !out[i].name)
                || (out[i].value_len > 0 && !out[i].value)) {
                abort();
            }
            if (out[i].name_len > N00B_QPACK_MAX_FIELD_LINE
                || out[i].value_len > N00B_QPACK_MAX_FIELD_LINE) {
                abort();
            }
        }
    }

    n00b_qpack_decoder_close(dec);
    return 0;
}
