/*
 * test_http_h3.c — Phase 6 chunk 3 unit tests for the h3 round-trip
 * primitive.
 *
 * Argument validation only at this revision.  The end-to-end loopback
 * test reuses the Caddy fixture's bones and lives at
 * `test_quic_h3_caddy_smoke.c`; the Phase-6 generalization there
 * lands when chunk 5's dispatcher cuts in (so a single test exercises
 * both the new public surface and the underlying h3 layer through
 * one driver).
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "n00b.h"
#include "core/runtime.h"
#include "adt/result.h"
#include "net/quic/quic_types.h"
#include "internal/net/http/http_url.h"
#include "internal/net/http/http_h3.h"

static void
test_null_url(void)
{
    auto r = n00b_http_h3_round_trip(nullptr);
    assert(n00b_result_is_err(r));
    assert((int)n00b_result_get_err(r) == N00B_QUIC_ERR_NULL_ARG);
    printf("  [PASS] nullptr URL → NULL_ARG\n");
}

static void
test_unresolvable_host(void)
{
    /* Hostname that is guaranteed not to resolve; verifies the DNS
     * failure path returns an error rather than crashing. */
    auto ur = n00b_http_url_parse(n00b_string_from_cstr(
        "https://this-host-must-not-resolve.invalid./"));
    assert(n00b_result_is_ok(ur));
    n00b_http_url_t *url = n00b_result_get(ur);

    auto r = n00b_http_h3_round_trip(url, .handshake_ms = 200);
    assert(n00b_result_is_err(r));
    /* DNS failure maps to BIND_FAILED in the wrapper. */
    assert((int)n00b_result_get_err(r) == N00B_QUIC_ERR_BIND_FAILED);
    printf("  [PASS] DNS failure → BIND_FAILED\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_h3:\n");
    test_null_url();
    test_unresolvable_host();
    printf("All test_http_h3 tests passed.\n");

    n00b_shutdown();
    return 0;
}
