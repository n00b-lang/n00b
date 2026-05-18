/* src/attest/signer.c — signer resolver edge.
 *
 * Implements the surface declared in
 * include/attest/n00b_attest_signer.h:
 *   - n00b_attest_signer_resolve            (URI dispatch by scheme)
 *   - n00b_attest_signer_sign               (dispatch through backend.sign)
 *   - n00b_attest_signer_pubkey_spki_der    (dispatch through backend.pubkey)
 *   - n00b_attest_signer_release            (dispatch through backend.release)
 *
 * Plus the package-private:
 *   - n00b_attest_register_backend          (architecture §6.1; resolver
 *                                            consults the registration
 *                                            list at every resolve)
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
 *   - The fixed capacity (16) is well above WP-002's single
 *     entry and far above the foreseeable in-tree count; a future
 *     WP that ships plugin support and needs unbounded capacity
 *     can promote this to a `n00b_list_t` without touching the
 *     `n00b_attest_register_backend` ABI.
 *
 * The list is module-level state in a strict sense, but it caches
 * only backend-owned compile-time-known pointers (each backend's
 * file-scope vtable instance); no caller allocations are stashed
 * here, so the api-guidelines §10.9 "no module-level state" rule
 * is honored in spirit. The architecture spec's §6.1 registration
 * model explicitly anticipates this shape.
 *
 * Kwarg-to-opts flattening:
 *
 *   The public `_kargs` block carries `.allocator`. The internal
 *   vtable boundary takes a `const n00b_attest_backend_call_opts_t
 *   *opts` for `load` and `sign`. The resolver populates a stack
 *   `n00b_attest_backend_call_opts_t` once, passes its address,
 *   and lets the backend thread the allocator forward into
 *   whatever it allocates. The `pubkey` getter is allocation-
 *   free post-load and takes no opts (matching architecture
 *   §6.1's spec-canonical signature); the resolver dispatches
 *   straight through.
 */

#include <attest/n00b_attest_signer.h>

#include "internal/attest/backends.h"
#include "internal/attest/backends/file.h"

#include "core/string.h"

#include <string.h>

// ---------------------------------------------------------------------------
// Backend registration list.
// ---------------------------------------------------------------------------

#define N00B_ATTEST_MAX_REGISTERED_BACKENDS 16

static n00b_attest_backend_t *k_registered_backends
    [N00B_ATTEST_MAX_REGISTERED_BACKENDS] = {};
static size_t k_n_registered_backends     = 0;

n00b_result_t(bool)
n00b_attest_register_backend(n00b_attest_backend_t *backend)
{
    if (backend == nullptr || backend->scheme == nullptr) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_KEY_NOT_FOUND);
    }
    if (k_n_registered_backends >= N00B_ATTEST_MAX_REGISTERED_BACKENDS) {
        // Out of room. WP-002 ships one backend; the cap (16) is
        // well above any near-future in-tree count. If a future WP
        // hits this, promote the storage to a `n00b_list_t` (see
        // the rationale block at the top of this file).
        return n00b_result_err(bool, N00B_ATTEST_ERR_KEY_NOT_FOUND);
    }
    k_registered_backends[k_n_registered_backends++] = backend;
    return n00b_result_ok(bool, true);
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

// Extract the scheme (substring before the first colon) from a
// `ref` URI and look up the matching backend registration. Returns
// nullptr if `ref` is null/short, has no colon, or no backend
// registration matches the scheme.
//
// Comparison is exact byte-equal — schemes are ASCII-only per RFC
// 3986 § 3.1, so a lower-case `file` doesn't have to handle case
// folding. (If we later need case-insensitive scheme matching, the
// change is local to this helper.)
static n00b_attest_backend_t *
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
    for (size_t i = 0; i < k_n_registered_backends; i++) {
        n00b_attest_backend_t *b = k_registered_backends[i];
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

n00b_result_t(n00b_attest_signer_t *)
n00b_attest_signer_resolve() _kargs
{
    n00b_string_t    *ref       = nullptr;
    n00b_allocator_t *allocator = nullptr;
}
{
    if (ref == nullptr) {
        // FR-SM-2 discovery chain — empty in WP-002.
        return n00b_result_err(n00b_attest_signer_t *,
                               N00B_ATTEST_ERR_KEY_NOT_FOUND);
    }

    n00b_attest_backend_t *backend = resolve_backend_for(ref);
    if (backend == nullptr) {
        return n00b_result_err(n00b_attest_signer_t *,
                               N00B_ATTEST_ERR_UNSUPPORTED_SCHEME);
    }

    n00b_attest_backend_call_opts_t opts = {
        .allocator = allocator,
    };
    return backend->load(ref, &opts);
}

n00b_result_t(n00b_buffer_t *)
n00b_attest_signer_sign(n00b_attest_signer_t *signer,
                        n00b_buffer_t        *bytes_to_sign) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (signer == nullptr || signer->backend == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_SIGN_FAILED);
    }
    n00b_attest_backend_call_opts_t opts = {
        .allocator = allocator,
    };
    return signer->backend->sign(signer, bytes_to_sign, &opts);
}

n00b_buffer_t *
n00b_attest_signer_pubkey_spki_der(n00b_attest_signer_t *signer)
{
    // Per architecture §6.1 the SPKI bytes are pre-built at load
    // time; the getter is allocation-free and cannot fail at
    // runtime. A null `signer` argument is a precondition
    // violation per the header's `@pre` clause; we surface it
    // here as a null return rather than a result-typed error
    // (the public-API shape declares `n00b_buffer_t *`, not
    // `n00b_result_t(n00b_buffer_t *)`).
    if (signer == nullptr || signer->backend == nullptr) {
        return nullptr;
    }
    return signer->backend->pubkey(signer);
}

n00b_string_t *
n00b_attest_signer_keyid(n00b_attest_signer_t *signer)
{
    // Symmetric with `n00b_attest_signer_pubkey_spki_der` above:
    // the keyid was built at load time and cached on the backend
    // state. The getter dispatches through the vtable and returns
    // the cached string. A null `signer` argument is a
    // precondition violation per the header's `@pre` clause; we
    // surface it as a null return (the public-API shape declares
    // `n00b_string_t *`, not a result wrapper).
    if (signer == nullptr || signer->backend == nullptr) {
        return nullptr;
    }
    return signer->backend->keyid(signer);
}

void
n00b_attest_signer_release(n00b_attest_signer_t *signer)
{
    if (signer == nullptr || signer->backend == nullptr) {
        return;
    }
    signer->backend->release(signer);
}
