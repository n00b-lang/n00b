/**
 * @file rpc_status.h
 * @brief n00b RPC status codes (gRPC-numeric for ease of bridging).
 *
 * Phase 4 § 4.10.  Numeric values match gRPC's status-code
 * enumeration so that an `n00b_rpc <-> gRPC` bridge (third-party
 * follow-up; not in v1 scope) can map one-to-one without
 * translation tables.
 *
 * Wire format (Phase 4 § 7 / docs/quic/rpc.md):
 *   Response carries an `n00b-rpc-status: <int>` HTTP/3 header
 *   with the integer value of the enum, plus optional
 *   `n00b-rpc-status-detail: <free-form text>`.  The HTTP `:status`
 *   pseudo-header is set to a coarse-grained transport-level
 *   value (200 for OK, 4xx for client-side, 5xx for server-side)
 *   per `n00b_rpc_status_http_class`.
 *
 * Application errors (handler returned err) → `n00b-rpc-status` set
 * to a non-zero value + appropriate `:status`.  Transport errors
 * (truncated body, alg refused, ...) → `RESET_STREAM` with the
 * matching H3 / QUIC error code.
 *
 * @see ~/dd/quic_4.md § 4.10 + § 10
 */
#pragma once

#include <stdint.h>

#include "n00b.h"

/**
 * @brief RPC status code.
 *
 * Numeric values mirror gRPC's `google.rpc.Code`:
 * https://grpc.io/docs/guides/status-codes/
 */
typedef enum : int32_t {
    N00B_RPC_OK                  = 0,
    N00B_RPC_CANCELLED           = 1,
    N00B_RPC_UNKNOWN             = 2,
    N00B_RPC_INVALID_ARGUMENT    = 3,
    N00B_RPC_DEADLINE_EXCEEDED   = 4,
    N00B_RPC_NOT_FOUND           = 5,
    N00B_RPC_ALREADY_EXISTS      = 6,
    N00B_RPC_PERMISSION_DENIED   = 7,
    N00B_RPC_RESOURCE_EXHAUSTED  = 8,
    N00B_RPC_FAILED_PRECONDITION = 9,
    N00B_RPC_ABORTED             = 10,
    N00B_RPC_OUT_OF_RANGE        = 11,
    N00B_RPC_UNIMPLEMENTED       = 12,
    N00B_RPC_INTERNAL            = 13,
    N00B_RPC_UNAVAILABLE         = 14,
    N00B_RPC_DATA_LOSS           = 15,
    N00B_RPC_UNAUTHENTICATED     = 16,
} n00b_rpc_status_t;

/**
 * @brief Stable, human-readable status name (for logs + audit).
 *
 * Returns a statically-allocated string; never NULL.  Codes
 * outside the enum range produce the sentinel "unknown".
 *
 * @param s  Status code.
 * @return   Status name, e.g. "OK", "PERMISSION_DENIED".
 */
extern const char *n00b_rpc_status_str(n00b_rpc_status_t s);

/**
 * @brief Coarse-grained HTTP `:status` for a given RPC status.
 *
 * Used by the H3 wire layer (Phase 4 § 7) to set the `:status`
 * pseudo-header alongside the more-precise `n00b-rpc-status`.
 *
 * @param s  RPC status.
 * @return   3-digit HTTP status integer (200, 400, 403, 404,
 *           500, 503, 504, ...).
 */
extern int n00b_rpc_status_http_class(n00b_rpc_status_t s);

/**
 * @brief Map a Phase 3 `n00b_quic_err_t` (auth/transport error)
 *        onto the closest RPC status.
 *
 * Used by the auth-policy wiring (Phase 4 § 9) when an inbound
 * RPC's policy eval returns an error: the runtime maps the
 * QUIC-level err code into an RPC-level status the wire encodes.
 *
 * @param err  A negative `n00b_quic_err_t` value.
 * @return     Closest RPC status; INTERNAL when nothing matches.
 */
extern n00b_rpc_status_t
n00b_rpc_status_from_quic_err(int err);
