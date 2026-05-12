/**
 * @file qpack.h
 * @brief QPACK header compression (RFC 9204) — encoder + decoder.
 *
 * QPACK is HTTP/3's header compression scheme.  It is conceptually
 * similar to HPACK (RFC 7541) but designed for QUIC's stream-multiplexed,
 * out-of-order delivery model: the dynamic table is updated on a
 * dedicated unidirectional encoder stream rather than inline with the
 * field section, and the decoder confirms reception via a dedicated
 * unidirectional decoder stream.
 *
 * ## Architectural map
 *
 * The QPACK module ships three independently testable pieces:
 *
 * - **Static table** (RFC 9204 Appendix A; 99 entries).  Read-only,
 *   shared between encoder and decoder by reference.
 * - **Dynamic table** (mutex-protected; per-connection).  Encoder
 *   adds entries via the encoder stream; decoder mirrors by
 *   processing those same bytes.
 * - **Huffman codec** (RFC 7541 Appendix B; QPACK reuses HPACK's
 *   table).  Stateless functions; no per-connection allocation.
 *
 * The encoder + decoder are deliberately decoupled from any HTTP/3
 * stack: feed bytes in, get bytes (or fields) out.  This makes them
 * trivially testable (`test/unit/test_quic_qpack.c`) and re-usable
 * by anyone with a QUIC stream — including the H3 module that lands
 * in sub-phase 4.3.
 *
 * ## Wire shapes (RFC 9204 § 4)
 *
 * **Encoded field section** — emitted on a request/push stream:
 *
 * ```
 * Required Insert Count (prefix-int 8)
 * S | Delta Base       (prefix-int 7, S = sign bit)
 * Field-line representations [...]
 * ```
 *
 * **Encoder stream** — uni stream type 0x02:
 *
 * ```
 * 1xxxxxxx          : Insert with Name Reference (prefix-int 6)
 * 01xxxxxx          : Insert with Literal Name (prefix-int 5)
 * 001xxxxx          : Set Dynamic Table Capacity (prefix-int 5)
 * 000xxxxx          : Duplicate (prefix-int 5)
 * ```
 *
 * **Decoder stream** — uni stream type 0x03:
 *
 * ```
 * 1xxxxxxx          : Section Acknowledgment (prefix-int 7)
 * 01xxxxxx          : Stream Cancellation (prefix-int 6)
 * 00xxxxxx          : Insert Count Increment (prefix-int 6)
 * ```
 *
 * Each prefix-int uses RFC 7541 § 5.1's variable-length encoding.
 *
 * ## Memory + threading
 *
 * Both `n00b_qpack_encoder_t` and `n00b_qpack_decoder_t` are
 * conduit-pool allocated.  Internal state is mutex-protected; the
 * encoder + decoder may be called concurrently by the QUIC IO
 * thread (encoder-stream byte arrival, decoder-stream byte arrival)
 * and by application code (per-request encode/decode).
 *
 * ## Hard caps (DoS defenses)
 *
 * - `N00B_QPACK_MAX_FIELD_LINE` — 64 KiB per name+value.  Anything
 *   larger is rejected with `N00B_QUIC_ERR_FRAME_TOO_LARGE`.
 * - `N00B_QPACK_MAX_FIELDS_PER_SECTION` — 1024 fields.
 * - `N00B_QPACK_MAX_DYNAMIC_CAPACITY` — 1 MiB hard ceiling on the
 *   advertised dynamic table size; protects against a peer trying
 *   to make us allocate gigabytes.
 *
 * @see ~/dd/quic_4.md § 5 (sub-phase 4.2)
 * @see RFC 9204 (https://www.rfc-editor.org/rfc/rfc9204)
 * @see RFC 7541 § 5 + Appendix B (the integer / string encodings
 *      and Huffman table that QPACK reuses)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "n00b.h"
#include "adt/option.h"
#include "adt/result.h"
#include "core/buffer.h"
#include "net/quic/quic_types.h"

/* ===========================================================================
 * Hard caps
 * =========================================================================== */

/** @brief Maximum bytes for a single header name or value. */
#define N00B_QPACK_MAX_FIELD_LINE         ((size_t)(64u * 1024u))

/** @brief Maximum number of fields in one encoded field section. */
#define N00B_QPACK_MAX_FIELDS_PER_SECTION ((size_t)1024u)

/**
 * @brief Hard ceiling on dynamic-table capacity (bytes).
 *
 * Protects against a misbehaving peer asking us to back a
 * multi-gigabyte table.  RFC 9204 § 3.2.3 lets the encoder ask the
 * decoder for any capacity ≤ the decoder's `SETTINGS_QPACK_MAX_TABLE_CAPACITY`
 * advertisement; we enforce this at construction time.
 */
#define N00B_QPACK_MAX_DYNAMIC_CAPACITY   ((uint64_t)(1u * 1024u * 1024u))

/** @brief Number of static-table entries (RFC 9204 Appendix A). */
#define N00B_QPACK_STATIC_TABLE_SIZE      ((size_t)99u)

/* ===========================================================================
 * Public types
 * =========================================================================== */

/**
 * @brief Decoded header field (name + value).
 *
 * Both `name` and `value` are NUL-terminated for ergonomic
 * inspection, but lengths are tracked separately because header
 * values may legitimately contain bytes ≥ 0x80 once Huffman decoded.
 *
 * Decoded fields are conduit-pool allocated; their lifetime matches
 * the field-section list returned by `n00b_qpack_decode`.
 */
typedef struct {
    const uint8_t *name;       /**< Lowercase ASCII name bytes. */
    size_t         name_len;   /**< Bytes in @c name (no NUL). */
    const uint8_t *value;      /**< Value bytes (may contain non-ASCII). */
    size_t         value_len;  /**< Bytes in @c value (no NUL). */
} n00b_qpack_field_t;

/**
 * @brief Opaque encoder handle.
 *
 * Owns one half of the encoder/decoder mirror state for one QUIC
 * connection.  Allocated from `conduit_pool`.
 */
typedef struct n00b_qpack_encoder n00b_qpack_encoder_t;

/**
 * @brief Opaque decoder handle.
 *
 * Owns the other half of the mirror state.  Allocated from
 * `conduit_pool`.
 */
typedef struct n00b_qpack_decoder n00b_qpack_decoder_t;

/* ===========================================================================
 * Encoder API
 * =========================================================================== */

/**
 * @brief Allocate a new encoder.
 *
 * @param max_table_capacity   Upper bound on dynamic-table size in
 *                             bytes.  Capped at
 *                             @c N00B_QPACK_MAX_DYNAMIC_CAPACITY.
 *                             Pass 0 to disable the dynamic table
 *                             entirely (static-only encoding).
 * @param max_blocked_streams  Maximum number of request streams the
 *                             encoder is willing to leave blocked
 *                             on a not-yet-acked dynamic insertion.
 *                             0 disables blocking encodings.
 *
 * @return New encoder handle, never nullptr.
 */
extern n00b_qpack_encoder_t *
n00b_qpack_encoder_new(uint64_t max_table_capacity, uint64_t max_blocked_streams);

/**
 * @brief Release the encoder and its dynamic-table backing store.
 *
 * @param enc Encoder; nullptr-safe.
 */
extern void n00b_qpack_encoder_close(n00b_qpack_encoder_t *enc);

/**
 * @brief Encode a list of header fields for one request/push stream.
 *
 * Produces:
 * - The encoded **field section** appended to @p out_field_section
 *   (this is what gets stuffed inside the H3 HEADERS frame body).
 * - Optional encoder-stream insertions appended to
 *   @p out_encoder_stream (bytes the caller should write to its
 *   uni stream of type 0x02 — order-preserving across calls).
 *
 * @param enc                 Encoder.
 * @param stream_id           Request/push stream ID; tracked for
 *                            ack accounting.
 * @param fields              Array of fields to encode.
 * @param n_fields            Length of @p fields.
 * @param out_field_section   Buffer to append the field section to.
 * @param out_encoder_stream  Buffer to append encoder-stream bytes
 *                            to (may be nullptr if dynamic table
 *                            capacity is 0).
 *
 * @return ok(true) on success;
 *         err(@c N00B_QUIC_ERR_INVALID_ARG) on bad inputs;
 *         err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if a name/value
 *         exceeds @c N00B_QPACK_MAX_FIELD_LINE.
 *
 * @pre  Each `fields[i].name_len` ≤ @c N00B_QPACK_MAX_FIELD_LINE.
 * @post On error, @p out_field_section and @p out_encoder_stream are
 *       restored to their pre-call lengths.
 */
extern n00b_result_t(bool)
    n00b_qpack_encode(n00b_qpack_encoder_t      *enc,
                      uint64_t                   stream_id,
                      const n00b_qpack_field_t  *fields,
                      size_t                     n_fields,
                      n00b_buffer_t             *out_field_section,
                      n00b_buffer_t             *out_encoder_stream);

/**
 * @brief Feed decoder-stream bytes into the encoder.
 *
 * Processes Section Acknowledgment / Stream Cancellation / Insert
 * Count Increment messages (RFC 9204 § 4.4).  Updates the encoder's
 * "known received count" used to gate blocked encodings.
 *
 * @param enc       Encoder.
 * @param data      Decoder-stream bytes received from peer.
 * @param data_len  Number of bytes in @p data.
 *
 * @return ok(consumed) on success — the number of bytes successfully
 *         processed.  If the trailing bytes form a partial message,
 *         the caller should keep them and call again with more bytes.
 *         err(@c N00B_QUIC_ERR_PROTOCOL) on a malformed message.
 */
extern n00b_result_t(size_t)
    n00b_qpack_encoder_consume_decoder_stream(n00b_qpack_encoder_t *enc,
                                              const uint8_t        *data,
                                              size_t                data_len);

/**
 * @brief Update the encoder's dynamic-table capacity.
 *
 * Emits a Set Dynamic Table Capacity instruction onto
 * @p out_encoder_stream (if non-nullptr).  Evicts existing entries
 * if the new capacity is smaller than the current usage.
 *
 * @param enc                 Encoder.
 * @param new_capacity        Capacity in RFC bytes (≤
 *                            @c N00B_QPACK_MAX_DYNAMIC_CAPACITY).
 * @param out_encoder_stream  Buffer to append the instruction to;
 *                            may be nullptr (test-mode caller).
 *
 * @return ok(true) on success;
 *         err(@c N00B_QUIC_ERR_INVALID_ARG) if @p new_capacity exceeds
 *         the encoder's maximum.
 */
extern n00b_result_t(bool)
    n00b_qpack_encoder_set_capacity(n00b_qpack_encoder_t *enc,
                                    uint64_t              new_capacity,
                                    n00b_buffer_t        *out_encoder_stream);

/**
 * @brief Number of dynamic-table insertions performed so far.
 *
 * Monotonic counter; identical on encoder and decoder once the
 * decoder has processed all encoder-stream bytes.
 */
extern uint64_t n00b_qpack_encoder_insert_count(n00b_qpack_encoder_t *enc);

/**
 * @brief Number of insertions the peer has acknowledged.
 *
 * Always ≤ the value of `n00b_qpack_encoder_insert_count`.  The
 * difference is the number of in-flight insertions.
 */
extern uint64_t n00b_qpack_encoder_known_received_count(n00b_qpack_encoder_t *enc);

/* ===========================================================================
 * Decoder API
 * =========================================================================== */

/**
 * @brief Allocate a new decoder.
 *
 * @param max_table_capacity   Same role as encoder; bounds the
 *                             dynamic-table backing store.
 * @param max_blocked_streams  Maximum streams allowed to be blocked
 *                             waiting for dynamic-table inserts.
 *                             0 = encoder must never produce blocking
 *                             references.
 */
extern n00b_qpack_decoder_t *
n00b_qpack_decoder_new(uint64_t max_table_capacity, uint64_t max_blocked_streams);

/** @brief Release the decoder; nullptr-safe. */
extern void n00b_qpack_decoder_close(n00b_qpack_decoder_t *dec);

/**
 * @brief Feed encoder-stream bytes into the decoder.
 *
 * Mirrors the encoder's dynamic-table state.  Updates the
 * decoder's insert count and may emit Insert Count Increment
 * messages (appended to @p out_decoder_stream) per RFC 9204 § 4.4.2.
 *
 * @param dec                Decoder.
 * @param data               Bytes received on the encoder stream.
 * @param data_len           Length of @p data.
 * @param out_decoder_stream Buffer that receives the resulting
 *                           Insert Count Increment messages, if
 *                           any.  May be nullptr.
 *
 * @return ok(consumed) — bytes successfully processed.  Any
 *         trailing partial instruction stays unconsumed.
 *         err(@c N00B_QUIC_ERR_PROTOCOL) on malformed input.
 */
extern n00b_result_t(size_t)
    n00b_qpack_decoder_consume_encoder_stream(n00b_qpack_decoder_t *dec,
                                              const uint8_t        *data,
                                              size_t                data_len,
                                              n00b_buffer_t        *out_decoder_stream);

/**
 * @brief Decode an encoded field section.
 *
 * @param dec                Decoder.
 * @param stream_id          Request/push stream ID.  Section
 *                           Acknowledgment will reference this id.
 * @param data               Bytes of the encoded field section.
 * @param data_len           Length of @p data.
 * @param out_fields         Destination — caller-provided array of
 *                           at least `out_fields_cap` slots.
 * @param out_fields_cap     Capacity of @p out_fields.
 * @param out_n_fields       On success, populated with the number of
 *                           decoded fields.
 * @param out_decoder_stream Buffer receiving any decoder-stream
 *                           messages (Section Ack / Insert Count
 *                           Increment / Stream Cancellation).  May
 *                           be nullptr; if so, no acks are emitted
 *                           (useful for unit-test purposes).
 *
 * @return ok(true) on success;
 *         err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if too many fields;
 *         err(@c N00B_QUIC_ERR_PROTOCOL) on malformed input or
 *         out-of-bounds index references;
 *         err(@c N00B_QUIC_ERR_NEED_MORE_DATA) if the field section
 *         references dynamic-table entries we haven't received yet.
 *
 * @post Decoded field bytes are conduit-pool allocated and remain
 *       valid as long as the decoder handle is alive (or until
 *       another `n00b_qpack_decode` overwrites the same buffer
 *       slot — implementation reserves no state across calls; each
 *       call freshly allocates).
 */
extern n00b_result_t(bool)
    n00b_qpack_decode(n00b_qpack_decoder_t *dec,
                      uint64_t              stream_id,
                      const uint8_t        *data,
                      size_t                data_len,
                      n00b_qpack_field_t   *out_fields,
                      size_t                out_fields_cap,
                      size_t               *out_n_fields,
                      n00b_buffer_t        *out_decoder_stream);

/** @brief Number of dynamic-table insertions the decoder has processed. */
extern uint64_t n00b_qpack_decoder_insert_count(n00b_qpack_decoder_t *dec);

/* ===========================================================================
 * Huffman codec (RFC 7541 Appendix B)
 *
 * Exposed publicly because:
 *   - Tests can hit it directly (Appendix-D vectors).
 *   - The H3 server may want to Huffman-encode trailers without
 *     going through the full QPACK path.
 * =========================================================================== */

/**
 * @brief Compute the encoded length, in bytes, of @p src.
 *
 * @param src     Source bytes.
 * @param src_len Number of bytes in @p src.
 * @return        Bytes the encoded form would take.
 */
extern size_t
n00b_qpack_huffman_encoded_size(const uint8_t *src, size_t src_len);

/**
 * @brief Huffman-encode @p src into @p dst.
 *
 * @param src      Source bytes.
 * @param src_len  Bytes in @p src.
 * @param dst      Destination (must have capacity ≥
 *                 @c n00b_qpack_huffman_encoded_size(src,src_len)).
 * @param dst_cap  Capacity of @p dst.
 *
 * @return ok(written) — number of bytes written.
 *         err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if @p dst_cap is too small.
 */
extern n00b_result_t(size_t)
    n00b_qpack_huffman_encode(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap);

/**
 * @brief Huffman-decode @p src into @p dst.
 *
 * The decoder enforces RFC 7541 § 5.2's padding rule: any padding
 * shorter than the longest code must match the EOS prefix; any
 * embedded EOS or padding longer than 7 bits is a hard error.
 *
 * @param src      Source bytes (Huffman-encoded).
 * @param src_len  Bytes in @p src.
 * @param dst      Destination buffer.
 * @param dst_cap  Capacity of @p dst.
 *
 * @return ok(written) — number of decoded bytes.
 *         err(@c N00B_QUIC_ERR_PROTOCOL) on bad padding or embedded EOS.
 *         err(@c N00B_QUIC_ERR_FRAME_TOO_LARGE) if @p dst_cap is too small.
 */
extern n00b_result_t(size_t)
    n00b_qpack_huffman_decode(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_cap);

/* ===========================================================================
 * Static-table introspection (mostly for tests + diagnostics)
 * =========================================================================== */

/**
 * @brief Look up a static-table entry by index (0..98).
 *
 * @param idx  Index in [0, @c N00B_QPACK_STATIC_TABLE_SIZE).
 * @param[out] out_field  Populated with name/value views into the
 *                        static (read-only) table; remain valid
 *                        for the program's lifetime.
 *
 * @return true if @p idx is in range and @p out_field was populated;
 *         false otherwise.
 */
extern bool
n00b_qpack_static_lookup(size_t idx, n00b_qpack_field_t *out_field);
