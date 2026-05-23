#pragma once

/**
 * @file internal/attest/backends.h
 * @brief Package-private signer-backend vtable surface.
 *
 * Declarations consumed by `src/attest/signer.c` (the resolver
 * edge) and each backend's translation unit
 * (`src/attest/backends/file.c`, future
 * `src/attest/backends/keychain.m`, etc.). NOT included from the
 * public umbrella `include/attest/n00b_attest.h` — library
 * consumers cannot see this header and therefore cannot construct
 * an opts-struct directly.
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
 * see only the `_kargs` form on @ref n00b_attest_signer_resolve
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
 * point that populates each in-tree backend's vtable and calls
 * @ref n00b_attest_register_backend to wire the backend into
 * the resolver's registration list. The init function is called
 * exactly once during process startup before any signer-resolve
 * happens; this is the standard libn00b module-init pattern and
 * does NOT use `[[gnu::constructor]]` (which the api-guidelines
 * §4.3 explicitly bans for caching caller state).
 *
 * Out-of-tree backends register via the same
 * @ref n00b_attest_register_backend entry point. The declaration
 * is package-private for WP-002 (declared here, not in the public
 * umbrella header); a future WP will promote it to the public
 * surface when plugin support ships. The architecture-spec §6's
 * `extern` declaration of `n00b_attest_register_backend` is
 * forward-looking; the WP-002 placement is the conservative read
 * (no premature public commitment).
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
#include <attest/n00b_attest_signer.h>

/**
 * @brief Package-private call options threaded across the
 *        backend vtable boundary.
 *
 * The resolver edge in `src/attest/signer.c` flattens the public
 * `_kargs` block (`.allocator = ...`) into this struct once on
 * entry and passes a pointer to the vtable function. New knobs
 * (e.g., a per-call timeout for remote backends in a later WP)
 * land here without disturbing the public surface.
 */
typedef struct {
    /** Optional allocator (nullptr = runtime default). */
    n00b_allocator_t *allocator;
} n00b_attest_backend_call_opts_t;

/**
 * @brief Forward declaration of the backend vtable type.
 *
 * The concrete struct definition is below. Forward-declaring
 * gives backend-specific headers a name to refer to without
 * pulling in the full layout.
 */
typedef struct n00b_attest_backend n00b_attest_backend_t;

/**
 * @brief Per-backend interface — scheme + load / sign / pubkey /
 *        release.
 *
 * Each registered backend ships a file-scope vtable instance in
 * its translation unit (e.g., the file backend's
 * `n00b_attest_backend_file` in `backends/file.c`). The fields
 * are populated at module-init time (per the registration
 * discussion above). The resolver dispatches by URI scheme to
 * the matching instance.
 *
 * Function-pointer signatures take a `const
 * n00b_attest_backend_call_opts_t *opts` rather than a `_kargs`
 * block (per the §6.1 rationale above) for the entry points that
 * thread an allocator. A null `opts` is treated the same as an
 * opts with `.allocator = nullptr` (use the runtime default for
 * everything). The `pubkey` getter is allocation-free post-load
 * (the SPKI bytes are constructed at load time and the buffer
 * wrapper handed back unchanged on every call) and therefore
 * takes neither an opts struct nor a `_kargs` block.
 */
struct n00b_attest_backend {
    /**
     * URI scheme this backend serves (e.g., `"file"`, `"keychain"`).
     * Populated at module-init time via `n00b_string_from_cstr`;
     * scheme strings live for the process lifetime.
     */
    n00b_string_t *scheme;

    /**
     * Load a signer from the supplied URI.
     *
     * @param ref   The full URI string (scheme intact).
     * @param opts  Call-time options (allocator). May be null.
     *
     * @return On success, a signer handle whose first field is a
     *         `n00b_attest_backend_t *` pointer back to this
     *         vtable (so the resolver edge can dispatch through
     *         it without a separate type tag).
     */
    n00b_result_t(n00b_attest_signer_t *)
        (*load)(n00b_string_t                         *ref,
                const n00b_attest_backend_call_opts_t *opts);

    /**
     * Sign arbitrary bytes with the signer's secret key.
     *
     * @param signer         The signer handle.
     * @param bytes_to_sign  The bytes to sign.
     * @param opts           Call-time options. May be null.
     *
     * @return The algorithm-appropriate signature bytes on
     *         success.
     */
    n00b_result_t(n00b_buffer_t *)
        (*sign)(n00b_attest_signer_t                  *signer,
                n00b_buffer_t                         *bytes_to_sign,
                const n00b_attest_backend_call_opts_t *opts);

    /**
     * Return the signer's public key in SubjectPublicKeyInfo DER
     * form. Per architecture §6.1 this returns a raw
     * `n00b_buffer_t *` with no `_kargs` and no result wrapper:
     * the SPKI bytes are constructed at load time and stored on
     * the signer state, so the getter is allocation-free and
     * cannot fail at runtime (a null `signer` argument is a
     * precondition violation, not a runtime error).
     *
     * @param signer  The signer handle.
     */
    n00b_buffer_t *
        (*pubkey)(n00b_attest_signer_t *signer);

    /**
     * Return the signer's keyid as a hex-encoded SHA-256 of the
     * SPKI DER (per D-039). Symmetric with @c pubkey above:
     * allocation-free, no opts struct, no result wrapper. The
     * backend constructs the keyid at load time and caches it on
     * the signer state.
     *
     * @param signer  The signer handle.
     */
    n00b_string_t *
        (*keyid)(n00b_attest_signer_t *signer);

    /**
     * Release the signer's state, zeroizing any private key
     * material before the buffer returns to the allocator. Does
     * not take an opts struct because release performs no
     * allocations and threads no allocator forward; the
     * allocator the load path captured already owns every byte
     * the release path must wipe / free.
     *
     * @param signer  The signer handle. Calling on a null pointer
     *                is a no-op.
     */
    void (*release)(n00b_attest_signer_t *signer);
};

/**
 * @brief Package-internal opaque signer handle.
 *
 * Every backend's per-signer state struct begins with one of
 * these as its first field. The resolver edge dispatches by
 * reading `signer->backend->vtable_member(...)` without knowing
 * which backend the signer came from.
 *
 * The `backend` pointer aliases a file-scope vtable instance
 * populated at module-init time. The pointer is non-owning.
 * (It is no longer `const`-qualified — the vtable is mutable
 * during module-init, immutable thereafter, but the type system
 * does not model that transition.)
 */
struct n00b_attest_signer {
    /** Vtable pointer back to the producing backend's instance. */
    n00b_attest_backend_t *backend;
};

/**
 * @brief Register a backend with the resolver.
 *
 * Package-private entry point: called by
 * `n00b_attest_module_init` for each in-tree backend, and by
 * out-of-tree plugins (when plugin support ships in a future
 * WP) for caller-defined backends.
 *
 * @param backend  Backend vtable to register. The pointer is
 *                 stored verbatim in the resolver's registration
 *                 list; the backend must outlive every signer
 *                 the resolver hands out under that scheme
 *                 (which, for the in-tree case, means the
 *                 process lifetime).
 *
 * @return `n00b_result_ok(bool, true)` on success. Returns
 *         `n00b_result_err(bool, N00B_ATTEST_ERR_KEY_NOT_FOUND)`
 *         if `backend` or `backend->scheme` is null (a misuse
 *         that almost certainly indicates a registration bug).
 *
 * @pre Called only during module-init / before any concurrent
 *      signer resolve; the registration list is not lock-
 *      protected.
 */
extern n00b_result_t(bool)
n00b_attest_register_backend(n00b_attest_backend_t *backend);
