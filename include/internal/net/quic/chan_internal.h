/**
 * @file chan_internal.h
 * @internal
 * @brief Layout of `n00b_quic_chan_t`.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "n00b.h"
#include "net/quic/quic_types.h"

/* Forward decl: opaque outside of quic/auth_policy.h. */
struct n00b_quic_auth_policy;

struct n00b_quic_chan {
    n00b_quic_conn_t       *conn;        /**< Owning connection; borrowed. */
    n00b_quic_chan_t       *next_in_conn;/**< Intrusive list link; conn-owned. */
    uint64_t                stream_id;   /**< Picoquic-assigned stream ID. */
    n00b_quic_chan_kind_t   kind;
    n00b_quic_chan_state_t  state;
    /* Phase 3 § 10: declarative auth policy attached at chan_open
     * time.  Borrowed; lifetime is the application's. */
    struct n00b_quic_auth_policy *auth_policy;
    uint64_t                bytes_sent;
    uint64_t                bytes_received;
    uint64_t                app_err_local;
    uint64_t                app_err_peer;
    uint64_t                last_activity_ns;

    /* Recv buffer.  Bytes received from picoquic on this stream
     * accumulate here until the application drains them via
     * `n00b_quic_chan_recv`.  Lazily allocated from the conduit pool
     * on first append.  Used by FRAMED and BYTES kinds only. */
    n00b_buffer_t          *recv_buf;

    /* DGRAM-only: queue of received datagram payloads.  Each entry
     * is one complete datagram (RFC 9221 atomic delivery semantics
     * — no concatenation, no fragmentation).  The list is a struct
     * (value-semantic — n00b_list_t isn't a pointer type); it gets
     * initialized when the singleton DGRAM channel is created in
     * `_n00b_quic_conn_dgram_chan`.  Unused on stream-kind channels. */
    n00b_list_t(n00b_buffer_t *)  dgram_recv_queue;
    bool                          dgram_queue_inited;

    bool                    bidi;
    bool                    sent_fin;    /**< Local FIN already sent. */
    bool                    recv_fin;    /**< Peer FIN seen. */
    bool                    closed;      /**< Local close called. */
};

/**
 * @internal
 * @brief Append a received datagram payload to a DGRAM channel's queue.
 *
 * Each call pushes one whole datagram; the application pops them one
 * at a time via `n00b_quic_chan_recv_dgram`.  Called from the
 * picoquic per-cnx callback when a `picoquic_callback_datagram` event
 * fires.
 */
extern void
_n00b_quic_chan_push_dgram(n00b_quic_chan_t *chan,
                           const uint8_t    *bytes,
                           size_t            len);

/**
 * @internal
 * @brief Append bytes received from picoquic onto a channel's recv buffer.
 *
 * Grows the buffer geometrically as needed.  Callers (the per-cnx
 * stream callback) must hold no other channel locks during this call.
 */
extern void
_n00b_quic_chan_append_recv(n00b_quic_chan_t *chan,
                            const uint8_t    *bytes,
                            size_t            len);
