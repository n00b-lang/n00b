/**
 * @file nfs_rpc.h
 * @brief Minimal ONC RPC (RFC 5531) framing for NFSv3.
 *
 * Handles TCP record marking (4-byte length prefix with last-fragment bit),
 * RPC message header parsing/building, and program dispatch.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// RPC constants
// ============================================================================

#define N00B_RPC_CALL          0
#define N00B_RPC_REPLY         1
#define N00B_RPC_MSG_ACCEPTED  0
#define N00B_RPC_SUCCESS       0
#define N00B_RPC_PROG_UNAVAIL  1
#define N00B_RPC_PROG_MISMATCH 2
#define N00B_RPC_PROC_UNAVAIL  3

// NFS program numbers.
#define N00B_NFS_PROGRAM   100003
#define N00B_NFS_VERSION   3
#define N00B_MOUNT_PROGRAM 100005
#define N00B_MOUNT_VERSION 3

// ============================================================================
// RPC header (parsed from a CALL message)
// ============================================================================

typedef struct {
    uint32_t xid;
    uint32_t program;
    uint32_t version;
    uint32_t procedure;
} n00b_rpc_call_hdr_t;

// ============================================================================
// TCP record marking
// ============================================================================

/** @brief The last-fragment bit in the record marking header. */
#define N00B_RPC_LAST_FRAG (1u << 31)

/**
 * @brief Encode a record-marking header.
 * @param len   Payload length (must be < 2^31).
 * @param last  True if this is the last (or only) fragment.
 */
static inline uint32_t
n00b_rpc_rm_encode(uint32_t len, bool last)
{
    return last ? (len | N00B_RPC_LAST_FRAG) : len;
}

/**
 * @brief Decode a record-marking header.
 * @param rm    Raw 4-byte value (big-endian).
 * @param len   Output: payload length.
 * @param last  Output: true if last fragment.
 */
static inline void
n00b_rpc_rm_decode(uint32_t rm, uint32_t *len, bool *last)
{
    *last = (rm & N00B_RPC_LAST_FRAG) != 0;
    *len  = rm & ~N00B_RPC_LAST_FRAG;
}
