/* src/attest/n00b_attest_module.c — module-init entry point.
 *
 * `n00b_attest_module_init` is the single authoritative entry
 * point that wires up every in-tree backend's vtable + resolver
 * registration. The host caller (test binary, CLI entry, etc.)
 * is responsible for calling this function exactly once during
 * process startup before any signer-resolve happens; this is
 * the standard libn00b module-init pattern and does NOT use
 * `[[gnu::constructor]]` (which the api-guidelines §4.3
 * explicitly bans for caching caller state).
 *
 * Phase 1's stub body has graduated to a real init body: it
 * populates the file backend's vtable (per architecture §6.1,
 * `n00b_string_t *scheme;` is on the vtable, so the vtable
 * cannot be a file-scope `static const` constant — it must be
 * filled at runtime) and registers the backend with the
 * resolver.
 *
 * Calling this function more than once is idempotent in
 * practice for WP-002's single in-tree backend (the file
 * backend will be re-registered with itself, causing the
 * registration list to grow by one but not changing
 * resolve-time behavior since the linear search hits the
 * first match). A future hardening pass can add an
 * already-initialized guard if multi-call becomes a real
 * concern; WP-002 leaves the single-call convention
 * unenforced because the host caller owns the lifecycle.
 *
 * No globals beyond the registration list, no allocations
 * tied to a caller arena (the scheme strings live for the
 * process lifetime; no caller state is captured). Per
 * api-guidelines §10.9 this is the correct shape for
 * module-init.
 */

#include <attest/n00b_attest.h>

#include "internal/attest/backends.h"
#include "internal/attest/backends/file.h"
#include "internal/attest/verifier_backends.h"
#include "internal/attest/verifier_backends/file.h"
#include "util/panic.h"

void
n00b_attest_module_init(void)
{
    // Populate the file signer-backend vtable's fields. The
    // vtable instance is declared in `backends/file.c`; its
    // fields are mutable until init returns, then read-only for
    // the process lifetime.
    _n00b_attest_backend_file_init();

    // Wire the populated signer vtable into the resolver.
    // Registration failure is unrecoverable per `n00b-code-
    // auditor` W-4 + user disposition: there is no sensible
    // recovery (every subsequent signer-resolve would fail with
    // UNSUPPORTED_SCHEME), so we surface the failure immediately
    // with a diagnostic.
    n00b_result_t(bool) r =
        n00b_attest_register_backend(&n00b_attest_backend_file);
    if (n00b_result_is_err(r)) {
        n00b_panic(
            "n00b_attest_module_init: failed to register file backend");
    }

    // WP-003 Phase 2: populate and register the file verifier-
    // backend vtable. Same fast-fail-via-`n00b_panic` shape per
    // D-042 W-4 — a verifier-backend registration failure means
    // every subsequent verifier-resolve would fail with
    // _VERIFIER_UNSUPPORTED_SCHEME.
    _n00b_attest_verifier_backend_file_init();

    n00b_result_t(bool) v =
        n00b_attest_register_verifier_backend(
            &n00b_attest_verifier_backend_file);
    if (n00b_result_is_err(v)) {
        n00b_panic(
            "n00b_attest_module_init: failed to register file "
            "verifier backend");
    }
}
