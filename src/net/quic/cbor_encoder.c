/*
 * cbor_encoder.c — RFC 8949 CBOR encoder.
 *
 * Phase 4 § 4.1.  Pure append-to-buffer; no decoder state.  Outputs
 * canonical / "core deterministic" CBOR (§ 4.2.1):
 *
 *   - Integers use the smallest length that fits.
 *   - Floats are emitted as 64-bit (IEEE 754 binary64).  Half/float
 *     down-conversion is a follow-up; we don't need it for the RPC
 *     wire format.
 *   - Arrays / maps are definite-length only.
 *
 * All allocations route through the conduit pool — see
 * `n00b_cbor_alloc()`.
 */

#define N00B_USE_INTERNAL_API
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "net/quic/cbor.h"
#include "internal/net/quic/cbor_internal.h"

/* ===========================================================================
 * Allocator — every CBOR-owned alloc lands in conduit_pool.
 * =========================================================================== */

n00b_allocator_t *
n00b_cbor_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

/* ===========================================================================
 * Buffer append helpers
 * =========================================================================== */

static void
buf_append(n00b_buffer_t *dst, const uint8_t *src, size_t n)
{
    size_t old = (size_t)dst->byte_len;
    n00b_buffer_resize(dst, (uint64_t)(old + n));
    memcpy(dst->data + old, src, n);
}

static void
buf_append_u8(n00b_buffer_t *dst, uint8_t b)
{
    buf_append(dst, &b, 1);
}

/* RFC 8949 § 3 head: write the major type + additional-info nibble
 * plus the (possibly absent) extended-length payload.  `value` is
 * the magnitude / count for major types 0-3 + 6 + arrays/maps; for
 * primitives it carries the simple value or float bit width. */
static void
write_head(n00b_buffer_t *dst, uint8_t mt, uint64_t value)
{
    uint8_t mthi = (uint8_t)(mt << 5);

    if (value < 24) {
        buf_append_u8(dst, (uint8_t)(mthi | (uint8_t)value));
    } else if (value <= 0xff) {
        uint8_t bytes[2] = { (uint8_t)(mthi | 24u), (uint8_t)value };
        buf_append(dst, bytes, 2);
    } else if (value <= 0xffff) {
        uint8_t bytes[3] = {
            (uint8_t)(mthi | 25u),
            (uint8_t)((value >> 8) & 0xff),
            (uint8_t)(value & 0xff),
        };
        buf_append(dst, bytes, 3);
    } else if (value <= 0xffffffffu) {
        uint8_t bytes[5] = {
            (uint8_t)(mthi | 26u),
            (uint8_t)((value >> 24) & 0xff),
            (uint8_t)((value >> 16) & 0xff),
            (uint8_t)((value >>  8) & 0xff),
            (uint8_t)( value        & 0xff),
        };
        buf_append(dst, bytes, 5);
    } else {
        uint8_t bytes[9] = {
            (uint8_t)(mthi | 27u),
            (uint8_t)((value >> 56) & 0xff),
            (uint8_t)((value >> 48) & 0xff),
            (uint8_t)((value >> 40) & 0xff),
            (uint8_t)((value >> 32) & 0xff),
            (uint8_t)((value >> 24) & 0xff),
            (uint8_t)((value >> 16) & 0xff),
            (uint8_t)((value >>  8) & 0xff),
            (uint8_t)( value        & 0xff),
        };
        buf_append(dst, bytes, 9);
    }
}

/* ===========================================================================
 * Public encoder entry points
 * =========================================================================== */

void
n00b_cbor_write_uint(n00b_buffer_t *dst, uint64_t value)
{
    write_head(dst, N00B_CBOR_MT_UINT, value);
}

void
n00b_cbor_write_int(n00b_buffer_t *dst, int64_t value)
{
    if (value < 0) {
        /* CBOR negint encodes ~value (i.e., abs(value) - 1).  For
         * INT64_MIN this still fits in uint64_t. */
        uint64_t mag = (uint64_t)(-(value + 1));
        write_head(dst, N00B_CBOR_MT_NEGINT, mag);
    } else {
        write_head(dst, N00B_CBOR_MT_UINT, (uint64_t)value);
    }
}

void
n00b_cbor_write_bool(n00b_buffer_t *dst, bool value)
{
    buf_append_u8(dst,
                  (uint8_t)((N00B_CBOR_MT_PRIMITIVE << 5)
                            | (value ? N00B_CBOR_AI_TRUE : N00B_CBOR_AI_FALSE)));
}

void
n00b_cbor_write_null(n00b_buffer_t *dst)
{
    buf_append_u8(dst,
                  (uint8_t)((N00B_CBOR_MT_PRIMITIVE << 5) | N00B_CBOR_AI_NULL));
}

void
n00b_cbor_write_double(n00b_buffer_t *dst, double value)
{
    /* Major 7, ai=27, 8-byte big-endian IEEE 754 binary64. */
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    uint8_t bytes[9] = {
        (uint8_t)((N00B_CBOR_MT_PRIMITIVE << 5) | N00B_CBOR_AI_FLOAT64),
        (uint8_t)((bits >> 56) & 0xff),
        (uint8_t)((bits >> 48) & 0xff),
        (uint8_t)((bits >> 40) & 0xff),
        (uint8_t)((bits >> 32) & 0xff),
        (uint8_t)((bits >> 24) & 0xff),
        (uint8_t)((bits >> 16) & 0xff),
        (uint8_t)((bits >>  8) & 0xff),
        (uint8_t)( bits        & 0xff),
    };
    buf_append(dst, bytes, 9);
}

void
n00b_cbor_write_bytes(n00b_buffer_t *dst, const uint8_t *data, size_t len)
{
    write_head(dst, N00B_CBOR_MT_BYTES, (uint64_t)len);
    if (len > 0) {
        buf_append(dst, data, len);
    }
}

void
n00b_cbor_write_buffer(n00b_buffer_t *dst, n00b_buffer_t *value)
{
    if (!value || value->byte_len <= 0) {
        write_head(dst, N00B_CBOR_MT_BYTES, 0);
        return;
    }
    write_head(dst, N00B_CBOR_MT_BYTES, (uint64_t)value->byte_len);
    buf_append(dst, (const uint8_t *)value->data, (size_t)value->byte_len);
}

void
n00b_cbor_write_text(n00b_buffer_t *dst, const char *data, size_t len)
{
    write_head(dst, N00B_CBOR_MT_TEXT, (uint64_t)len);
    if (len > 0) {
        buf_append(dst, (const uint8_t *)data, len);
    }
}

void
n00b_cbor_write_string(n00b_buffer_t *dst, n00b_string_t *value)
{
    if (!value || value->u8_bytes == 0) {
        write_head(dst, N00B_CBOR_MT_TEXT, 0);
        return;
    }
    write_head(dst, N00B_CBOR_MT_TEXT, (uint64_t)value->u8_bytes);
    buf_append(dst, (const uint8_t *)value->data, value->u8_bytes);
}

void
n00b_cbor_write_array_header(n00b_buffer_t *dst, uint64_t count)
{
    write_head(dst, N00B_CBOR_MT_ARRAY, count);
}

void
n00b_cbor_write_map_header(n00b_buffer_t *dst, uint64_t count)
{
    write_head(dst, N00B_CBOR_MT_MAP, count);
}

void
n00b_cbor_write_tag(n00b_buffer_t *dst, uint64_t tag)
{
    write_head(dst, N00B_CBOR_MT_TAG, tag);
}

/* ===========================================================================
 * Top-level convenience encoders
 *
 * Each constructs a fresh conduit-pool buffer, writes the value, and
 * returns the buffer.
 * =========================================================================== */

static n00b_buffer_t *
fresh_cbor_buf(void)
{
    return n00b_buffer_empty(.allocator = n00b_cbor_alloc());
}

n00b_buffer_t *
n00b_cbor_encode_int64(int64_t v)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_int(b, v);
    return b;
}

n00b_buffer_t *
n00b_cbor_encode_bool_(bool v)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_bool(b, v);
    return b;
}

n00b_buffer_t *
n00b_cbor_encode_double_(double v)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_double(b, v);
    return b;
}

n00b_buffer_t *
n00b_cbor_encode_string_(n00b_string_t *v)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_string(b, v);
    return b;
}

n00b_buffer_t *
n00b_cbor_encode_buffer_(n00b_buffer_t *v)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_buffer(b, v);
    return b;
}

n00b_buffer_t *
n00b_cbor_encode_null_(void)
{
    n00b_buffer_t *b = fresh_cbor_buf();
    n00b_cbor_write_null(b);
    return b;
}
