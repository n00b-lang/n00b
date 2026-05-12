/**
 * @file framer.h
 * @brief Wire framer: varint-prefixed, type-tagged byte frames.
 *
 * The framer is the lowest layer of the QUIC application-protocol stack.
 * It defines a single self-describing frame format used by both H3 and the
 * RPC layer (each carving out its own type-tag range, see
 * @c quic_types.h).
 *
 * ### Wire format
 *
 * ```
 * +-----------------+----------+-----------+
 * | varint(len)     | u8 type  | payload[] |
 * +-----------------+----------+-----------+
 * ```
 *
 * - `len` is an RFC 9000 §16 varint and gives the **payload length in
 *   bytes**.  The type byte is *not* counted.
 * - `type` is one octet from the namespaces declared in @c quic_types.h.
 * - `payload[]` is exactly `len` bytes.
 *
 * The total encoded frame size is `varint_size(len) + 1 + len`.
 *
 * ### Hard cap
 *
 * The framer enforces a configurable maximum encoded frame size; the
 * default is @c N00B_QUIC_DEFAULT_MAX_FRAME_SIZE (16 MiB).  Defends
 * against a peer sending a varint claiming up to 2^62 bytes, which is
 * the QUIC varint range.  Configurable downward via the `.max_size`
 * kwarg; configurable upward only with the
 * `N00B_QUIC_ALLOW_LARGE_FRAMES` compile flag.
 *
 * ### No fragmentation
 *
 * The framer does not split a logical message across multiple frames.
 * Byte-stream channels are a different channel kind with their own API
 * (see @c chan.h, @c N00B_QUIC_CHAN_BYTES).  Conflating framed-message
 * and byte-stream semantics on one channel produces the HTTP/2
 * CONTINUATION-frame class of bug.
 *
 * ### Concurrency
 *
 * Framer functions do not lock the buffers they operate on.  The caller
 * is responsible for ensuring no concurrent mutation during a parse or
 * emit call.  In practice the buffers passed in are usually freshly
 * created in the calling code path or under an explicit lock.
 *
 * @see quic_types.h, chan.h
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "n00b.h"
#include "core/buffer.h"
#include "adt/option.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"

/**
 * @brief A parsed frame view.
 *
 * After a successful parse the @c payload is a borrowed slice into the
 * source buffer; it does not own the bytes and is invalidated by any
 * mutation of the source buffer.  Copy out before further mutation.
 *
 * @c consumed is the total number of bytes the frame occupies in the
 * source buffer including the varint header and type byte; the caller
 * advances by this amount to parse the next frame.
 */
typedef struct {
    uint8_t        type;       /**< Type tag from the source buffer. */
    const uint8_t *payload;    /**< Borrowed pointer into source buffer. */
    size_t         payload_len;/**< Bytes of payload (matches encoded varint). */
    size_t         consumed;   /**< Total encoded frame size (header + type + payload). */
} n00b_quic_frame_t;

/* ===========================================================================
 * Varint primitives (RFC 9000 §16)
 *
 * Exposed publicly because the codec layer (Phase 4) and qlog (Phase 1)
 * both need to encode / decode varints in their own buffers; reusing
 * one tested implementation everywhere is cheaper than reinventing.
 * =========================================================================== */

/**
 * @brief Compute the encoded size of a QUIC varint, in bytes.
 *
 * Picks the smallest encoding that fits @p value: 1 / 2 / 4 / 8 bytes for
 * values up to 2^6 - 1 / 2^14 - 1 / 2^30 - 1 / 2^62 - 1 respectively.
 *
 * @param value Value to encode.  Must be ≤ @c N00B_QUIC_VARINT_MAX.
 * @return Encoded length in bytes (1, 2, 4, or 8).  If @p value exceeds
 *         @c N00B_QUIC_VARINT_MAX, returns 0.
 *
 * @post  The returned size matches what @c n00b_quic_varint_encode would
 *        write for the same value.
 */
extern size_t n00b_quic_varint_size(uint64_t value);

/**
 * @brief Encode a QUIC varint into a fixed-size output buffer.
 *
 * Writes the smallest valid encoding for @p value; high two bits of the
 * first byte indicate the length (00 / 01 / 10 / 11).
 *
 * @param out      Destination byte buffer.
 * @param out_cap  Capacity of @p out in bytes.
 * @param value    Value to encode (0 ≤ @p value ≤ @c N00B_QUIC_VARINT_MAX).
 *
 * @return On success, the number of bytes written (1 / 2 / 4 / 8).
 *         On failure, a `n00b_quic_err_t` value: @c N00B_QUIC_ERR_INVALID_ARG
 *         if @p value is too large, @c N00B_QUIC_ERR_FRAME_TOO_LARGE if
 *         @p out_cap is insufficient.
 *
 * @pre @p out is non-nullptr.
 * @pre @p out_cap ≥ @c n00b_quic_varint_size(value).
 */
extern n00b_result_t(size_t)
    n00b_quic_varint_encode(uint8_t *out, size_t out_cap, uint64_t value);

/**
 * @brief Decode a QUIC varint from a fixed-size input buffer.
 *
 * On success, writes the decoded value to @p out_value (if non-nullptr)
 * and returns the number of bytes consumed.  On a truncated input the
 * function returns an "ok with zero consumed" sentinel via
 * @c n00b_option_t — the caller treats that as "need more data".
 *
 * @param in        Source byte buffer.
 * @param in_len    Bytes available in @p in.
 * @param out_value Destination for the decoded value (may be nullptr to
 *                  query encoded size only).
 *
 * @return Result of an option of bytes consumed: ok+some(N) on a complete
 *         varint, ok+none on truncated input (more bytes needed),
 *         err(@c N00B_QUIC_ERR_BAD_VARINT) on a malformed encoding.  Note
 *         RFC 9000 §16 disallows non-canonical encodings only at the
 *         length-class level (you can encode 0 in any of 1/2/4/8 bytes);
 *         we accept any class.
 *
 * @pre @p in is non-nullptr if @p in_len > 0.
 */
extern n00b_result_t(n00b_option_t(size_t))
    n00b_quic_varint_decode(const uint8_t *in, size_t in_len, uint64_t *out_value);

/* ===========================================================================
 * Frame primitives
 * =========================================================================== */

/**
 * @brief Append one encoded frame to @p out.
 *
 * Encodes `varint(payload_len) || type || payload` and appends to @p out.
 * The buffer is grown as needed.  Refuses if the resulting frame would
 * exceed `.max_size`.
 *
 * @param out     Destination buffer (mutable; grown if needed).
 * @param type    Frame type byte (see ranges in @c quic_types.h).
 * @param payload Payload bytes.  May be nullptr only if its length is 0.
 * @param payload_len Number of payload bytes.
 *
 * @kw max_size Maximum total encoded frame size in bytes; default
 *              @c N00B_QUIC_DEFAULT_MAX_FRAME_SIZE.  Hard cap of 2^62 - 1
 *              regardless (RFC 9000 varint limit).
 *
 * @return Result of bool: ok(true) on success;
 *         err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if frame would exceed
 *         the cap;
 *         err(@c N00B_QUIC_ERR_NULL_ARG) if @p out is nullptr or
 *         @p payload is nullptr while @p payload_len > 0.
 *
 * @pre  @p out has been initialized via @c n00b_buffer_init.
 * @post On success, @c n00b_buffer_len(out) increases by exactly the
 *       encoded frame size.
 */
extern n00b_result_t(bool)
    n00b_quic_frame_emit(n00b_buffer_t *out,
                         uint8_t        type,
                         const uint8_t *payload,
                         size_t         payload_len)
    _kargs {
        size_t max_size = N00B_QUIC_DEFAULT_MAX_FRAME_SIZE;
    };

/**
 * @brief Try to parse one frame from @p in starting at @p offset.
 *
 * The successful parse populates a @c n00b_quic_frame_t whose @c payload
 * pointer borrows into @p in's storage; copy out before mutating @p in.
 *
 * @param in      Source buffer.
 * @param offset  Byte offset to start parsing at.
 *
 * @kw max_size  Maximum allowed total frame size in bytes; default
 *               @c N00B_QUIC_DEFAULT_MAX_FRAME_SIZE.  An advertised length
 *               that would push the frame past this cap is a hard error.
 *
 * @return Result of option of frame:
 *         - ok+some(frame) on a complete parse;
 *         - ok+none if the input does not yet contain a complete frame
 *           (caller waits for more bytes);
 *         - err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if the advertised length
 *           exceeds the cap;
 *         - err(@c N00B_QUIC_ERR_BAD_VARINT) if the varint is malformed;
 *         - err(@c N00B_QUIC_ERR_NULL_ARG) if @p in is nullptr;
 *         - err(@c N00B_QUIC_ERR_INVALID_ARG) if @p offset > buffer length.
 *
 * @pre  @p in has been initialized.
 */
extern n00b_result_t(n00b_option_t(n00b_quic_frame_t))
    n00b_quic_frame_parse(n00b_buffer_t *in, size_t offset)
    _kargs {
        size_t max_size = N00B_QUIC_DEFAULT_MAX_FRAME_SIZE;
    };
