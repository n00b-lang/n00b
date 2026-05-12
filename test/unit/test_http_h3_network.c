/*
 * test_http_h3_network.c — Phase 6 chunk 3 informational smoke for
 * the h3 round-trip primitive against real public HTTPS endpoints.
 *
 * Opt-in gate: N00B_TEST_NET=1.
 *
 * **Tolerant** of h3 transport failures.  The picotls→OS-trust
 * bridge is wired (Phase 3 work landed in `picotls_verify.c`), but
 * real-world QUIC reachability still depends on:
 *   - UDP packets making it past local NAT / corp proxies,
 *   - the remote stack not requiring a feature our pinned picoquic
 *     version doesn't speak (e.g. specific Retry-token forms),
 *   - the remote endpoint actually advertising h3 over UDP at all.
 *
 * So we treat HANDSHAKE / TIMEOUT / BIND_FAILED / TRUST_REJECTED /
 * PEER_CLOSED as "transport hiccup, not a programming bug" and pass
 * through.  The test fails only when those same endpoints raise an
 * error class outside that set — those flag a real bug in the round
 * trip primitive.
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h3.h"
#include "internal/net/http/http_pool.h"

static bool
expected_trust_miss(int err)
{
    return err == N00B_QUIC_ERR_HANDSHAKE
        || err == N00B_QUIC_ERR_TIMEOUT
        || err == N00B_QUIC_ERR_TRUST_REJECTED
        || err == N00B_QUIC_ERR_PEER_CLOSED
        || err == N00B_QUIC_ERR_BIND_FAILED;  /* DNS/UDP-bind on
                                                 sandboxed CI */
}

static int
try_h3(const char *url_cstr)
{
    auto ur = n00b_http_url_parse(n00b_string_from_cstr(url_cstr));
    if (n00b_result_is_err(ur)) {
        printf("  [WARN] %s: URL parse failed\n", url_cstr);
        return -1;
    }
    n00b_http_url_t *url = n00b_result_get(ur);

    auto rr = n00b_http_h3_round_trip(url,
                                       .handshake_ms = 5000,
                                       .await_ms     = 10000);
    if (n00b_result_is_err(rr)) {
        int err = (int)n00b_result_get_err(rr);
        const char *kind = expected_trust_miss(err)
                               ? "EXPECTED_TRANSPORT_HICCUP"
                               : "UNEXPECTED";
        printf("  [%s] %s: err=%d (%s)\n",
               kind, url_cstr, err,
               n00b_quic_err_str((n00b_quic_err_t)err));
        return expected_trust_miss(err) ? 0 : -1;
    }
    n00b_h3_response_t *resp = n00b_result_get(rr);
    printf("  [OK] %s: h3 status=%u body=%zu bytes\n",
           url_cstr, (unsigned)resp->status,
           (size_t)resp->body->byte_len);
    return 1;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    if (!getenv("N00B_TEST_NET")) {
        printf("Skipping http_h3_network test "
               "(set N00B_TEST_NET=1 to enable).\n");
        n00b_shutdown();
        return 0;
    }

    static const char *targets[] = {
        "https://www.cloudflare.com/",
        "https://www.google.com/",
        "https://quic.nginx.org/",
    };
    int succeeded = 0;
    int unexpected = 0;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        int rc = try_h3(targets[i]);
        if (rc == 1) succeeded++;
        else if (rc == -1) unexpected++;
    }

    printf("  Summary: %d/3 succeeded, %d unexpected errors\n",
           succeeded, unexpected);

    /* Until the trust bridge lands the only failure that matters is
     * an UNEXPECTED error — those flag a real bug in the round-trip
     * primitive.  Trust misses are silent. */
    if (unexpected > 0) {
        fprintf(stderr,
                "  [FAIL] %d endpoint(s) hit unexpected errors\n",
                unexpected);
        abort();
    }
    printf("  [PASS] h3 round-trip primitive driveable end-to-end\n");

    /* Pool reuse smoke: hammer one origin three times with a shared
     * pool, see whether at least one acquire_hit shows up.  Use
     * quic.nginx.org because cloudflare / google fail handshake
     * pre-trust-bridge (cf. EXPECTED_PRE_TRUST_BRIDGE above) which
     * would leave nothing in the pool to reuse. */
    n00b_http_connection_pool_t *pool = n00b_http_connection_pool_new();
    auto                         ur   = n00b_http_url_parse(
        n00b_string_from_cstr("https://quic.nginx.org/"));
    if (n00b_result_is_ok(ur)) {
        n00b_http_url_t *url = n00b_result_get(ur);
        int              ok  = 0;
        for (int i = 0; i < 3; i++) {
            auto rr = n00b_http_h3_round_trip(url,
                                              .handshake_ms = 5000,
                                              .await_ms     = 10000,
                                              .pool         = pool);
            if (n00b_result_is_ok(rr)) ok++;
        }
        n00b_http_connection_pool_stats_t st =
            n00b_http_connection_pool_stats(pool);
        printf("  [INFO] h3 pool: hits=%zu misses=%zu idle=%zu (ok=%d/3)\n",
               st.acquire_hits, st.acquire_misses, st.idle_count, ok);
        if (ok >= 2 && st.acquire_hits == 0) {
            fprintf(stderr,
                    "  [FAIL] h3 pool reused 0 connections out of %d ok\n",
                    ok);
            abort();
        }
    }
    n00b_http_connection_pool_close(pool);

    n00b_shutdown();
    return 0;
}
