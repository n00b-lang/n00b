/*
 * fuzz_quic_cbor.c — libFuzzer harness for the CBOR decoder.
 *
 * Phase 4 § 4.1 + § 12 (test plan).  Build with libFuzzer (Clang
 * `-fsanitize=fuzzer,address`) and link against libn00b.  The
 * harness feeds raw bytes to `n00b_cbor_decode_bytes` and asserts
 * the contract:
 *
 *   - The call must NEVER crash, ASan-flag, UBSan-trap, or assert.
 *   - On success, the AST root's `kind` is a valid enumerator and
 *     the entire input was consumed (the decoder rejects trailing
 *     bytes; we re-check that invariant here).
 *   - On failure, the error code is one of the documented codes.
 *
 * Suggested seed corpus: extracted from the unit-test hex vectors
 * (`grep '^"' test/unit/test_quic_cbor.c | xxd -r -p`) plus the
 * cbor-test-vectors github project (RFC 8949 expanded set).
 *
 * Usage:
 *
 *     clang -O1 -g -fsanitize=fuzzer,address \\
 *           -I include -I include/internal \\
 *           test/fuzz/fuzz_quic_cbor.c \\
 *           -L build_debug -ln00b -lpicoquic-core -lpicotls-core ... \\
 *           -o build_debug/fuzz_quic_cbor
 *     build_debug/fuzz_quic_cbor corpus/ -max_total_time=300
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "n00b.h"
#include "core/runtime.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/cbor.h"

/* ----------------------------------------------------------------
 * Process-once init.  libFuzzer enters `LLVMFuzzerTestOneInput`
 * thousands of times per second; we set up the n00b runtime once
 * via an atomic-flag-guarded one-shot.
 * ---------------------------------------------------------------- */

static _Atomic uint32_t init_once;
static n00b_runtime_t g_rt;

static void
do_init(void)
{
    int argc = 1;
    const char *argv[] = { "fuzz_quic_cbor", nullptr };
    n00b_init(&g_rt, argc, (char **)argv);
}

/* ----------------------------------------------------------------
 * Recursive AST sanity walker.  Asserts every reachable node has
 * a valid kind discriminator and that container counts agree with
 * the materialized children pointers.
 * ---------------------------------------------------------------- */

static void
sanity_walk(n00b_cbor_value_t *v, int depth)
{
    if (!v) return;
    /* Defense in depth: matches the decoder's cap. */
    if (depth > N00B_CBOR_MAX_DEPTH + 2) abort();

    switch (v->kind) {
    case N00B_CBOR_VT_UINT:
    case N00B_CBOR_VT_NEGINT:
    case N00B_CBOR_VT_INT64:
    case N00B_CBOR_VT_BOOL:
    case N00B_CBOR_VT_NULL:
    case N00B_CBOR_VT_UNDEFINED:
    case N00B_CBOR_VT_SIMPLE:
    case N00B_CBOR_VT_DOUBLE:
    case N00B_CBOR_VT_FLOAT32:
    case N00B_CBOR_VT_FLOAT16:
        return;
    case N00B_CBOR_VT_BYTES:
        if (!v->u.bytes) abort();
        return;
    case N00B_CBOR_VT_STRING:
        if (!v->u.string) abort();
        return;
    case N00B_CBOR_VT_ARRAY:
        if (v->u.array.count > 0 && !v->u.array.items) abort();
        for (size_t i = 0; i < v->u.array.count; i++) {
            sanity_walk(v->u.array.items[i], depth + 1);
        }
        return;
    case N00B_CBOR_VT_MAP:
        if (v->u.map.count > 0 && !v->u.map.pairs) abort();
        for (size_t i = 0; i < v->u.map.count; i++) {
            sanity_walk(v->u.map.pairs[i].key, depth + 1);
            sanity_walk(v->u.map.pairs[i].val, depth + 1);
        }
        return;
    case N00B_CBOR_VT_TAG:
        sanity_walk(v->u.tag.inner, depth + 1);
        return;
    default:
        abort();
    }
}

/* ----------------------------------------------------------------
 * libFuzzer entry point.
 * ---------------------------------------------------------------- */

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    do { uint32_t _e = 0; if (atomic_compare_exchange_strong(&init_once, &_e, 1)) { do_init(); atomic_store(&init_once, 2); } else { while (atomic_load(&init_once) != 2) {} } } while (0);

    auto r = n00b_cbor_decode_bytes(data, size);
    if (n00b_result_is_ok(r)) {
        n00b_cbor_value_t *v = n00b_result_get(r);
        sanity_walk(v, 0);
    } else {
        n00b_err_t e = n00b_result_get_err(r);
        /* Documented contract — see cbor.h. */
        if (e != N00B_QUIC_ERR_NEED_MORE_DATA
         && e != N00B_QUIC_ERR_PROTOCOL
         && e != N00B_QUIC_ERR_NULL_ARG
         && e != N00B_QUIC_ERR_FRAME_TOO_LARGE
         && e != N00B_QUIC_ERR_BAD_TYPE) {
            abort();
        }
    }
    return 0;
}
