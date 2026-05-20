/* src/attest/oci/auth.c — OCI auth-resolver edge + auth-handle
 * lifecycle.
 *
 * Implements the auth-resolver half of the surface declared in
 * include/attest/n00b_attest_oci.h:
 *   - n00b_attest_oci_auth_resolve   (walks the source chain per
 *                                     OQ-1's 3-of-5 subset)
 *   - n00b_attest_oci_auth_release   (no-op-on-null; no crypto_wipe
 *                                     in Phase 1 per the header's
 *                                     "@details" framing)
 *
 * The 3-of-5 source dispatch (D-051 OQ-1):
 *
 *   - N00B_ATTEST_OCI_AUTH_CALLER          — no-op in the resolver
 *                                             (caller threads a
 *                                             pre-built handle into
 *                                             _client_new directly).
 *   - N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON — reads
 *                                             $XDG_CONFIG_HOME/
 *                                             n00b-attest/
 *                                             registries.json (or
 *                                             ~/.config/n00b-attest/
 *                                             registries.json when
 *                                             XDG_CONFIG_HOME is
 *                                             unset). The schema is
 *                                             N00B-specific.
 *   - N00B_ATTEST_OCI_AUTH_ANONYMOUS       — terminal fallback;
 *                                             returns a no-token
 *                                             handle.
 *   - N00B_ATTEST_OCI_AUTH_CRED_HELPER     — future Tier-2 WP;
 *                                             surfaces
 *                                             _OCI_AUTH_SOURCE_NOT_FOUND.
 *   - N00B_ATTEST_OCI_AUTH_KEYCHAIN        — future Tier-2 WP;
 *                                             surfaces
 *                                             _OCI_AUTH_SOURCE_NOT_FOUND.
 *
 * D-045 `alloc_for_call` idiom: every allocating function threads
 * one `alloc_for_call` local through every allocation site.
 *
 * Test-file carve-out (D-030) does NOT apply here — this is a
 * source file, so libc I/O is banned by the standing rule. The
 * registries.json read uses libn00b's n00b_file_open / read
 * surface; the JSON parse uses n00b_json_parse.
 *
 * `getenv` is authorized for env-var config discovery per D-052
 * (project-local libc exception; scoped: read-side only,
 * getenv-only, n00b-attest-only; each call site cites D-052
 * inline). A future libn00b `n00b_getenv` lift (DF-010) makes
 * this exception unnecessary; when that surface ships, the
 * call sites below migrate to it.
 */

#include "internal/attest/oci/registry.h"
#include "internal/attest/json_util.h"
#include <attest/n00b_attest_oci.h>
#include <attest/n00b_attest_error.h>

#include "core/string.h"
#include "core/buffer.h"
#include "core/file.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "parsers/json.h"
#include "adt/dict_untyped.h"
#include "adt/list.h"

#include <stdatomic.h>
#include <stdlib.h>  // getenv
#include <string.h>

// ---------------------------------------------------------------------------
// Helpers — registries.json path resolution.
//
// Per the header doxygen + D-051 OQ-1 framing the resolver looks up
// `$XDG_CONFIG_HOME/n00b-attest/registries.json` (fall back to
// `$HOME/.config/n00b-attest/registries.json` when XDG_CONFIG_HOME
// is unset). Missing file is NOT an error — the resolver returns
// nullptr from this helper and the caller falls through to the
// next source.
// ---------------------------------------------------------------------------

static n00b_string_t *
resolve_registries_json_path(n00b_allocator_t *alloc_for_call)
{
    // getenv() per D-052 (project-local libc exception for env-var
    // config discovery). Future libn00b `n00b_getenv` lift = DF-010.
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        // $XDG_CONFIG_HOME/n00b-attest/registries.json
        size_t xdg_len     = strlen(xdg);
        const char  suffix[]   = "/n00b-attest/registries.json";
        size_t suffix_len  = sizeof(suffix) - 1;
        size_t total_len   = xdg_len + suffix_len;
        char  *buf = n00b_alloc_array_with_opts(
            char,
            total_len + 1,
            &(n00b_alloc_opts_t){.allocator = alloc_for_call});
        memcpy(buf, xdg, xdg_len);
        memcpy(buf + xdg_len, suffix, suffix_len);
        buf[total_len] = '\0';
        return n00b_string_from_raw(buf,
                                    (int64_t)total_len,
                                    .allocator = alloc_for_call);
    }
    // getenv() per D-052 (project-local libc exception); same
    // authorization as the XDG_CONFIG_HOME site above.
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return nullptr;
    }
    size_t home_len    = strlen(home);
    const char  suffix[]   = "/.config/n00b-attest/registries.json";
    size_t suffix_len  = sizeof(suffix) - 1;
    size_t total_len   = home_len + suffix_len;
    char  *buf = n00b_alloc_array_with_opts(
        char,
        total_len + 1,
        &(n00b_alloc_opts_t){.allocator = alloc_for_call});
    memcpy(buf, home, home_len);
    memcpy(buf + home_len, suffix, suffix_len);
    buf[total_len] = '\0';
    return n00b_string_from_raw(buf,
                                (int64_t)total_len,
                                .allocator = alloc_for_call);
}

// ---------------------------------------------------------------------------
// Helpers — read a file in full to a buffer.
//
// Returns nullptr on any I/O error (open / read / size-query). The
// resolver treats nullptr as "file not present or unreadable" and
// falls through to the next source per the source-chain semantics.
// ---------------------------------------------------------------------------

static n00b_buffer_t *
read_file_to_buffer(n00b_string_t *path, n00b_allocator_t *alloc_for_call)
{
    if (path == nullptr) {
        return nullptr;
    }
    auto open_r = n00b_file_open(path);
    if (n00b_result_is_err(open_r)) {
        return nullptr;
    }
    n00b_file_t *f = n00b_result_get(open_r);

    int64_t sz = n00b_file_size(f);
    if (sz < 0) {
        n00b_file_close(f);
        return nullptr;
    }
    auto read_r = n00b_file_read(f, (size_t)sz);
    if (n00b_result_is_err(read_r)) {
        n00b_file_close(f);
        return nullptr;
    }
    n00b_buffer_t *raw = n00b_result_get(read_r);
    n00b_file_close(f);

    // The mmap-backed substrate may hand back a buffer aliased into
    // the mapping; copy into an allocator-owned buffer to decouple
    // lifetime.
    if (raw == nullptr || raw->byte_len == 0) {
        return nullptr;
    }
    return n00b_buffer_from_bytes(raw->data,
                                  (int64_t)raw->byte_len,
                                  .allocator = alloc_for_call);
}

// ---------------------------------------------------------------------------
// Helpers — registries.json source dispatch.
//
// Schema: top-level object keyed by registry hostname. Each entry
// is an object carrying either an `"auth"` field (base64 of
// user:pass — Basic auth) or a `"token"` field (bearer-token
// string). Missing file is NOT an error; malformed JSON surfaces
// to the resolver as nullptr (treated identically to "no
// credentials in this source").
// ---------------------------------------------------------------------------

static n00b_attest_oci_auth_t *
auth_from_registries_json(n00b_string_t    *registry_filter,
                          n00b_allocator_t *alloc_for_call)
{
    n00b_string_t *path = resolve_registries_json_path(alloc_for_call);
    if (path == nullptr) {
        return nullptr;
    }

    n00b_buffer_t *raw = read_file_to_buffer(path, alloc_for_call);
    if (raw == nullptr) {
        // File absent or unreadable — fall through to next source.
        return nullptr;
    }

    const char       *err  = nullptr;
    n00b_json_node_t *root = n00b_json_parse(raw->data, raw->byte_len, &err);
    if (root == nullptr || !n00b_json_is_object(root)) {
        return nullptr;
    }

    // The resolver walks the object keys to find an entry. Two
    // policies, per the header doxygen:
    //   - With a registry filter: return only the entry whose key
    //     byte-equals the filter string. No match -> nullptr.
    //   - Without a filter: return the first entry that yields
    //     usable credentials.
    //
    // The walk uses the same internal-store pattern as
    // `n00b_attest_json_obj_lookup` in `src/attest/json_util.c`:
    // n00b's untyped dict doesn't expose a public foreach macro, so
    // we walk the bucket array directly under atomic_load (matches
    // the precedent established by WP-001's JSON helpers).
    if (root->object == nullptr) {
        return nullptr;
    }
    n00b_dict_untyped_store_t *store = atomic_load(&root->object->store);
    if (store == nullptr) {
        return nullptr;
    }

    n00b_string_t *bearer_key = r"token";
    n00b_string_t *basic_key  = r"auth";

    for (uint32_t i = 0; i <= store->last_slot; i++) {
        n00b_dict_untyped_bucket_t *b = &store->buckets[i];
        if (b->hv == 0) {
            continue;
        }
        const char *host_key = (const char *)b->key;
        if (host_key == nullptr) {
            continue;
        }
        n00b_json_node_t *entry = (n00b_json_node_t *)b->value;
        if (entry == nullptr || !n00b_json_is_object(entry)) {
            continue;
        }
        size_t host_len = strlen(host_key);
        if (registry_filter != nullptr) {
            if (host_len != registry_filter->u8_bytes
                || memcmp(host_key,
                          registry_filter->data,
                          host_len) != 0) {
                continue;
            }
        }

        // Look up "token" (Bearer) and "auth" (Basic). Prefer
        // token over auth when both are present (modern bearer
        // shape over legacy basic).
        n00b_json_node_t *tok = n00b_attest_json_obj_lookup(entry, bearer_key);
        n00b_json_node_t *bas = n00b_attest_json_obj_lookup(entry, basic_key);

        if (tok != nullptr && n00b_json_is_string(tok)
            && tok->string != nullptr) {
            n00b_attest_oci_auth_t *a = n00b_alloc_with_opts(
                n00b_attest_oci_auth_t,
                &(n00b_alloc_opts_t){.allocator = alloc_for_call});
            a->source   = N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON;
            a->registry = n00b_string_from_raw(host_key,
                                               (int64_t)host_len,
                                               .allocator = alloc_for_call);
            a->bearer_token = n00b_buffer_from_bytes(
                tok->string,
                (int64_t)strlen(tok->string),
                .allocator = alloc_for_call);
            a->basic_auth = nullptr;
            a->allocator  = alloc_for_call;
            return a;
        }
        if (bas != nullptr && n00b_json_is_string(bas)
            && bas->string != nullptr) {
            n00b_attest_oci_auth_t *a = n00b_alloc_with_opts(
                n00b_attest_oci_auth_t,
                &(n00b_alloc_opts_t){.allocator = alloc_for_call});
            a->source   = N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON;
            a->registry = n00b_string_from_raw(host_key,
                                               (int64_t)host_len,
                                               .allocator = alloc_for_call);
            a->bearer_token = nullptr;
            a->basic_auth   = n00b_buffer_from_bytes(
                bas->string,
                (int64_t)strlen(bas->string),
                .allocator = alloc_for_call);
            a->allocator    = alloc_for_call;
            return a;
        }
        if (registry_filter != nullptr) {
            // We matched the filter but the entry had neither
            // field; do not fall through to a different key.
            return nullptr;
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Helpers — anonymous source.
// ---------------------------------------------------------------------------

static n00b_attest_oci_auth_t *
auth_anonymous(n00b_string_t    *registry_filter,
               n00b_allocator_t *alloc_for_call)
{
    n00b_attest_oci_auth_t *a = n00b_alloc_with_opts(
        n00b_attest_oci_auth_t,
        &(n00b_alloc_opts_t){.allocator = alloc_for_call});
    a->source       = N00B_ATTEST_OCI_AUTH_ANONYMOUS;
    a->registry     = registry_filter;
    a->bearer_token = nullptr;
    a->basic_auth   = nullptr;
    a->allocator    = alloc_for_call;
    return a;
}

// ---------------------------------------------------------------------------
// Public surface — auth resolver edge.
// ---------------------------------------------------------------------------

n00b_result_t(n00b_attest_oci_auth_t *)
n00b_attest_oci_auth_resolve()
    _kargs {
        n00b_string_t                                 *registry  = nullptr;
        n00b_list_t(n00b_attest_oci_auth_source_t)    *sources   = nullptr;
        n00b_allocator_t                              *allocator = nullptr;
    }
{
    n00b_allocator_t *alloc_for_call = allocator;

    // Determine the source-chain ordering. nullptr -> default chain
    // [CALLER, REGISTRIES_JSON, ANONYMOUS] per the header doxygen.
    n00b_attest_oci_auth_source_t default_chain[3] = {
        N00B_ATTEST_OCI_AUTH_CALLER,
        N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON,
        N00B_ATTEST_OCI_AUTH_ANONYMOUS,
    };

    size_t                          n_sources = 0;
    n00b_attest_oci_auth_source_t  *src_arr   = nullptr;

    if (sources == nullptr) {
        n_sources = 3;
        src_arr   = default_chain;
    } else {
        // Read the caller's list into a stack-allocated array so the
        // dispatch loop below is uniform across both the default-
        // chain and caller-list paths. The list-element type is the
        // enum itself (not a pointer); n00b_list_len / _get expect
        // an lvalue list (NOT a pointer) so we deref the kwarg.
        size_t len = (size_t)n00b_list_len(*sources);
        if (len == 0) {
            // Caller passed an empty list — no sources to walk.
            return n00b_result_err(
                n00b_attest_oci_auth_t *,
                N00B_ATTEST_ERR_OCI_AUTH_SOURCE_NOT_FOUND);
        }
        src_arr = n00b_alloc_array_with_opts(
            n00b_attest_oci_auth_source_t,
            len,
            &(n00b_alloc_opts_t){.allocator = alloc_for_call});
        for (size_t i = 0; i < len; i++) {
            src_arr[i] = n00b_list_get(*sources, (int64_t)i);
        }
        n_sources = len;
    }

    for (size_t i = 0; i < n_sources; i++) {
        switch (src_arr[i]) {
        case N00B_ATTEST_OCI_AUTH_CALLER:
            // No-op in the resolver per the header doxygen. The
            // caller threads a pre-built auth handle into
            // _client_new directly; this slot exists for
            // documentation symmetry with the other sources.
            break;
        case N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON: {
            n00b_attest_oci_auth_t *a = auth_from_registries_json(
                registry, alloc_for_call);
            if (a != nullptr) {
                return n00b_result_ok(n00b_attest_oci_auth_t *, a);
            }
            break;
        }
        case N00B_ATTEST_OCI_AUTH_ANONYMOUS: {
            n00b_attest_oci_auth_t *a = auth_anonymous(registry,
                                                       alloc_for_call);
            return n00b_result_ok(n00b_attest_oci_auth_t *, a);
        }
        case N00B_ATTEST_OCI_AUTH_CRED_HELPER:
        case N00B_ATTEST_OCI_AUTH_KEYCHAIN:
            // Reserved-but-not-implemented in WP-004 Phase 1 per
            // D-051 OQ-1. If the caller listed one of these
            // explicitly, fail with the dedicated code; if it's
            // a falling-through-from-an-earlier-source path (i.e.
            // the next iteration of the loop) we keep walking the
            // chain — but since the default chain doesn't include
            // these, the only way we land here is if the caller
            // passed an explicit list. Treat as "no credentials
            // from this source" and continue walking.
            break;
        default:
            // Unknown enum value — treat as "no credentials" and
            // continue walking. Defensive; the enum is closed in
            // Phase 1 but a future-WP-added value could land here
            // before the dispatch is updated.
            break;
        }
    }

    return n00b_result_err(n00b_attest_oci_auth_t *,
                           N00B_ATTEST_ERR_OCI_AUTH_SOURCE_NOT_FOUND);
}

void
n00b_attest_oci_auth_release(n00b_attest_oci_auth_t *auth)
{
    // No crypto_wipe in WP-004 Phase 1 per the header doxygen
    // ("@details" block on n00b_attest_oci_auth_release). Bearer
    // tokens are short-lived and Basic creds are loaded fresh from
    // disk on each resolve. Under the n00b GC convention the
    // allocator owns the lifetime and reclaims on scope / arena
    // teardown; explicit deallocation here would be premature
    // (the caller may still hold references to fields they pulled
    // out of the handle). This function exists to satisfy the
    // surface-symmetry contract with the verifier-side release
    // (the public surface declares a release entry point).
    (void)auth;
}
