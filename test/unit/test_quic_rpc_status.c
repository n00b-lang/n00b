/*
 * test_quic_rpc_status.c — Phase 4 § 4.10.  RPC status enumeration
 * + name lookup + HTTP-class mapping + QUIC-err bridge.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "net/quic/quic_types.h"
#include "net/quic/rpc_status.h"

static void
test_status_str(void)
{
    /* Spot-check a few known names. */
    assert(strcmp(n00b_rpc_status_str(N00B_RPC_OK), "OK") == 0);
    assert(strcmp(n00b_rpc_status_str(N00B_RPC_CANCELLED), "CANCELLED") == 0);
    assert(strcmp(n00b_rpc_status_str(N00B_RPC_PERMISSION_DENIED),
                  "PERMISSION_DENIED") == 0);
    assert(strcmp(n00b_rpc_status_str(N00B_RPC_DEADLINE_EXCEEDED),
                  "DEADLINE_EXCEEDED") == 0);
    /* Out-of-range → "unknown". */
    assert(strcmp(n00b_rpc_status_str((n00b_rpc_status_t)9999), "unknown") == 0);
    printf("  [PASS] status_str names + out-of-range fallback\n");
}

static void
test_http_class(void)
{
    /* Coarse HTTP mapping. */
    assert(n00b_rpc_status_http_class(N00B_RPC_OK)                  == 200);
    assert(n00b_rpc_status_http_class(N00B_RPC_INVALID_ARGUMENT)    == 400);
    assert(n00b_rpc_status_http_class(N00B_RPC_UNAUTHENTICATED)     == 401);
    assert(n00b_rpc_status_http_class(N00B_RPC_PERMISSION_DENIED)   == 403);
    assert(n00b_rpc_status_http_class(N00B_RPC_NOT_FOUND)           == 404);
    assert(n00b_rpc_status_http_class(N00B_RPC_ALREADY_EXISTS)      == 409);
    assert(n00b_rpc_status_http_class(N00B_RPC_RESOURCE_EXHAUSTED)  == 429);
    assert(n00b_rpc_status_http_class(N00B_RPC_CANCELLED)           == 499);
    assert(n00b_rpc_status_http_class(N00B_RPC_UNIMPLEMENTED)       == 501);
    assert(n00b_rpc_status_http_class(N00B_RPC_UNAVAILABLE)         == 503);
    assert(n00b_rpc_status_http_class(N00B_RPC_DEADLINE_EXCEEDED)   == 504);
    assert(n00b_rpc_status_http_class(N00B_RPC_INTERNAL)            == 500);
    assert(n00b_rpc_status_http_class(N00B_RPC_DATA_LOSS)           == 500);
    /* Out-of-range → 500. */
    assert(n00b_rpc_status_http_class((n00b_rpc_status_t)9999)      == 500);
    printf("  [PASS] http_class mapping (incl. out-of-range fallback)\n");
}

static void
test_from_quic_err(void)
{
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_OK) == N00B_RPC_OK);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_TIMEOUT)
           == N00B_RPC_DEADLINE_EXCEEDED);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_FLOW_BLOCKED)
           == N00B_RPC_RESOURCE_EXHAUSTED);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_AUTH_TOKEN_INVALID)
           == N00B_RPC_UNAUTHENTICATED);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_AUTH_MTLS_MISMATCH)
           == N00B_RPC_PERMISSION_DENIED);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_NOT_IMPLEMENTED)
           == N00B_RPC_UNIMPLEMENTED);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_PROTOCOL)
           == N00B_RPC_INVALID_ARGUMENT);
    assert(n00b_rpc_status_from_quic_err(N00B_QUIC_ERR_PEER_RESET)
           == N00B_RPC_CANCELLED);
    /* Unknown err → INTERNAL. */
    assert(n00b_rpc_status_from_quic_err(-9999) == N00B_RPC_INTERNAL);
    printf("  [PASS] quic_err → rpc_status mapping\n");
}

static void
test_grpc_numeric_alignment(void)
{
    /* Spot-check that we match gRPC's numeric values for ease of
     * bridging.  Reference: https://grpc.io/docs/guides/status-codes/ */
    assert((int)N00B_RPC_OK                  ==  0);
    assert((int)N00B_RPC_CANCELLED           ==  1);
    assert((int)N00B_RPC_INVALID_ARGUMENT    ==  3);
    assert((int)N00B_RPC_DEADLINE_EXCEEDED   ==  4);
    assert((int)N00B_RPC_PERMISSION_DENIED   ==  7);
    assert((int)N00B_RPC_UNAUTHENTICATED     == 16);
    printf("  [PASS] numeric values match gRPC's status codes\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_quic_rpc_status:\n");
    test_status_str();
    test_http_class();
    test_from_quic_err();
    test_grpc_numeric_alignment();
    printf("All quic_rpc_status tests passed.\n");

    n00b_shutdown();
    return 0;
}
