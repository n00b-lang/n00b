/*
 * rpc_status.c — gRPC-numeric RPC status codes + helpers.
 *
 * Phase 4 § 4.10.  Pure functions; no allocations.
 */

#define N00B_USE_INTERNAL_API
#include "n00b.h"
#include "net/quic/quic_types.h"
#include "net/quic/rpc_status.h"

const char *
n00b_rpc_status_str(n00b_rpc_status_t s)
{
    switch (s) {
    case N00B_RPC_OK:                  return "OK";
    case N00B_RPC_CANCELLED:           return "CANCELLED";
    case N00B_RPC_UNKNOWN:             return "UNKNOWN";
    case N00B_RPC_INVALID_ARGUMENT:    return "INVALID_ARGUMENT";
    case N00B_RPC_DEADLINE_EXCEEDED:   return "DEADLINE_EXCEEDED";
    case N00B_RPC_NOT_FOUND:           return "NOT_FOUND";
    case N00B_RPC_ALREADY_EXISTS:      return "ALREADY_EXISTS";
    case N00B_RPC_PERMISSION_DENIED:   return "PERMISSION_DENIED";
    case N00B_RPC_RESOURCE_EXHAUSTED:  return "RESOURCE_EXHAUSTED";
    case N00B_RPC_FAILED_PRECONDITION: return "FAILED_PRECONDITION";
    case N00B_RPC_ABORTED:             return "ABORTED";
    case N00B_RPC_OUT_OF_RANGE:        return "OUT_OF_RANGE";
    case N00B_RPC_UNIMPLEMENTED:       return "UNIMPLEMENTED";
    case N00B_RPC_INTERNAL:            return "INTERNAL";
    case N00B_RPC_UNAVAILABLE:         return "UNAVAILABLE";
    case N00B_RPC_DATA_LOSS:           return "DATA_LOSS";
    case N00B_RPC_UNAUTHENTICATED:     return "UNAUTHENTICATED";
    }
    return "unknown";
}

int
n00b_rpc_status_http_class(n00b_rpc_status_t s)
{
    /* Following the gRPC-over-HTTP/2 mapping convention (RFC 7540
     * conventions used in grpc-web / grpc-gateway).  We use
     * coarser HTTP statuses than gRPC's official table because
     * H3's :status header is informational only (the precise
     * meaning is in n00b-rpc-status); browsers and proxies see
     * something sensible. */
    switch (s) {
    case N00B_RPC_OK:                  return 200;
    case N00B_RPC_INVALID_ARGUMENT:
    case N00B_RPC_FAILED_PRECONDITION:
    case N00B_RPC_OUT_OF_RANGE:        return 400;
    case N00B_RPC_UNAUTHENTICATED:     return 401;
    case N00B_RPC_PERMISSION_DENIED:   return 403;
    case N00B_RPC_NOT_FOUND:           return 404;
    case N00B_RPC_ALREADY_EXISTS:
    case N00B_RPC_ABORTED:             return 409;
    case N00B_RPC_RESOURCE_EXHAUSTED:  return 429;
    case N00B_RPC_CANCELLED:           return 499;  /* nginx convention */
    case N00B_RPC_UNIMPLEMENTED:       return 501;
    case N00B_RPC_UNAVAILABLE:         return 503;
    case N00B_RPC_DEADLINE_EXCEEDED:   return 504;
    case N00B_RPC_UNKNOWN:
    case N00B_RPC_INTERNAL:
    case N00B_RPC_DATA_LOSS:
    default:                           return 500;
    }
}

n00b_rpc_status_t
n00b_rpc_status_from_quic_err(int err)
{
    switch (err) {
    case N00B_QUIC_OK:                       return N00B_RPC_OK;
    case N00B_QUIC_ERR_TIMEOUT:              return N00B_RPC_DEADLINE_EXCEEDED;
    case N00B_QUIC_ERR_FLOW_BLOCKED:         return N00B_RPC_RESOURCE_EXHAUSTED;
    case N00B_QUIC_ERR_FRAME_TOO_LARGE:      return N00B_RPC_INVALID_ARGUMENT;
    case N00B_QUIC_ERR_BAD_VARINT:
    case N00B_QUIC_ERR_BAD_TYPE:
    case N00B_QUIC_ERR_PROTOCOL:             return N00B_RPC_INVALID_ARGUMENT;
    case N00B_QUIC_ERR_NOT_IMPLEMENTED:      return N00B_RPC_UNIMPLEMENTED;
    case N00B_QUIC_ERR_PEER_CLOSED:
    case N00B_QUIC_ERR_LOCAL_RESET:
    case N00B_QUIC_ERR_PEER_RESET:           return N00B_RPC_CANCELLED;
    case N00B_QUIC_ERR_BIND_FAILED:
    case N00B_QUIC_ERR_HANDSHAKE:
    case N00B_QUIC_ERR_TRUST_REJECTED:       return N00B_RPC_UNAVAILABLE;
    case N00B_QUIC_ERR_AUTH_TOKEN_MISSING:
    case N00B_QUIC_ERR_AUTH_TOKEN_INVALID:
    case N00B_QUIC_ERR_AUTH_TOKEN_EXPIRED:
    case N00B_QUIC_ERR_AUTH_KEY_NOT_FOUND:
    case N00B_QUIC_ERR_AUTH_ALG_REFUSED:
    case N00B_QUIC_ERR_AUTH_ISS_MISMATCH:
    case N00B_QUIC_ERR_AUTH_AUD_MISMATCH:
    case N00B_QUIC_ERR_AUTH_DPOP_FAILED:
    case N00B_QUIC_ERR_AUTH_REPLAY_DETECTED: return N00B_RPC_UNAUTHENTICATED;
    case N00B_QUIC_ERR_AUTH_MTLS_MISMATCH:   return N00B_RPC_PERMISSION_DENIED;
    case N00B_QUIC_ERR_INVALID_ARG:
    case N00B_QUIC_ERR_NULL_ARG:             return N00B_RPC_INVALID_ARGUMENT;
    default:
        if (err == 0) return N00B_RPC_OK;
        return N00B_RPC_INTERNAL;
    }
}
