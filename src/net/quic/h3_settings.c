/*
 * h3_settings.c — RFC 9114 § 7.2.4 SETTINGS frame body codec.
 *
 * The SETTINGS frame body is a sequence of `(identifier, value)`
 * pairs, both encoded as RFC 9000 varints.  Duplicate identifiers
 * MUST be treated as a connection error of type SETTINGS_ERROR (RFC
 * 9114 § 7.2.4).  Unknown identifiers MUST be ignored (§ 7.2.4.1).
 *
 * Identifiers RFC 9114 itself defines (rare) live in § 7.2.4.1; the
 * QPACK spec (RFC 9204 § 5) adds two more.  We track the supported
 * identifiers in a switch statement; everything else goes through
 * the "ignore unknown" path.
 *
 * The SETTINGS frame envelope (type byte + length varint) lives in
 * h3_frame.c — this file deals only with the body.
 */
#define N00B_USE_INTERNAL_API
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "adt/result.h"
#include "net/quic/h3.h"
#include "net/quic/h3_types.h"
#include "net/quic/quic_types.h"
#include "internal/net/quic/h3_internal.h"

n00b_result_t(bool)
n00b_h3_settings_emit_body(n00b_buffer_t *out,
                           uint64_t       qpack_max_table_capacity,
                           uint64_t       qpack_blocked_streams,
                           uint64_t       max_field_section_size)
{
    if (!out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }

    /* Always emit the QPACK-related settings; only emit
     * MAX_FIELD_SECTION_SIZE if the caller asked for one (0 = "no
     * advertised cap").  RFC 9114 doesn't require any specific
     * settings to be present; the wire shape is just a sequence of
     * pairs. */

    size_t a = n00b_h3_varint_append(out, N00B_H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY);
    if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    a = n00b_h3_varint_append(out, qpack_max_table_capacity);
    if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);

    a = n00b_h3_varint_append(out, N00B_H3_SETTINGS_QPACK_BLOCKED_STREAMS);
    if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    a = n00b_h3_varint_append(out, qpack_blocked_streams);
    if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);

    if (max_field_section_size > 0) {
        a = n00b_h3_varint_append(out, N00B_H3_SETTINGS_MAX_FIELD_SECTION_SIZE);
        if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
        a = n00b_h3_varint_append(out, max_field_section_size);
        if (a == 0) return n00b_result_err(bool, N00B_QUIC_ERR_FRAME_TOO_LARGE);
    }

    return n00b_result_ok(bool, true);
}

n00b_result_t(bool)
n00b_h3_settings_parse(const uint8_t      *body,
                       size_t              body_len,
                       n00b_h3_settings_t *out)
{
    if (!out) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }
    if (!body && body_len > 0) {
        return n00b_result_err(bool, N00B_QUIC_ERR_NULL_ARG);
    }

    memset(out, 0, sizeof(*out));

    /* Track which identifiers we've seen so we can reject
     * duplicates per RFC 9114 § 7.2.4. */
    bool saw_max_cap        = false;
    bool saw_blocked        = false;
    bool saw_max_field_size = false;
    bool saw_enable_connect = false;

    size_t off = 0;
    while (off < body_len) {
        uint64_t ident;
        int64_t  in = n00b_h3_varint_decode(body + off, body_len - off, &ident);
        if (in == 0) {
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }
        if (in < 0) {
            return n00b_result_err(bool, N00B_QUIC_ERR_BAD_VARINT);
        }
        off += (size_t)in;

        if (off >= body_len) {
            /* Identifier without a paired value. */
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }

        uint64_t value;
        int64_t  vn = n00b_h3_varint_decode(body + off, body_len - off, &value);
        if (vn == 0) {
            return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
        }
        if (vn < 0) {
            return n00b_result_err(bool, N00B_QUIC_ERR_BAD_VARINT);
        }
        off += (size_t)vn;

        switch (ident) {
        case N00B_H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY:
            if (saw_max_cap) {
                return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
            }
            saw_max_cap                  = true;
            out->qpack_max_table_capacity = value;
            break;
        case N00B_H3_SETTINGS_QPACK_BLOCKED_STREAMS:
            if (saw_blocked) {
                return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
            }
            saw_blocked              = true;
            out->qpack_blocked_streams = value;
            break;
        case N00B_H3_SETTINGS_MAX_FIELD_SECTION_SIZE:
            if (saw_max_field_size) {
                return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
            }
            saw_max_field_size         = true;
            out->max_field_section_size = value;
            break;
        case N00B_H3_SETTINGS_ENABLE_CONNECT_PROTOCOL:
            if (saw_enable_connect) {
                return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
            }
            saw_enable_connect          = true;
            out->enable_connect_protocol = (value != 0);
            break;
        default:
            /* RFC 9114 § 7.2.4.1: ignore unknown identifiers; also
             * trips on grease values (0x21, 0x40, ...).  RFC 9114
             * § 7.2.4.1 explicitly says reserved settings (those
             * matching the HTTP/2 leftovers, like 0x02/0x03/0x04/
             * 0x05) MUST be treated as a connection error of type
             * H3_SETTINGS_ERROR.  We enforce that here. */
            if (ident == 0x02u || ident == 0x03u || ident == 0x04u ||
                ident == 0x05u) {
                return n00b_result_err(bool, N00B_QUIC_ERR_PROTOCOL);
            }
            /* Otherwise: ignore. */
            break;
        }
    }

    out->received = true;
    return n00b_result_ok(bool, true);
}
