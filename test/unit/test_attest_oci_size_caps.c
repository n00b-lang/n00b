/** @file test/unit/test_attest_oci_size_caps.c — per-call body-
 *  size cap regression (WP-004 Phase 4).
 *
 *  WP-004 Phase 4 hardening (NFR-5) enforces a 1 MiB per-call body-
 *  size cap on:
 *    [a] referrer-listing pagination (`_list_referrers`).
 *    [b] blob / manifest fetch (`_blob_fetch` / `_manifest_fetch`)
 *        via the existing `max_size` kwarg inherited from Phase 3.
 *
 *  libn00b's `n00b_http_request_sync` does NOT (yet) carry a
 *  per-call `max_body_size` kwarg, so the cap is enforced n00b-
 *  attest-side at the post-parse seam. Lifting the enforcement
 *  into libn00b's dispatcher (so an oversized body never
 *  materializes past the cap on the wire) is tracked as a
 *  candidate DF flagged at Phase 4 closeout.
 *
 *  This unit test verifies the substrate's surface without a real
 *  network round-trip:
 *
 *    [1] The error-code namespace carries `_OCI_RESPONSE_TOO_LARGE`
 *        (-6010) with a non-empty human-readable mapping from
 *        `n00b_attest_err_str`.
 *    [2] The error-code namespace carries `_OCI_BLOB_TOO_LARGE`
 *        (-6007) with a non-empty mapping (Phase 3 + Phase 4 share
 *        this code for the blob-size cap surface).
 *    [3] The two codes are distinct (the response-body cap and the
 *        blob-cap surfaces are deliberately separate per D-046's
 *        phase-introduces-codes-when-it-uses-them rule + the
 *        intent that callers can distinguish "registry response was
 *        oversized" from "fetched blob was oversized").
 *    [4] Bad-input legs on the size-capping fetchers (`_blob_fetch` /
 *        `_manifest_fetch`) correctly surface `_OCI_BAD_URL` for
 *        null inputs — proves the kwarg-block-with-`max_size`-and-
 *        `timeout_ms` declaration parses correctly and the impl
 *        routes through the standard guard rail.
 *
 *  The end-to-end behavioral surface (an oversized real registry
 *  response triggers the code in production) is exercised by the
 *  Docker-gated `attest_oci_discover_smoke` / `attest_oci_pull_smoke`
 *  precedent suite when the underlying registry returns a real body
 *  past the cap; that path is real-traffic-only and not appropriate
 *  for an always-runs regression. The substrate-level surface tested
 *  here ensures the codes + symbols are wired correctly so the
 *  behavioral path can compile + emit the right code.
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

// ---------------------------------------------------------------------------
// Sub-case [1] + [2]: every size-cap error code maps to a non-empty
// human-readable string.
// ---------------------------------------------------------------------------

static void
test_size_cap_codes_have_strings(void)
{
    n00b_string_t *s_resp = n00b_attest_err_str(
        N00B_ATTEST_ERR_OCI_RESPONSE_TOO_LARGE);
    assert(s_resp != nullptr);
    assert(s_resp->u8_bytes > 0);

    n00b_string_t *s_blob = n00b_attest_err_str(
        N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE);
    assert(s_blob != nullptr);
    assert(s_blob->u8_bytes > 0);

    printf("  [PASS] size_cap_codes_have_strings\n");
}

// ---------------------------------------------------------------------------
// Sub-case [3]: the two codes are distinct (deliberate per D-046).
// ---------------------------------------------------------------------------

static void
test_codes_are_distinct(void)
{
    assert(N00B_ATTEST_ERR_OCI_RESPONSE_TOO_LARGE
           != N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE);
    // Pin the slot values: a future code-namespace shuffle that
    // accidentally collides these triggers a build-time visible
    // assert.
    assert(N00B_ATTEST_ERR_OCI_RESPONSE_TOO_LARGE == -6010);
    assert(N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE     == -6007);
    printf("  [PASS] codes_are_distinct\n");
}

// ---------------------------------------------------------------------------
// Sub-case [4]: bad-input legs on the fetchers route through
// _OCI_BAD_URL — confirms the kwarg-block parses correctly + the
// guard rail is wired before the cap check.
// ---------------------------------------------------------------------------

static void
test_fetcher_bad_input_legs(void)
{
    // Build a valid client handle so the bad-input legs we exercise
    // are the fetcher's own null-arg checks rather than upstream
    // client validation.
    n00b_string_t *url = n00b_string_from_cstr("https://localhost:5000");
    auto           cr  = n00b_attest_oci_client_new(url);
    if (n00b_result_is_err(cr)) {
        fprintf(stderr, "FAIL: client_new returned err\n");
        assert(0);
    }
    n00b_attest_oci_client_t *client = n00b_result_get(cr);

    // Null name on blob_fetch -> BAD_URL.
    auto br1 = n00b_attest_oci_blob_fetch(client,
                                          nullptr,
                                          n00b_string_from_cstr(
                                              "sha256:deadbeef"));
    assert(n00b_result_is_err(br1));
    assert(n00b_result_get_err(br1) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Null digest on blob_fetch -> BAD_URL.
    auto br2 = n00b_attest_oci_blob_fetch(client,
                                          n00b_string_from_cstr("foo/bar"),
                                          nullptr);
    assert(n00b_result_is_err(br2));
    assert(n00b_result_get_err(br2) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Null name on manifest_fetch -> BAD_URL.
    auto mr1 = n00b_attest_oci_manifest_fetch(client,
                                              nullptr,
                                              n00b_string_from_cstr(
                                                  "sha256:deadbeef"));
    assert(n00b_result_is_err(mr1));
    assert(n00b_result_get_err(mr1) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Null digest on manifest_fetch -> BAD_URL.
    auto mr2 = n00b_attest_oci_manifest_fetch(client,
                                              n00b_string_from_cstr("foo/bar"),
                                              nullptr);
    assert(n00b_result_is_err(mr2));
    assert(n00b_result_get_err(mr2) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Confirm the kwarg-block accepts both max_size + timeout_ms
    // simultaneously — Phase 4 added the second kwarg; this asserts
    // the impl compiles + runs through both. Pass a synthetic large
    // max_size to confirm the kwarg block accepts non-zero values.
    // The call still surfaces an HTTP error because there is no live
    // registry at localhost:5000 in this test; we don't assert the
    // exact error code (it depends on libn00b's dispatcher), just
    // that the call returns (i.e. the kwarg block parsed).
    auto br3 = n00b_attest_oci_blob_fetch(
        client,
        n00b_string_from_cstr("foo/bar"),
        n00b_string_from_cstr("sha256:deadbeef"),
        .max_size   = 1024,
        .timeout_ms = 100);  // 100ms — should err out quickly
    // The result is Err either way (network unreachable / bad digest);
    // we just confirm the call shape compiled + executed.
    (void)br3;

    n00b_attest_oci_client_release(client);
    printf("  [PASS] fetcher_bad_input_legs\n");
}

// ---------------------------------------------------------------------------
// Sub-case [5]: list_referrers also carries the `timeout_ms` kwarg
// added in Phase 4 — confirms the kwarg-block parses + the null-
// input guard rail is wired.
// ---------------------------------------------------------------------------

static void
test_list_referrers_bad_input(void)
{
    n00b_string_t *url = n00b_string_from_cstr("https://localhost:5000");
    auto           cr  = n00b_attest_oci_client_new(url);
    if (n00b_result_is_err(cr)) {
        fprintf(stderr, "FAIL: client_new returned err\n");
        assert(0);
    }
    n00b_attest_oci_client_t *client = n00b_result_get(cr);

    // Null name -> BAD_URL.
    auto r1 = n00b_attest_oci_list_referrers(client,
                                             nullptr,
                                             n00b_string_from_cstr(
                                                 "sha256:deadbeef"));
    assert(n00b_result_is_err(r1));
    assert(n00b_result_get_err(r1) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Null digest -> BAD_URL.
    auto r2 = n00b_attest_oci_list_referrers(client,
                                             n00b_string_from_cstr("foo/bar"),
                                             nullptr);
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == N00B_ATTEST_ERR_OCI_BAD_URL);

    // Confirm the kwarg-block accepts timeout_ms. As above, the call
    // surfaces a network error (no live registry); we just confirm
    // the call shape compiled.
    auto r3 = n00b_attest_oci_list_referrers(
        client,
        n00b_string_from_cstr("foo/bar"),
        n00b_string_from_cstr("sha256:deadbeef"),
        .timeout_ms = 100);
    (void)r3;

    n00b_attest_oci_client_release(client);
    printf("  [PASS] list_referrers_bad_input\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest OCI size-caps ==\n");
    test_size_cap_codes_have_strings();
    test_codes_are_distinct();
    test_fetcher_bad_input_legs();
    test_list_referrers_bad_input();

    printf("All n00b_attest OCI size-caps tests passed.\n");
    return 0;
}
