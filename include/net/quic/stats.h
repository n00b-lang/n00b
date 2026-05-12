/**
 * @file stats.h
 * @brief Public stats snapshot structs for QUIC connections and channels.
 *
 * Stats are surfaced through poll-style accessors:
 *
 * ```c
 * n00b_quic_conn_stats_t   c = n00b_quic_conn_stats(conn);
 * n00b_quic_stream_stats_t s = n00b_quic_chan_stats(chan);
 * ```
 *
 * Per @c api_design.md Principle 12, these structs have only public fields
 * — no "internal use" markers, no opaque cookies.  Operators reading these
 * snapshots should be able to identify failure modes without attaching a
 * debugger:
 *
 * - **Stuck connection-level window with healthy stream window** —
 *   @c send_window > 0 on a channel while @c conn_send_window == 0 on its
 *   connection.
 * - **Long-lived write-blocked stream** — @c write_blocked_us large
 *   relative to channel age.
 * - **Asymmetric loss / reset** — @c app_err_local set without
 *   @c app_err_peer (we reset; peer didn't), or vice versa.
 *
 * The snapshot is taken under the connection's read lock; values are
 * mutually consistent within one snapshot but not across multiple calls.
 *
 * @see chan.h, conn.h
 */
#pragma once

#include <stdint.h>
#include "net/quic/quic_types.h"

/**
 * @brief Per-channel snapshot.
 *
 * @c send_window / @c recv_window are the *per-stream* flow-control windows;
 * connection-level windows live in @c n00b_quic_conn_stats_t.  Two surfaces
 * intentionally — both can independently throttle progress, and operators
 * need to be able to tell which one is the culprit.
 */
typedef struct {
    uint64_t bytes_sent;        /**< Application bytes handed to picoquic for send. */
    uint64_t bytes_acked;       /**< Of those, bytes the peer has acknowledged. */
    uint64_t bytes_in_flight;   /**< bytes_sent - bytes_acked, capped at non-negative. */
    uint64_t bytes_received;    /**< Application bytes delivered to caller. */
    uint64_t send_window;       /**< Per-stream peer-advertised window remaining. */
    uint64_t recv_window;       /**< Per-stream window we are advertising. */
    uint64_t write_blocked_us;  /**< Cumulative microseconds spent blocked since open. */
    uint64_t last_activity_ns;  /**< Monotonic timestamp of last send/recv. */
    uint64_t app_err_local;     /**< Non-zero if reset locally, with the app error code. */
    uint64_t app_err_peer;      /**< Non-zero if reset by peer, with the app error code. */
    uint8_t  state;             /**< @c n00b_quic_chan_state_t. */
    uint8_t  kind;              /**< @c n00b_quic_chan_kind_t. */
    uint8_t  bidi;              /**< 1 = bidirectional, 0 = unidirectional. */
    uint8_t  reserved_[5];      /**< Padding; always zero. */
} n00b_quic_stream_stats_t;

/**
 * @brief Per-connection snapshot.
 *
 * Sufficient information for operators to identify CC misbehavior, head-of-
 * line blocking at the connection level, and outstanding loss/RTT issues.
 */
typedef struct {
    uint64_t conn_send_window;  /**< Bytes peer will accept across the connection right now. */
    uint64_t conn_recv_window;  /**< Bytes we will accept across the connection right now. */
    uint64_t rtt_us;            /**< Smoothed RTT estimate, microseconds. */
    uint64_t rttvar_us;         /**< RTT variance, microseconds. */
    uint64_t cwnd;              /**< Current congestion window, bytes. */
    uint64_t bytes_in_flight;   /**< Bytes sent but not acknowledged across the connection. */
    uint64_t packets_sent;      /**< Total packets sent on this connection. */
    uint64_t packets_lost;      /**< Total packets considered lost (retransmitted). */
    uint64_t packets_received;  /**< Total valid packets received. */
    uint64_t bytes_sent;        /**< Total wire bytes sent. */
    uint64_t bytes_received;    /**< Total wire bytes received. */
    uint64_t channels_open;     /**< Channels currently in OPEN or HALF_CLOSED state. */
    uint64_t channels_total;    /**< All channels ever opened on this connection. */
    uint8_t  cc_algo;           /**< @c n00b_quic_cc_algo_t. */
    uint8_t  reserved_[7];      /**< Padding; always zero. */
} n00b_quic_conn_stats_t;
