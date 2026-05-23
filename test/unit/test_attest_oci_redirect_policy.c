/** @file test/unit/test_attest_oci_redirect_policy.c — redirect-
 *  policy delegation regression (WP-004 Phase 4).
 *
 *  WP-004 Phase 4 hardening (per the plan + the D-051 OQ-7
 *  disposition + the 2026-05-19 user direction) delegates redirect
 *  policy enforcement to libn00b's `n00b_http_request_sync` machinery
 *  rather than rolling a redirect-follower inside the OCI client.
 *  The OCI client's contribution is:
 *
 *    [1] Storing the per-call `redirect_host_allowlist` kwarg on the
 *        client handle for forward-compat with a libn00b lift (the
 *        kwarg is preserved on the handle; libn00b's dispatcher does
 *        NOT yet carry a matching per-call kwarg, so the allowlist is
 *        storage-only at WP-004 Phase 4 — surfaced as a libn00b gap
 *        / candidate DF at Phase 4 closeout).
 *
 *    [2] Toggling `follow_redirects = true/false` on libn00b's
 *        dispatcher based on the client's `allow_redirects` field,
 *        with `max_redirects = 1` (single-hop cap — registries that
 *        need more are misconfigured + a stricter cap reduces the
 *        attack surface of redirect chains).
 *
 *    [3] Relying on libn00b's "Cross-scheme redirects to anything
 *        other than `https://` are rejected" guarantee documented on
 *        `n00b_http_request_sync` for HTTPS-only-on-3xx enforcement.
 *
 *  This test verifies the storage-side contributions ([1] + [2])
 *  without an actual HTTP round-trip — the goal is to assert that the
 *  OCI client's policy threading shape is correct on the construction
 *  side. The actual redirect-following behavior is libn00b's
 *  responsibility (with its own regression coverage in
 *  `test/unit/test_h1_pinned_trust.c` + the libn00b core-WP suite).
 *
 *  Sub-cases:
 *    - `redirect_host_allowlist` defaults to nullptr.
 *    - `redirect_host_allowlist` with a non-empty list is stored
 *      verbatim on the handle.
 *    - `allow_redirects` defaults to true.
 *    - `allow_redirects = false` is stored verbatim.
 *    - Constructing a client with a tag-allowlist + redirects-disabled
 *      combination preserves both settings independently.
 *
 *  Test-file carve-out (D-030) applies — libc I/O for stdout logging
 *  is acceptable per the established test-file precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "attest/n00b_attest.h"
#include "internal/attest/oci/registry.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                (int)n00b_result_get_err(r)); \
        assert(0); } } while (0)

// ---------------------------------------------------------------------------
// Sub-case: defaults — allow_redirects = true; allowlist = nullptr.
// ---------------------------------------------------------------------------

static void
test_defaults(void)
{
    n00b_string_t *url = n00b_string_from_cstr("https://localhost:5000");
    auto           r   = n00b_attest_oci_client_new(url);
    ASSERT_OK(r);
    n00b_attest_oci_client_t *c = n00b_result_get(r);

    assert(c->allow_redirects == true);
    assert(c->redirect_host_allowlist == nullptr);

    n00b_attest_oci_client_release(c);
    printf("  [PASS] defaults\n");
}

// ---------------------------------------------------------------------------
// Sub-case: storage of an explicit host-allowlist.
// ---------------------------------------------------------------------------

static void
test_allowlist_stored_verbatim(void)
{
    n00b_string_t *url = n00b_string_from_cstr("https://ghcr.io");
    n00b_list_t(n00b_string_t *) allowlist =
        n00b_list_new(n00b_string_t *);
    n00b_list_push(allowlist, n00b_string_from_cstr("ghcr.io"));
    n00b_list_push(allowlist, n00b_string_from_cstr("mirror.ghcr.io"));

    auto r = n00b_attest_oci_client_new(
        url,
        .redirect_host_allowlist = &allowlist);
    ASSERT_OK(r);
    n00b_attest_oci_client_t *c = n00b_result_get(r);

    // The kwarg is stored verbatim on the handle (D-051 OQ-7 per-call
    // kwarg path). Storage-only at WP-004 Phase 4 — libn00b's
    // dispatcher does not yet enforce it; flagged as a candidate DF.
    assert(c->redirect_host_allowlist == &allowlist);
    assert(c->allow_redirects == true);  // default carry-through

    n00b_attest_oci_client_release(c);
    printf("  [PASS] allowlist_stored_verbatim\n");
}

// ---------------------------------------------------------------------------
// Sub-case: allow_redirects = false toggles cleanly.
// ---------------------------------------------------------------------------

static void
test_redirects_disabled(void)
{
    n00b_string_t *url = n00b_string_from_cstr("https://ghcr.io");
    auto           r   = n00b_attest_oci_client_new(url,
                                          .allow_redirects = false);
    ASSERT_OK(r);
    n00b_attest_oci_client_t *c = n00b_result_get(r);

    assert(c->allow_redirects == false);
    assert(c->redirect_host_allowlist == nullptr);

    n00b_attest_oci_client_release(c);
    printf("  [PASS] redirects_disabled\n");
}

// ---------------------------------------------------------------------------
// Sub-case: both knobs threaded together — independent storage.
// ---------------------------------------------------------------------------

static void
test_both_knobs_combined(void)
{
    n00b_string_t *url = n00b_string_from_cstr("https://ghcr.io");
    n00b_list_t(n00b_string_t *) allowlist =
        n00b_list_new(n00b_string_t *);
    n00b_list_push(allowlist, n00b_string_from_cstr("ghcr.io"));

    auto r = n00b_attest_oci_client_new(
        url,
        .allow_redirects         = false,
        .redirect_host_allowlist = &allowlist);
    ASSERT_OK(r);
    n00b_attest_oci_client_t *c = n00b_result_get(r);

    assert(c->allow_redirects == false);
    assert(c->redirect_host_allowlist == &allowlist);

    n00b_attest_oci_client_release(c);
    printf("  [PASS] both_knobs_combined\n");
}

// ---------------------------------------------------------------------------
// Sub-case: HTTPS-only construction enforced at _client_new time
// (libn00b layer enforces HTTPS-only on redirect follow — this
// substrate test confirms the OCI client also rejects bare HTTP
// at construction so a later redirect downgrade can't even start
// from an HTTP base URL).
// ---------------------------------------------------------------------------

static void
test_construction_https_only(void)
{
    n00b_string_t *http_url = n00b_string_from_cstr("http://ghcr.io");
    auto           r        = n00b_attest_oci_client_new(http_url);
    assert(n00b_result_is_err(r));
    assert(n00b_result_get_err(r) == N00B_ATTEST_ERR_OCI_BAD_URL);
    printf("  [PASS] construction_https_only\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest OCI redirect-policy ==\n");
    test_defaults();
    test_allowlist_stored_verbatim();
    test_redirects_disabled();
    test_both_knobs_combined();
    test_construction_https_only();

    printf("All n00b_attest OCI redirect-policy tests passed.\n");
    return 0;
}
