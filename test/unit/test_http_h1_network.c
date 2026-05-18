/*
 * test_http_h1_network.c — Phase 6 chunk 2.3: end-to-end smoke for
 * the new h1 round-trip path against real public HTTPS endpoints.
 *
 * Opt-in: skipped unless N00B_TEST_NET=1 is set.  Same gating shape
 * as test_quic_acme_http_network.c so CI stays green on offline
 * runners.  Tries three independent endpoints (cloudflare.com,
 * www.google.com, www.example.com) so a single edge outage doesn't
 * fail the suite — asserts at least 2/3 succeed with status < 500
 * per ~/dd/quic_6.md § 8.2.
 *
 * The TLS layer is the existing acme_tls.c shim (TCP + picotls +
 * OS-trust verification).  This test exists to prove that the
 * generalized request builder + response parser + Connection
 * keep-alive parsing in chunk 2.1/2.2 round-trip cleanly against
 * real servers.  In-process loopback (§ 8.1) lands later when we
 * stand up the TLS server fixture.
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
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h1.h"
#include "internal/net/http/http_pool.h"

static bool
try_get(const char *url_cstr, int *status_out)
{
    auto ur = n00b_http_url_parse(n00b_string_from_cstr(url_cstr));
    if (n00b_result_is_err(ur)) {
        printf("  [WARN] %s: URL parse failed\n", url_cstr);
        return false;
    }
    n00b_http_url_t *url = n00b_result_get(ur);

    auto rr = n00b_http_h1_round_trip(url, .timeout_ms = 10000);
    if (n00b_result_is_err(rr)) {
        printf("  [WARN] %s: round-trip failed err=%d\n",
               url_cstr, (int)n00b_result_get_err(rr));
        return false;
    }
    n00b_http_h1_response_t *resp = n00b_result_get(rr);
    *status_out                   = resp->status;
    printf("  [INFO] %s: status=%d body=%zu bytes (h1.minor=%u, keep=%d)\n",
           url_cstr, resp->status, (size_t)resp->body->byte_len,
           (unsigned)resp->http_minor, (int)resp->keep_alive);
    return resp->status >= 100 && resp->status < 500;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    if (!getenv("N00B_TEST_NET")) {
        printf("Skipping http_h1_network test "
               "(set N00B_TEST_NET=1 to enable).\n");
        n00b_shutdown();
        return 0;
    }

    static const char *targets[] = {
        "https://www.cloudflare.com/",
        "https://www.google.com/",
        "https://www.example.com/",
    };
    int succeeded = 0;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        int status = -1;
        if (try_get(targets[i], &status)) {
            succeeded++;
        }
    }
    /* Tolerant gate: 2-of-3.  A single edge or DNS hiccup shouldn't
     * fail the smoke. */
    if (succeeded < 2) {
        fprintf(stderr,
                "  [FAIL] only %d/3 endpoints succeeded\n", succeeded);
        abort();
    }
    printf("  [PASS] %d/3 public endpoints round-tripped through h1\n",
           succeeded);

    /* Pool-reuse smoke: hammer one origin three times with a shared
     * pool and confirm at least one acquire_hit (i.e. the second/third
     * request reused the first request's TCP+TLS pair). */
    n00b_http_connection_pool_t *pool = n00b_http_connection_pool_new();
    auto                         ur   = n00b_http_url_parse(
        n00b_string_from_cstr("https://www.cloudflare.com/"));
    if (n00b_result_is_ok(ur)) {
        n00b_http_url_t *url = n00b_result_get(ur);
        int              ok  = 0;
        for (int i = 0; i < 3; i++) {
            auto rr = n00b_http_h1_round_trip(url,
                                              .timeout_ms = 10000,
                                              .pool       = pool);
            if (n00b_result_is_ok(rr)) ok++;
        }
        n00b_http_connection_pool_stats_t st =
            n00b_http_connection_pool_stats(pool);
        printf("  [INFO] pool: hits=%zu misses=%zu idle=%zu (ok=%d/3)\n",
               st.acquire_hits, st.acquire_misses, st.idle_count, ok);
        if (ok >= 2 && st.acquire_hits == 0) {
            fprintf(stderr,
                    "  [FAIL] pool reused 0 connections out of %d ok\n", ok);
            abort();
        }
    }
    n00b_http_connection_pool_close(pool);

    n00b_shutdown();
    return 0;
}
