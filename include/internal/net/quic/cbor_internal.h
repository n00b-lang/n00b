/**
 * @file cbor_internal.h
 * @internal
 * @brief Shared CBOR encoder/decoder internals — not for public use.
 *
 * The encoder is a pure append-to-buffer routine; only the decoder
 * needs internal state types (cursor + bounded depth tracking).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "core/alloc.h"
#include "net/quic/cbor.h"

/* ---------------------------------------------------------------------------
 * Major / additional-info nibble constants (RFC 8949 § 3)
 * --------------------------------------------------------------------------- */

#define N00B_CBOR_MT_UINT     0
#define N00B_CBOR_MT_NEGINT   1
#define N00B_CBOR_MT_BYTES    2
#define N00B_CBOR_MT_TEXT     3
#define N00B_CBOR_MT_ARRAY    4
#define N00B_CBOR_MT_MAP      5
#define N00B_CBOR_MT_TAG      6
#define N00B_CBOR_MT_PRIMITIVE 7

#define N00B_CBOR_AI_FALSE    20
#define N00B_CBOR_AI_TRUE     21
#define N00B_CBOR_AI_NULL     22
#define N00B_CBOR_AI_UNDEF    23
#define N00B_CBOR_AI_SIMPLE1  24  /* simple value in next byte */
#define N00B_CBOR_AI_FLOAT16  25
#define N00B_CBOR_AI_FLOAT32  26
#define N00B_CBOR_AI_FLOAT64  27
#define N00B_CBOR_AI_INDEF    31  /* indefinite length / break */

/* ---------------------------------------------------------------------------
 * Decoder state — single-buffer, single-pass.
 * --------------------------------------------------------------------------- */

/**
 * @internal
 * @brief Decoder cursor.
 *
 * Tracks the input bytes plus a depth counter to enforce
 * `N00B_CBOR_MAX_DEPTH`.  Allocated on the stack at the entry point;
 * never escapes the call.
 */
typedef struct {
    const uint8_t    *data;
    size_t            len;
    size_t            pos;
    int               depth;
    n00b_allocator_t *alloc;
} n00b_cbor_decoder_t;

/* ---------------------------------------------------------------------------
 * Shared allocator helper
 *
 * Defined in `cbor_encoder.c`; referenced by `cbor_decoder.c` via the
 * extern declaration here so both sides agree on the pool.
 * --------------------------------------------------------------------------- */

extern n00b_allocator_t *n00b_cbor_alloc(void);
