/**
 * @file lb_cid.h
 * @brief LB-CID encoding (draft-ietf-quic-load-balancers, block-cipher mode).
 *
 * QUIC connection IDs are normally opaque to L4 load balancers;
 * draft-ietf-quic-load-balancers specifies an encoding that lets the
 * LB peek into a CID and learn which server instance handles the
 * connection.  The block-cipher mode (the most secure of the three
 * the draft defines) encrypts a `<server_id>||<nonce>` plaintext with
 * AES-128 using a key shared between the LB and the servers.
 *
 * **Phase 2 v1 constraint**: CIDs are exactly 16 bytes (one AES
 * block).  The Feistel-network mode for non-16-byte CIDs that
 * picotls's openssl/fusion backends ship is not yet implemented for
 * minicrypto; this is a follow-up.  16-byte fixed is the most common
 * deployment shape and matches RFC 9000's max CID length.
 *
 * Threading: encode is called per-new-CID (rare — once per
 * connection on the fast path); a tiny mutex serializes access to
 * the underlying picotls cipher context.  Decode is also rare (LB
 * side only).
 *
 * @see ~/dd/quic_2.md § 8
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "n00b.h"
#include "adt/result.h"

#define N00B_QUIC_LB_CID_LEN 16  /* one AES-128 block */

typedef struct n00b_quic_lb_cid_config n00b_quic_lb_cid_config_t;

/**
 * @brief Build an LB-CID configuration.
 *
 * @param key            16-byte AES-128 key shared with the LB.
 * @param server_id      This instance's server-id value (low
 *                       @p server_id_len bytes are used; high bytes
 *                       are ignored).
 * @param server_id_len  Length of the server-id field in bytes
 *                       (1..15).  The remaining
 *                       `N00B_QUIC_LB_CID_LEN - server_id_len`
 *                       bytes of the plaintext are filled with a
 *                       fresh nonce on every encode.
 *
 * @return Owned config; close with @c n00b_quic_lb_cid_config_close.
 */
extern n00b_result_t(n00b_quic_lb_cid_config_t *)
n00b_quic_lb_cid_config_new(const uint8_t key[N00B_QUIC_LB_CID_LEN],
                            uint64_t      server_id,
                            uint8_t       server_id_len);

/**
 * @brief Generate a fresh encrypted CID.
 *
 * @param cfg          Configuration.
 * @param out          Receives N00B_QUIC_LB_CID_LEN encrypted bytes.
 *
 * @return Result: ok(true) on success.
 */
extern n00b_result_t(bool)
n00b_quic_lb_cid_encode(n00b_quic_lb_cid_config_t *cfg,
                        uint8_t                    out[N00B_QUIC_LB_CID_LEN]);

/**
 * @brief Decrypt a CID and extract the embedded server-id.
 *
 * Used on the LB side and for round-trip tests.  No validation —
 * the LB is responsible for checking that the decoded server-id is
 * one it knows about.
 *
 * @param cfg            Configuration.
 * @param encrypted_cid  N00B_QUIC_LB_CID_LEN bytes from a CID seen
 *                       on the wire.
 * @param server_id_out  Receives the embedded server-id (low
 *                       `server_id_len` bytes are meaningful; high
 *                       bytes are zeroed).
 */
extern n00b_result_t(bool)
n00b_quic_lb_cid_decode(n00b_quic_lb_cid_config_t *cfg,
                        const uint8_t              encrypted_cid[N00B_QUIC_LB_CID_LEN],
                        uint64_t                  *server_id_out);

/** @brief Close.  Idempotent. */
extern void
n00b_quic_lb_cid_config_close(n00b_quic_lb_cid_config_t *cfg);
