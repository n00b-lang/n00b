#pragma once

/**
 * @file internal/attest/verifier_backends.h
 * @brief Package-private verifier-backend vtable surface.
 *
 * Declarations consumed by `src/attest/verifier.c` (the resolver
 * edge) and each backend's translation unit
 * (`src/attest/verifier_backends/file.c`, future
 * `src/attest/verifier_backends/keychain.m`, etc.). NOT included
 * from the public umbrella `include/attest/n00b_attest.h` —
 * library consumers cannot see this header and therefore cannot
 * construct an opts-struct directly.
 *
 * Structurally mirrors `internal/attest/backends.h` (the signer-
 * side vtable surface). Where the signer's vtable has
 * `load / sign / pubkey / keyid / release`, the verifier's
 * vtable has `load / check / pubkey / keyid / release`.
 *
 * # Why an opts struct lives here
 *
 * The ncc kwarg rewrite binds at the call site against a declared
 * direct signature. A function pointer is reassignable at runtime,
 * so the rewrite has nothing to bind against — function-pointer
 * types in vtables therefore cannot carry `_kargs`. Where the
 * internal call needs to thread an allocator or other knobs
 * through, the resolver flattens its public `_kargs` block into a
 * small opts struct and passes a pointer at the vtable boundary.
 *
 * Per architecture §6.1 this is the **only** opts-struct-by-
 * pointer concession on the project's ncc surface; it lives
 * behind this private header so it cannot leak. Public consumers
 * see only the `_kargs` form on @ref n00b_attest_verifier_resolve
 * etc.
 *
 * # Backend registration
 *
 * Per architecture §6.1 the backend vtable carries an
 * `n00b_string_t *scheme;` field that identifies the URI scheme
 * the backend serves. The vtable is therefore not file-scope
 * `static const`; each backend's source file declares a
 * file-scope mutable vtable instance whose fields are populated
 * once at module-init time (because `n00b_string_from_cstr` is
 * not a constant expression).
 *
 * `n00b_attest_module_init` is the single authoritative entry
 * point that populates each in-tree verifier backend's vtable and
 * calls @ref n00b_attest_register_verifier_backend to wire the
 * backend into the resolver's registration list. The init
 * function is called exactly once during process startup before
 * any verifier-resolve happens; this is the standard libn00b
 * module-init pattern and does NOT use `[[gnu::constructor]]`
 * (which the api-guidelines §4.3 explicitly bans for caching
 * caller state).
 *
 * Out-of-tree verifier backends register via the same
 * @ref n00b_attest_register_verifier_backend entry point. The
 * declaration is package-private for WP-003 (declared here, not
 * in the public umbrella header); a future WP will promote it to
 * the public surface when plugin support ships.
 *
 * The registration list is module-level state in the loosest
 * sense, but it holds pointer-to-vtable values that each backend
 * owns at file scope (no caller allocations), and it is written
 * only during module-init (exactly-once, before any concurrent
 * resolve) and read-only thereafter. Per the §10.9 reading: this
 * is not caller-state caching — it is backend-owned compile-time-
 * known data routed through a runtime-populated indirection so
 * the spec's `n00b_string_t *scheme;` field can carry a real
 * `n00b_string_t *` (which cannot be a file-scope constant).
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_verifier.h>

/**
 * @brief Package-private call options threaded across the
 *        verifier-backend vtable boundary.
 *
 * The resolver edge in `src/attest/verifier.c` flattens the
 * public `_kargs` block (`.allocator = ...`) into this struct
 * once on entry and passes a pointer to the vtable function.
 * New knobs (e.g., a per-call timeout for remote verifier
 * backends in a later WP) land here without disturbing the
 * public surface.
 */
typedef struct {
    /** Optional allocator (nullptr = runtime default). */
    n00b_allocator_t *allocator;
} n00b_attest_verifier_backend_call_opts_t;

/**
 * @brief Forward declaration of the verifier-backend vtable type.
 *
 * The concrete struct definition is below. Forward-declaring
 * gives backend-specific headers a name to refer to without
 * pulling in the full layout.
 */
typedef struct n00b_attest_verifier_backend n00b_attest_verifier_backend_t;

/**
 * @brief Per-verifier-backend interface — scheme + load / check /
 *        pubkey / keyid / release.
 *
 * Each registered verifier backend ships a file-scope vtable
 * instance in its translation unit (e.g., the file backend's
 * `n00b_attest_verifier_backend_file` in
 * `verifier_backends/file.c`). The fields are populated at
 * module-init time (per the registration discussion above). The
 * resolver dispatches by URI scheme to the matching instance.
 *
 * Function-pointer signatures take a `const
 * n00b_attest_verifier_backend_call_opts_t *opts` rather than a
 * `_kargs` block (per the §6.1 rationale above) for the entry
 * points that thread an allocator. A null `opts` is treated the
 * same as an opts with `.allocator = nullptr` (use the runtime
 * default for everything). The `pubkey` and `keyid` getters are
 * allocation-free post-load (the SPKI bytes and keyid string are
 * constructed at load time and handed back unchanged on every
 * call) and therefore take neither an opts struct nor a `_kargs`
 * block.
 */
struct n00b_attest_verifier_backend {
    /**
     * URI scheme this backend serves (e.g., `"file"`, `"keychain"`).
     * Populated at module-init time via `n00b_string_from_cstr`;
     * scheme strings live for the process lifetime.
     */
    n00b_string_t *scheme;

    /**
     * Load a verifier from the supplied URI.
     *
     * @param ref   The full URI string (scheme intact).
     * @param opts  Call-time options (allocator). May be null.
     *
     * @return On success, a verifier handle whose first field is
     *         an `n00b_attest_verifier_backend_t *` pointer back
     *         to this vtable (so the resolver edge can dispatch
     *         through it without a separate type tag).
     */
    n00b_result_t(n00b_attest_verifier_t *)
        (*load)(n00b_string_t                                  *ref,
                const n00b_attest_verifier_backend_call_opts_t *opts);

    /**
     * Verify a signature over an arbitrary byte buffer.
     *
     * @param verifier  The verifier handle.
     * @param bytes     The bytes the signature claims to cover.
     * @param sig       The signature bytes.
     * @param opts      Call-time options. May be null.
     *
     * @return Verdict-encoding `n00b_result_t(bool)`:
     *         - `n00b_result_ok(bool, true)` — verified.
     *         - `n00b_result_ok(bool, false)` — did NOT verify
     *           (verdict, not failure).
     *         - `n00b_result_err(...)` — machinery failure.
     *         Backends MUST NOT collapse the false-verdict case
     *         into `Err`; Phase 4's 3-code exit shape depends on
     *         the verdict/error split.
     */
    n00b_result_t(bool)
        (*check)(n00b_attest_verifier_t                         *verifier,
                 n00b_buffer_t                                  *bytes,
                 n00b_buffer_t                                  *sig,
                 const n00b_attest_verifier_backend_call_opts_t *opts);

    /**
     * Return the verifier's public key in SubjectPublicKeyInfo
     * DER form. Allocation-free post-load — same shape as the
     * signer-side `pubkey` getter.
     *
     * @param verifier  The verifier handle.
     */
    n00b_buffer_t *
        (*pubkey)(n00b_attest_verifier_t *verifier);

    /**
     * Return the verifier's keyid (lowercase-hex SHA-256 of the
     * SPKI DER per D-039). Allocation-free post-load.
     *
     * @param verifier  The verifier handle.
     */
    n00b_string_t *
        (*keyid)(n00b_attest_verifier_t *verifier);

    /**
     * Release the verifier's state, returning its cached buffers
     * to the allocator. Unlike the signer's release this path
     * performs NO `crypto_wipe`: public-key bytes are not secret.
     *
     * @param verifier  The verifier handle. Calling on a null
     *                  pointer is a no-op.
     */
    void (*release)(n00b_attest_verifier_t *verifier);
};

/**
 * @brief Package-internal opaque verifier handle.
 *
 * Every verifier backend's per-handle state struct begins with
 * one of these as its first field. The resolver edge dispatches
 * by reading `verifier->backend->vtable_member(...)` without
 * knowing which backend the verifier came from.
 *
 * The `backend` pointer aliases a file-scope vtable instance
 * populated at module-init time. The pointer is non-owning. The
 * struct is non-const (matches the signer's W-5 const-chain fix
 * from D-039: vtable is mutable during module-init, immutable
 * thereafter, but the type system does not model that
 * transition).
 */
struct n00b_attest_verifier {
    /** Vtable pointer back to the producing backend's instance. */
    n00b_attest_verifier_backend_t *backend;
};

/**
 * @brief Register a verifier backend with the resolver.
 *
 * Package-private entry point: called by
 * `n00b_attest_module_init` for each in-tree verifier backend,
 * and by out-of-tree plugins (when plugin support ships in a
 * future WP) for caller-defined backends. Mirrors
 * @ref n00b_attest_register_backend on the signer side.
 *
 * @param backend  Verifier-backend vtable to register. The
 *                 pointer is stored verbatim in the resolver's
 *                 registration list; the backend must outlive
 *                 every verifier the resolver hands out under
 *                 that scheme (which, for the in-tree case,
 *                 means the process lifetime).
 *
 * @return `n00b_result_ok(bool, true)` on success. Returns
 *         `n00b_result_err(bool, N00B_ATTEST_ERR_VERIFY_BAD_INPUT)`
 *         if (a) `backend` or `backend->scheme` is null (a misuse
 *         that almost certainly indicates a registration bug),
 *         or (b) the registration list is full
 *         (`N00B_ATTEST_MAX_REGISTERED_VERIFIER_BACKENDS`
 *         already reached). The same `_VERIFY_BAD_INPUT` code
 *         covers both conditions because each is a runtime-action
 *         input-validation failure on the registration entry
 *         point; the @c \@details block on the code enumerates
 *         the full triggering-condition set. Per D-047 W-1 these
 *         callsites migrated from the Phase 2 placeholder
 *         @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND (which now
 *         only surfaces on the resolver edge for "backend looked,
 *         key isn't there").
 *
 * @pre Called only during module-init / before any concurrent
 *      verifier resolve; the registration list is not lock-
 *      protected.
 */
extern n00b_result_t(bool)
n00b_attest_register_verifier_backend(n00b_attest_verifier_backend_t *backend);
