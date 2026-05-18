/*
 * h3_frame.c — HTTP/3 (RFC 9114 § 7.2) frame encoder + decoder.
 *
 * The H3 frame layer is a thin envelope: each frame is
 * `varint(type) || varint(length) || payload`.  Both varints are RFC
 * 9000 § 16 QUIC varints (1/2/4/8 bytes; high two bits encode length).
 *
 * Hardening rules baked into the parser:
 *   - Truncated input → returns N00B_QUIC_ERR_NEED_MORE_DATA
 *     (callers feed bytes incrementally — the parser is stateless;
 *     just call again with more).
 *   - body length > caller-configurable cap (default 16 MiB) →
 *     N00B_QUIC_ERR_FRAME_TOO_LARGE.
 *   - Reserved frame types (0x02, 0x06, 0x08, 0x09 — RFC 9114
 *     § 7.2.8) are a hard error: the spec demands a connection close
 *     with H3_FRAME_UNEXPECTED.  We surface the rejection so the
 *     caller can do that.
 *   - Greased frame types (0x21, 0x40, 0x5f, ... — § 7.2.8) are
 *     parsed normally; the caller is responsible for ignoring them.
 *     We don't filter at the parse layer because the body is still
 *     valid bytes the caller may want to skip cleanly.
 */
#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "adt/option.h"
#include "adt/result.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "net/quic/quic_types.h"
#include "internal/net/quic/h3_internal.h"

/* ===========================================================================
 * Allocator
 * =========================================================================== */

n00b_allocator_t *
n00b_h3_alloc(void)
{
    return (n00b_allocator_t *)&n00b_get_runtime()->conduit_pool;
}

/* ===========================================================================
 * Internal varint helpers
 *
 * RFC 9000 § 16 / RFC 9114 § 7 — same encoding the QUIC framer uses.
 * Re-implemented locally to keep the H3 frame layer free of any
 * non-essential dependency.
 * =========================================================================== */

static size_t
varint_size(uint64_t value)
{
    if (value < (UINT64_C(1) << 6))  return 1;
    if (value < (UINT64_C(1) << 14)) return 2;
    if (value < (UINT64_C(1) << 30)) return 4;
    if (value <= N00B_QUIC_VARINT_MAX) return 8;
    return 0;
}

size_t
n00b_h3_varint_append(n00b_buffer_t *out, uint64_t value)
{
    size_t n = varint_size(value);
    if (n == 0) return 0;

    uint8_t prefix;
    switch (n) {
    case 1: prefix = 0x00; break;
    case 2: prefix = 0x40; break;
    case 4: prefix = 0x80; break;
    case 8: prefix = 0xc0; break;
    default: return 0;
    }

    size_t old = (size_t)out->byte_len;
    n00b_buffer_resize(out, (uint64_t)(old + n));
    uint8_t *p = (uint8_t *)out->data + old;

    /* Big-endian write of the value into the n-byte slot, then OR
     * the prefix bits in. */
    uint64_t v = value;
    for (size_t i = 0; i < n; i++) {
        p[n - 1 - i] = (uint8_t)(v & 0xffu);
        v >>= 8;
    }
    p[0] = (uint8_t)((p[0] & 0x3fu) | prefix);
    return n;
}

int64_t
n00b_h3_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out_value)
{
    if (in_len == 0) return 0;

    uint8_t first  = in[0];
    uint8_t prefix = (uint8_t)(first >> 6);
    size_t  n;

    switch (prefix) {
    case 0: n = 1; break;
    case 1: n = 2; break;
    case 2: n = 4; break;
    case 3: n = 8; break;
    default: return -1;
    }
    if (in_len < n) return 0;

    uint64_t value = (uint64_t)(first & 0x3fu);
    for (size_t i = 1; i < n; i++) {
        value = (value << 8) | (uint64_t)in[i];
    }
    if (out_value) *out_value = value;
    return (int64_t)n;
}

/* ===========================================================================
 * Frame emit
 * =========================================================================== */

n00b_result_t(size_t)
n00b_h3_frame_emit_header(n00b_buffer_t *out, uint64_t type, uint64_t length)
{
    if (!out) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_NULL_ARG);
    }
    if (type > N00B_QUIC_VARINT_MAX || length > N00B_QUIC_VARINT_MAX) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    size_t a = n00b_h3_varint_append(out, type);
    if (a == 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }
    size_t b = n00b_h3_varint_append(out, length);
    if (b == 0) {
        return n00b_result_err(size_t, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }
    return n00b_result_ok(size_t, a + b);
}

n00b_result_t(bool)
n00b_h3_frame_emit(n00b_buffer_t *out,
                   uint64_t       type,
                   const uint8_t *body,
                   size_t         body_len) _kargs
{
    size_t max_size = N00B_H3_DEFAULT_MAX_FRAME_BODY;
}
{
    if (!out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!body && body_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (body_len > max_size) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }
    if (body_len > N00B_QUIC_VARINT_MAX) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    /* Save the current length so we can roll back on a failed
     * write of the body chunk (the header may have been written
     * already). */
    size_t saved = (size_t)out->byte_len;

    n00b_result_t(size_t) hr = n00b_h3_frame_emit_header(out, type,
                                                         (uint64_t)body_len);
    if (n00b_result_is_err(hr)) {
        out->byte_len = (uint64_t)saved;
        return n00b_result_err(bool, n00b_result_get_err(hr));
    }

    if (body_len > 0) {
        size_t old = (size_t)out->byte_len;
        n00b_buffer_resize(out, (uint64_t)(old + body_len));
        memcpy((uint8_t *)out->data + old, body, body_len);
    }

    return n00b_result_ok(bool, true);
}

/* ===========================================================================
 * Frame parse
 *
 * Parses one frame.  Returns NEED_MORE_DATA on truncation; this is
 * critical for streaming use where bytes arrive incrementally.
 * =========================================================================== */

static n00b_result_t(bool)
parse_one(const uint8_t   *data,
          size_t           data_len,
          size_t           max_size,
          n00b_h3_frame_t *out)
{
    if (!out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!data && data_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (data_len == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }

    /* Parse type varint. */
    uint64_t type;
    int64_t  tn = n00b_h3_varint_decode(data, data_len, &type);
    if (tn == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }
    if (tn < 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_BAD_VARINT);
    }

    /* RFC 9114 § 7.2.8: reserved types are a connection error. */
    if (n00b_h3_frame_type_is_reserved(type)) {
        return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
    }

    /* Parse length varint. */
    uint64_t length;
    int64_t  ln = n00b_h3_varint_decode(data + tn,
                                        data_len - (size_t)tn,
                                        &length);
    if (ln == 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }
    if (ln < 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_BAD_VARINT);
    }

    if (length > (uint64_t)max_size) {
        return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    size_t header_size = (size_t)tn + (size_t)ln;
    size_t total       = header_size + (size_t)length;
    if (total > data_len) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NEED_MORE_DATA);
    }

    out->type     = type;
    out->body     = (length == 0) ? nullptr : data + header_size;
    out->body_len = (size_t)length;
    out->consumed = total;
    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_frame_parse(n00b_buffer_t   *in,
                    size_t           offset,
                    n00b_h3_frame_t *out) _kargs
{
    size_t max_size = N00B_H3_DEFAULT_MAX_FRAME_BODY;
}
{
    if (!in || !out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (offset > (size_t)in->byte_len) {
        return n00b_result_err(bool, N00B_QUIC_ERR_INVALID_ARG);
    }
    return parse_one((const uint8_t *)in->data + offset,
                     (size_t)in->byte_len - offset,
                     max_size, out);
}

n00b_result_t(bool)
n00b_h3_frame_parse_bytes(const uint8_t   *data,
                          size_t           data_len,
                          n00b_h3_frame_t *out) _kargs
{
    size_t max_size = N00B_H3_DEFAULT_MAX_FRAME_BODY;
}
{
    return parse_one(data, data_len, max_size, out);
}
