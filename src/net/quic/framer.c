#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "net/quic/framer.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Varint primitives (RFC 9000 §16)
 *
 * Two-bit length prefix in the high bits of the first byte:
 *   00 → 1 byte,  6 bits of value
 *   01 → 2 bytes, 14 bits of value
 *   10 → 4 bytes, 30 bits of value
 *   11 → 8 bytes, 62 bits of value
 *
 * The remaining bits are the value, big-endian.
 * =========================================================================== */

size_t
n00b_quic_varint_size(uint64_t value)
{
    if (value < (UINT64_C(1) << 6))  return 1;
    if (value < (UINT64_C(1) << 14)) return 2;
    if (value < (UINT64_C(1) << 30)) return 4;
    if (value <= N00B_QUIC_VARINT_MAX) return 8;
    return 0;
}

n00b_result_t(size_t)
n00b_quic_varint_encode(uint8_t *out, size_t out_cap, uint64_t value)
{
    if (!out) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    if (value > N00B_QUIC_VARINT_MAX) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }

    size_t  n      = n00b_quic_varint_size(value);
    uint8_t prefix;

    switch (n) {
    case 1: prefix = 0x00; break;
    case 2: prefix = 0x40; break;
    case 4: prefix = 0x80; break;
    case 8: prefix = 0xc0; break;
    default:
        /* Should be unreachable — caught above. */
        return n00b_result_err(size_t, N00B_QUIC_ERR_INVALID_ARG);
    }

    if (out_cap < n) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    /* Big-endian write. */
    for (size_t i = 0; i < n; i++) {
        out[n - 1 - i] = (uint8_t)(value & 0xffu);
        value >>= 8;
    }
    /* Stamp the length prefix into the high two bits of the first byte. */
    out[0] = (uint8_t)((out[0] & 0x3fu) | prefix);

    return n00b_result_ok(size_t, n);
}

n00b_result_t(n00b_option_t(size_t))
n00b_quic_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out_value)
{
    if (in_len == 0) {
        return n00b_result_ok(n00b_option_t(size_t),
                              n00b_option_none(size_t));
    }
    if (!in) {
        return n00b_result_err(n00b_option_t(size_t), N00B_QUIC_ERR_NULL_ARG);
    }

    uint8_t  first  = in[0];
    uint8_t  prefix = (uint8_t)(first >> 6);
    size_t   n;

    switch (prefix) {
    case 0: n = 1; break;
    case 1: n = 2; break;
    case 2: n = 4; break;
    case 3: n = 8; break;
    default: __builtin_unreachable();  /* prefix is 2 bits */
    }

    if (in_len < n) {
        /* Truncated; ask for more data. */
        return n00b_result_ok(n00b_option_t(size_t),
                              n00b_option_none(size_t));
    }

    uint64_t value = (uint64_t)(first & 0x3fu);
    for (size_t i = 1; i < n; i++) {
        value = (value << 8) | (uint64_t)in[i];
    }

    if (out_value) {
        *out_value = value;
    }
    return n00b_result_ok(n00b_option_t(size_t),
                          n00b_option_set(size_t, n));
}

/* ===========================================================================
 * Frame primitives
 * =========================================================================== */

n00b_result_t(bool)
n00b_quic_frame_emit(n00b_buffer_t *out,
                     uint8_t        type,
                     const uint8_t *payload,
                     size_t         payload_len) _kargs
{
    size_t max_size = N00B_QUIC_DEFAULT_MAX_FRAME_SIZE;
}
{
    if (!out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!payload && payload_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (payload_len > N00B_QUIC_VARINT_MAX) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    size_t vsize = n00b_quic_varint_size((uint64_t)payload_len);
    if (vsize == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    size_t total = vsize + 1 + payload_len;
    if (total > max_size) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    /* Grow the buffer to make room. resize() updates byte_len so we
     * record the old length first and write into [old, old+total).  */
    size_t old_len = (size_t)out->byte_len;
    n00b_buffer_resize(out, (uint64_t)(old_len + total));

    uint8_t *dst = (uint8_t *)out->data + old_len;

    n00b_result_t(size_t) er =
        n00b_quic_varint_encode(dst, vsize, (uint64_t)payload_len);
    if (n00b_result_is_err(er)) {
        /* Should not happen given the size pre-check; restore length. */
        n00b_buffer_resize(out, (uint64_t)old_len);
        return n00b_result_err(bool, n00b_result_get_err(er));
    }

    dst[vsize] = type;
    if (payload_len > 0) {
        memcpy(dst + vsize + 1, payload, payload_len);
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(n00b_option_t(n00b_quic_frame_t))
n00b_quic_frame_parse(n00b_buffer_t *in, size_t offset) _kargs
{
    size_t max_size = N00B_QUIC_DEFAULT_MAX_FRAME_SIZE;
}
{
    if (!in) {
        return n00b_result_err(n00b_option_t(n00b_quic_frame_t),
                               N00B_QUIC_ERR_NULL_ARG);
    }
    size_t buf_len = (size_t)in->byte_len;
    if (offset > buf_len) {
        return n00b_result_err(n00b_option_t(n00b_quic_frame_t),
                               N00B_QUIC_ERR_INVALID_ARG);
    }

    const uint8_t *p   = (const uint8_t *)in->data + offset;
    size_t         rem = buf_len - offset;

    uint64_t payload_len = 0;
    n00b_result_t(n00b_option_t(size_t)) vr =
        n00b_quic_varint_decode(p, rem, &payload_len);
    if (n00b_result_is_err(vr)) {
        return n00b_result_err(n00b_option_t(n00b_quic_frame_t),
                               n00b_result_get_err(vr));
    }
    n00b_option_t(size_t) vsz_opt = n00b_result_get(vr);
    if (!n00b_option_is_set(vsz_opt)) {
        /* Need more data for the varint header. */
        return n00b_result_ok(n00b_option_t(n00b_quic_frame_t),
                              n00b_option_none(n00b_quic_frame_t));
    }
    size_t vsize = n00b_option_get(vsz_opt);

    if (payload_len > N00B_QUIC_VARINT_MAX) {
        /* Caught at decode time, but defensively reject again. */
        return n00b_result_err(n00b_option_t(n00b_quic_frame_t),
                               N00B_QUIC_ERR_BAD_VARINT);
    }

    /* total = vsize + type byte + payload */
    if (payload_len > (uint64_t)max_size ||
        vsize + 1 + (size_t)payload_len > max_size) {
        return n00b_result_err(n00b_option_t(n00b_quic_frame_t),
                               N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    size_t needed = vsize + 1 + (size_t)payload_len;
    if (rem < needed) {
        /* We have a complete length header but not the full body yet. */
        return n00b_result_ok(n00b_option_t(n00b_quic_frame_t),
                              n00b_option_none(n00b_quic_frame_t));
    }

    n00b_quic_frame_t frame = {
        .type        = p[vsize],
        .payload     = (payload_len > 0) ? (p + vsize + 1) : nullptr,
        .payload_len = (size_t)payload_len,
        .consumed    = needed,
    };

    return n00b_result_ok(n00b_option_t(n00b_quic_frame_t),
                          n00b_option_set(n00b_quic_frame_t, frame));
}
