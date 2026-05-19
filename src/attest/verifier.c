/* src/attest/verifier.c — verifier resolver edge.
 *
 * Implements the surface declared in
 * include/attest/n00b_attest_verifier.h:
 *   - n00b_attest_verifier_resolve            (URI dispatch by scheme)
 *   - n00b_attest_verifier_check              (dispatch through
 *                                              backend.check)
 *   - n00b_attest_verifier_pubkey_spki_der    (dispatch through
 *                                              backend.pubkey)
 *   - n00b_attest_verifier_keyid              (dispatch through
 *                                              backend.keyid)
 *   - n00b_attest_verifier_release            (dispatch through
 *                                              backend.release)
 *
 * Plus the package-private:
 *   - n00b_attest_register_verifier_backend   (architecture §6.1;
 *                                              resolver consults the
 *                                              registration list at
 *                                              every resolve)
 *
 * Structural mirror of `src/attest/signer.c`. The shape rationale
 * (fixed-capacity registration list, init-once write-once read-many
 * model, kwargs-to-opts flattening) is identical to the signer side;
 * the only structural difference is the vtable function-pointer set
 * (`check` vs `sign`).
 *
 * Backend registration uses a small file-scope registration list
 * populated at module-init time by `n00b_attest_module_init`
 * (see `src/attest/n00b_attest_module.c`). The list shape is a
 * fixed-capacity static array — not a `n00b_list_t` — because:
 *
 *   - No allocator is in play at registration time (registration
 *     uses backend-owned static-storage values; no caller state
 *     is captured).
 *   - The list is module-init-write-once, then read-only for the
 *     process lifetime; no lock is needed for the steady-state
 *     resolve path.
 *   - The fixed capacity (16) is well above WP-003's single
 *     entry and far above the foreseeable in-tree count; a
 *     future WP that ships plugin support and needs unbounded
 *     capacity can promote this to a `n00b_list_t` without
 *     touching the `n00b_attest_register_verifier_backend` ABI.
 */

#include <attest/n00b_attest_verifier.h>

#include "internal/attest/verifier_backends.h"
#include "internal/attest/verifier_backends/file.h"

#include "core/string.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Backend registration list.
// ---------------------------------------------------------------------------

#define N00B_ATTEST_MAX_REGISTERED_VERIFIER_BACKENDS 16

static n00b_attest_verifier_backend_t *k_registered_verifier_backends
    [N00B_ATTEST_MAX_REGISTERED_VERIFIER_BACKENDS] = {};
static size_t k_n_registered_verifier_backends     = 0;

n00b_result_t(bool)
n00b_attest_register_verifier_backend(n00b_attest_verifier_backend_t *backend)
{
    if (backend == nullptr || backend->scheme == nullptr) {
        // Null pointer input — per D-047 W-1 this surfaces as the
        // dedicated `_VERIFY_BAD_INPUT` code (Phase 2 placeholder
        // was `_VERIFIER_KEY_NOT_FOUND`).
        return n00b_result_err(bool, N00B_ATTEST_ERR_VERIFY_BAD_INPUT);
    }
    if (k_n_registered_verifier_backends
        >= N00B_ATTEST_MAX_REGISTERED_VERIFIER_BACKENDS) {
        // Out of room. WP-003 ships one verifier backend; the cap
        // (16) is well above any near-future in-tree count. If a
        // future WP hits this, promote storage to a `n00b_list_t`.
        // Per D-047 W-1 the capacity-exceeded condition surfaces
        // through `_VERIFY_BAD_INPUT` (Phase 2 placeholder was
        // `_VERIFIER_KEY_NOT_FOUND`); see the @details block on
        // the code for the rationale on co-coding null-input and
        // capacity-exceeded.
        return n00b_result_err(bool, N00B_ATTEST_ERR_VERIFY_BAD_INPUT);
    }
    k_registered_verifier_backends[k_n_registered_verifier_backends++] = backend;
    return n00b_result_ok(bool, true);
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Extract the scheme (substring before the first colon) from a
// `ref` URI and look up the matching verifier-backend registration.
// Returns nullptr if `ref` is null/short, has no colon, or no
// backend registration matches the scheme. Mirror of the signer-
// side `resolve_backend_for`.
static n00b_attest_verifier_backend_t *
resolve_backend_for(n00b_string_t *ref)
{
    if (ref == nullptr || ref->u8_bytes == 0) {
        return nullptr;
    }
    const char *colon = memchr(ref->data, ':', ref->u8_bytes);
    if (colon == nullptr) {
        return nullptr;
    }
    size_t scheme_len = (size_t)(colon - ref->data);
    if (scheme_len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < k_n_registered_verifier_backends; i++) {
        n00b_attest_verifier_backend_t *b = k_registered_verifier_backends[i];
        if (b == nullptr || b->scheme == nullptr) {
            continue;
        }
        if (b->scheme->u8_bytes == scheme_len
            && memcmp(b->scheme->data, ref->data, scheme_len) == 0) {
            return b;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public surface — resolver edge.
// ---------------------------------------------------------------------------

n00b_result_t(n00b_attest_verifier_t *)
n00b_attest_verifier_resolve() _kargs
{
    n00b_string_t    *ref       = nullptr;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (ref == nullptr) {
        // FR-VM-2 discovery chain — empty in WP-003.
        return n00b_result_err(n00b_attest_verifier_t *,
                               N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND);
    }

    n00b_attest_verifier_backend_t *backend = resolve_backend_for(ref);
    if (backend == nullptr) {
        return n00b_result_err(n00b_attest_verifier_t *,
                               N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME);
    }

    n00b_attest_verifier_backend_call_opts_t opts = {
        .allocator = allocator,
    };
    return backend->load(ref, &opts);
}

n00b_result_t(bool)
n00b_attest_verifier_check(n00b_attest_verifier_t *verifier,
                           n00b_buffer_t          *bytes,
                           n00b_buffer_t          *sig) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (verifier == nullptr || verifier->backend == nullptr) {
        // Machinery failure: caller handed us a null/uninitialized
        // verifier. Route through Err (not Ok(false)) per D-044
        // OQ-1 — Phase 4's 3-code exit treats this as "exit 2"
        // not "exit 1". Per D-047 W-1 this null-input case at the
        // resolver edge migrates from the Phase 2 placeholder
        // `_VERIFIER_KEY_NOT_FOUND` to the dedicated
        // `_VERIFY_BAD_INPUT` code (the same condition as the
        // file-backend `check` vtable's null-input guard,
        // co-coded per the `_VERIFY_BAD_INPUT` @details block).
        return n00b_result_err(bool,
                               N00B_ATTEST_ERR_VERIFY_BAD_INPUT);
    }
    n00b_attest_verifier_backend_call_opts_t opts = {
        .allocator = allocator,
    };
    return verifier->backend->check(verifier, bytes, sig, &opts);
}

n00b_buffer_t *
n00b_attest_verifier_pubkey_spki_der(n00b_attest_verifier_t *verifier)
{
    // Per architecture §6.1 the SPKI bytes are pre-built at load
    // time; the getter is allocation-free and cannot fail at
    // runtime. A null `verifier` argument is a precondition
    // violation per the header's `@pre` clause; we surface it
    // here as a null return rather than a result-typed error
    // (the public-API shape declares `n00b_buffer_t *`, not
    // `n00b_result_t(n00b_buffer_t *)`).
    if (verifier == nullptr || verifier->backend == nullptr) {
        return nullptr;
    }
    return verifier->backend->pubkey(verifier);
}

n00b_string_t *
n00b_attest_verifier_keyid(n00b_attest_verifier_t *verifier)
{
    // Symmetric with `n00b_attest_verifier_pubkey_spki_der` above:
    // the keyid was built at load time and cached on the backend
    // state. The getter dispatches through the vtable and returns
    // the cached string. A null `verifier` argument is a
    // precondition violation per the header's `@pre` clause; we
    // surface it as a null return (the public-API shape declares
    // `n00b_string_t *`, not a result wrapper).
    if (verifier == nullptr || verifier->backend == nullptr) {
        return nullptr;
    }
    return verifier->backend->keyid(verifier);
}

void
n00b_attest_verifier_release(n00b_attest_verifier_t *verifier)
{
    if (verifier == nullptr || verifier->backend == nullptr) {
        return;
    }
    verifier->backend->release(verifier);
}
