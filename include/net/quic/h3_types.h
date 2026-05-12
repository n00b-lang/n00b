/**
 * @file h3_types.h
 * @brief HTTP/3 (RFC 9114) frame types, stream kinds, pseudo-headers,
 *        and error codes.
 *
 * Phase 4 § 4.3.  This is the leaf header for the H3 module: it
 * declares the wire-level constants that anyone consuming H3 (the
 * client in this sub-phase, the server in 4.4, the future RPC layer)
 * needs to share.  No code lives here — only constants, enumerations,
 * and small POD types.
 *
 * @see RFC 9114 § 7 (frames) + § 6.2 (uni stream types) + § 8 (errors)
 * @see ~/dd/quic_4.md § 4.3
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * ALPN
 * =========================================================================== */

/** @brief ALPN protocol identifier per RFC 9114 § 3.1. */
#define N00B_H3_ALPN  "h3"

/* ===========================================================================
 * Frame types — RFC 9114 § 7.2
 *
 * The wire encoding is `varint(type) || varint(length) || payload`.
 * Frame types listed as "reserved" in § 7.2.1 (HTTP/2 leftovers, e.g.,
 * 0x02 / 0x06 / 0x08 / 0x09) MUST be treated as a connection error
 * — the parser hard-rejects them.
 * =========================================================================== */

typedef enum : uint64_t {
    N00B_H3_FRAME_DATA          = 0x00,  /**< Body bytes. */
    N00B_H3_FRAME_HEADERS       = 0x01,  /**< QPACK-encoded fields. */
    N00B_H3_FRAME_CANCEL_PUSH   = 0x03,  /**< Cancel a server push promise. */
    N00B_H3_FRAME_SETTINGS      = 0x04,  /**< Connection-level settings. */
    N00B_H3_FRAME_PUSH_PROMISE  = 0x05,
    N00B_H3_FRAME_GOAWAY        = 0x07,
    N00B_H3_FRAME_MAX_PUSH_ID   = 0x0d,
} n00b_h3_frame_type_t;

/**
 * @brief Reserved frame types (RFC 9114 § 7.2.8) — MUST trigger a
 *        connection error if sent by a peer.
 */
static inline bool
n00b_h3_frame_type_is_reserved(uint64_t t)
{
    return t == 0x02u || t == 0x06u || t == 0x08u || t == 0x09u;
}

/**
 * @brief Reserved-by-grease frame types (RFC 9114 § 7.2.8) — must be
 *        IGNORED.  Greasing values match the `0x1f * N + 0x21` form.
 */
static inline bool
n00b_h3_frame_type_is_grease(uint64_t t)
{
    return (t >= 0x21u) && ((t - 0x21u) % 0x1fu == 0u);
}

/* ===========================================================================
 * Unidirectional stream kinds — RFC 9114 § 6.2
 *
 * The first varint on a uni stream identifies the stream's role.
 * =========================================================================== */

typedef enum : uint64_t {
    N00B_H3_UNI_CONTROL        = 0x00,  /**< H3 control stream. */
    N00B_H3_UNI_PUSH           = 0x01,  /**< Push stream. */
    N00B_H3_UNI_QPACK_ENCODER  = 0x02,  /**< QPACK encoder stream. */
    N00B_H3_UNI_QPACK_DECODER  = 0x03,  /**< QPACK decoder stream. */
} n00b_h3_uni_stream_kind_t;

/* ===========================================================================
 * Settings identifiers — RFC 9114 § 7.2.4.1 + RFC 9204 § 5
 * =========================================================================== */

typedef enum : uint64_t {
    N00B_H3_SETTINGS_QPACK_MAX_TABLE_CAPACITY = 0x01,  /**< RFC 9204 § 5 */
    N00B_H3_SETTINGS_MAX_FIELD_SECTION_SIZE   = 0x06,
    N00B_H3_SETTINGS_QPACK_BLOCKED_STREAMS    = 0x07,  /**< RFC 9204 § 5 */
    N00B_H3_SETTINGS_ENABLE_CONNECT_PROTOCOL  = 0x08,
} n00b_h3_setting_id_t;

/* ===========================================================================
 * Application error codes — RFC 9114 § 8.1
 *
 * Used on RESET_STREAM / STOP_SENDING / CONNECTION_CLOSE.  Greased
 * values match the `0x1f * N + 0x21` form.
 * =========================================================================== */

typedef enum : uint64_t {
    N00B_H3_ERR_NO_ERROR              = 0x0100,
    N00B_H3_ERR_GENERAL_PROTOCOL      = 0x0101,
    N00B_H3_ERR_INTERNAL_ERROR        = 0x0102,
    N00B_H3_ERR_STREAM_CREATION       = 0x0103,
    N00B_H3_ERR_CLOSED_CRITICAL_STREAM = 0x0104,
    N00B_H3_ERR_FRAME_UNEXPECTED      = 0x0105,
    N00B_H3_ERR_FRAME_ERROR           = 0x0106,
    N00B_H3_ERR_EXCESSIVE_LOAD        = 0x0107,
    N00B_H3_ERR_ID_ERROR              = 0x0108,
    N00B_H3_ERR_SETTINGS_ERROR        = 0x0109,
    N00B_H3_ERR_MISSING_SETTINGS      = 0x010a,
    N00B_H3_ERR_REQUEST_REJECTED      = 0x010b,
    N00B_H3_ERR_REQUEST_CANCELLED     = 0x010c,
    N00B_H3_ERR_REQUEST_INCOMPLETE    = 0x010d,
    N00B_H3_ERR_MESSAGE_ERROR         = 0x010e,
    N00B_H3_ERR_CONNECT_ERROR         = 0x010f,
    N00B_H3_ERR_VERSION_FALLBACK      = 0x0110,
    /* QPACK errors share the H3 application error space — RFC 9204 § 6. */
    N00B_H3_ERR_QPACK_DECOMPRESSION_FAILED = 0x0200,
    N00B_H3_ERR_QPACK_ENCODER_STREAM_ERROR = 0x0201,
    N00B_H3_ERR_QPACK_DECODER_STREAM_ERROR = 0x0202,
} n00b_h3_app_error_t;

/* ===========================================================================
 * Pseudo-header field names — RFC 9114 § 4.3
 * =========================================================================== */

#define N00B_H3_PSEUDO_METHOD     ":method"
#define N00B_H3_PSEUDO_SCHEME     ":scheme"
#define N00B_H3_PSEUDO_AUTHORITY  ":authority"
#define N00B_H3_PSEUDO_PATH       ":path"
#define N00B_H3_PSEUDO_STATUS     ":status"

/* ===========================================================================
 * Default frame size cap (configurable per-client at construction)
 * =========================================================================== */

/**
 * @brief Default maximum body length the H3 frame parser will accept.
 *
 * Applies to a single HEADERS or DATA frame body.  Configurable per
 * client via `.max_frame_body` on `n00b_h3_client_new`.  Hard-capped
 * downstream by RFC 9000's varint maximum (2^62 - 1).  16 MiB matches
 * what the framer module uses for QUIC frames.
 */
#define N00B_H3_DEFAULT_MAX_FRAME_BODY  ((size_t)(16u * 1024u * 1024u))
