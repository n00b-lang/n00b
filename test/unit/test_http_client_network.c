/*
 * test_http_client_network.c — Phase 6 chunk 5 live-network smoke
 * for the public dispatcher.  Exercises both h3-first (with h1
 * fallback) and h1-only paths against three public endpoints.
 *
 * Gated by N00B_TEST_NET=1.  Tolerant to h3 failure pre-trust-bridge:
 * if the dispatcher falls back to h1 and h1 returns 200, that's a
 * pass — the loss cache is exactly the expected behavior on the
 * h3-blocked origins surfaced by chunk 3's smoke.
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
#include "conduit/conduit.h"
#include "net/quic/quic_types.h"
#include "net/http/http_client.h"

static const char *
transport_name(n00b_http_transport_t t)
{
    switch (t) {
    case N00B_HTTP_TRANSPORT_H1: return "h1";
    case N00B_HTTP_TRANSPORT_H3: return "h3";
    default:                    return "?";
    }
}

static int
try_request(const char *url, bool prefer_h3)
{
    auto r = n00b_http_request_sync(
        n00b_string_from_cstr(url),
        .prefer_h3       = prefer_h3,
        .h3_handshake_ms = 2000,
        .timeout_ms      = 10000);
    if (n00b_result_is_err(r)) {
        printf("  [WARN] %s (prefer_h3=%d): err=%d (%s)\n",
               url, (int)prefer_h3,
               (int)n00b_result_get_err(r),
               n00b_quic_err_str(
                   (n00b_quic_err_t)n00b_result_get_err(r)));
        return -1;
    }
    n00b_http_response_t *resp = n00b_result_get(r);
    int                  status = n00b_http_response_status(resp);
    n00b_buffer_t       *body   = n00b_http_response_body(resp);
    printf("  [OK] %s prefer_h3=%d → %s status=%d body=%zu\n",
           url, (int)prefer_h3,
           transport_name(n00b_http_response_transport(resp)),
           status, (size_t)body->byte_len);
    return (status >= 100 && status < 500) ? 1 : 0;
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    if (!getenv("N00B_TEST_NET")) {
        printf("Skipping http_client_network test "
               "(set N00B_TEST_NET=1 to enable).\n");
        n00b_shutdown();
        return 0;
    }

    static const char *targets[] = {
        "https://www.cloudflare.com/",
        "https://www.google.com/",
        "https://www.example.com/",
    };

    /* h3-first: should work for h3-capable origins, fall back to h1
     * otherwise.  Either is a pass. */
    n00b_http_loss_cache_reset();
    int ok_h3_first = 0;
    for (size_t i = 0; i < 3; i++) {
        if (try_request(targets[i], true) == 1) ok_h3_first++;
    }

    /* h1-only: pure h1 path, must succeed for at least 2/3. */
    n00b_http_loss_cache_reset();
    int ok_h1_only = 0;
    for (size_t i = 0; i < 3; i++) {
        if (try_request(targets[i], false) == 1) ok_h1_only++;
    }

    printf("  Summary: h3-first=%d/3, h1-only=%d/3\n",
           ok_h3_first, ok_h1_only);
    if (ok_h3_first < 2) {
        fprintf(stderr,
                "  [FAIL] h3-first dispatcher: only %d/3 succeeded "
                "(should fall back to h1 cleanly)\n", ok_h3_first);
        abort();
    }
    if (ok_h1_only < 2) {
        fprintf(stderr,
                "  [FAIL] h1-only dispatcher: only %d/3 succeeded\n",
                ok_h1_only);
        abort();
    }
    printf("  [PASS] sync dispatcher race+fallback drives end-to-end\n");

    /* Topic-shaped path: same end-to-end flow, but the response
     * arrives via conduit_read on a typed topic. */
    auto cr = n00b_conduit_new();
    n00b_conduit_t *c = n00b_result_get(cr);

    n00b_http_loss_cache_reset();
    int ok_topic = 0;
    for (size_t i = 0; i < 3; i++) {
        auto tr = n00b_http_request(c, n00b_string_from_cstr(targets[i]),
                                     .timeout_ms = 10000);
        if (n00b_result_is_err(tr)) {
            printf("  [WARN] topic %s: dispatch err=%d\n",
                   targets[i], (int)n00b_result_get_err(tr));
            continue;
        }
        n00b_conduit_topic_t(n00b_http_response_t *) *t = n00b_result_get(tr);

        auto rr = n00b_conduit_read(n00b_http_response_t *, t,
                                    .timeout_ms = 15000);
        if (n00b_result_is_err(rr)) {
            printf("  [WARN] topic %s: read err=%d\n",
                   targets[i], (int)n00b_result_get_err(rr));
            continue;
        }
        n00b_conduit_message_t(n00b_http_response_t *) *m = n00b_result_get(rr);
        n00b_http_response_t *resp = m->payload;
        if (n00b_http_response_error(resp) != 0) {
            printf("  [WARN] topic %s: transport err=%d\n",
                   targets[i], (int)n00b_http_response_error(resp));
            continue;
        }
        printf("  [TOPIC OK] %s: status=%d body=%zu\n",
               targets[i], n00b_http_response_status(resp),
               (size_t)n00b_http_response_body(resp)->byte_len);
        ok_topic++;
    }
    n00b_conduit_destroy(c);
    if (ok_topic < 2) {
        fprintf(stderr,
                "  [FAIL] topic dispatcher: only %d/3 succeeded\n",
                ok_topic);
        abort();
    }
    printf("  [PASS] topic-shaped dispatcher delivers via conduit_read\n");

    n00b_shutdown();
    return 0;
}
