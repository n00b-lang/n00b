/* src/attest/cli.c — library-shaped cores of the `n00b-attest`
 * CLI verbs.
 *
 * Public surface declared in `include/attest/n00b_attest_cli.h`;
 * this translation unit lands the bodies. Library-API-first
 * (WP-002 plan §727): the bodies here consume in-memory inputs
 * and return in-memory outputs; the `n00b-attest` tool binary
 * (`src/tools/n00b-attest.c`) is a thin shim that binds stdin /
 * stdout / file paths around these cores. The regression test
 * (`test/unit/test_attest_cli_sign.c`) drives the cores directly
 * via in-memory buffers — never invoking the binary out of
 * process.
 *
 * Allocator threading: every public entry point here forwards
 * the caller's `allocator` kwarg into every downstream call
 * (statement parse + re-serialize, envelope construction,
 * signer resolve, envelope sign, envelope serialize). Per D-042
 * W-2 the signer's resolve-time allocator inherits forward into
 * `_sign`, so the high-level `n00b_attest_envelope_sign` call
 * here picks up the same arena without re-threading. The
 * default-`nullptr` `allocator` falls back to the runtime
 * allocator at the lowest layer; nothing here ever caches a
 * caller allocator (api-guidelines §10.9).
 */

#include <attest/n00b_attest.h>

#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"

#include "internal/attest/oci/registry.h"

#include <string.h>

/* Re-serialize a parsed Statement so the envelope payload carries
 * the canonical (parser-conformant) byte form. The CLI's stdin
 * may have stray whitespace, member reordering, or non-canonical
 * JSON; the wire envelope MUST carry the libn00b-canonical form
 * so verifiers downstream see a stable PAE input. Returns the
 * canonical Statement bytes on success; propagates the parse /
 * serialize error code on failure.
 *
 * Per D-043 (forthcoming — orchestrator will log at WP-002
 * closeout): Statements are canonicalized via libn00b's
 * serializer before being signed. Verifiers get byte-stable wire
 * payloads regardless of input whitespace / member ordering.
 * Matches JOSE / XML-DSIG precedent. The signature is therefore
 * over the canonical form, not the verbatim input bytes —
 * callers needing the verbatim form must canonicalize before
 * invoking `n00b_attest_cli_sign`. */
static n00b_result_t(n00b_buffer_t *)
canonicalize_statement(n00b_buffer_t    *statement_bytes,
                       n00b_allocator_t *allocator)
{
    auto parse_r = n00b_attest_statement_parse(statement_bytes,
                                               .allocator = allocator);
    if (n00b_result_is_err(parse_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(parse_r));
    }
    n00b_attest_statement_t *st = n00b_result_get(parse_r);

    auto ser_r = n00b_attest_statement_serialize(st,
                                                 .allocator = allocator);
    if (n00b_result_is_err(ser_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(ser_r));
    }
    return ser_r;
}

n00b_result_t(n00b_buffer_t *)
n00b_attest_cli_sign(n00b_buffer_t *statement_bytes,
                     n00b_string_t *key_uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (statement_bytes == nullptr || statement_bytes->byte_len == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_STMT_BAD_INPUT);
    }
    if (key_uri == nullptr || key_uri->u8_bytes == 0) {
        // No key URI to dispatch on; this is the resolver's
        // "empty discovery chain" branch surfaced one frame up.
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_KEY_NOT_FOUND);
    }

    // 1+2. Parse + re-serialize so the envelope payload carries
    // the canonical Statement bytes.
    auto canon_r = canonicalize_statement(statement_bytes, allocator);
    if (n00b_result_is_err(canon_r)) {
        return canon_r;
    }
    n00b_buffer_t *canon = n00b_result_get(canon_r);

    // 3. Wrap in an envelope.
    n00b_attest_envelope_t *env = n00b_attest_envelope_new(
        .allocator = allocator);
    auto set_r = n00b_attest_envelope_set_payload(env, canon);
    if (n00b_result_is_err(set_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(set_r));
    }

    // 4. Resolve the signer. Per D-042 W-2 the signer remembers
    // this allocator and inherits it forward into `_sign`.
    auto resolve_r = n00b_attest_signer_resolve(.ref       = key_uri,
                                                .allocator = allocator);
    if (n00b_result_is_err(resolve_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(resolve_r));
    }
    n00b_attest_signer_t *signer = n00b_result_get(resolve_r);

    // 5. High-level sign: PAE → sign → keyid → add_signature.
    // Per D-041 the CLI uses this entry point, NOT the low-level
    // `add_signature` directly.
    auto sign_r = n00b_attest_envelope_sign(env,
                                            signer,
                                            .allocator = allocator);
    if (n00b_result_is_err(sign_r)) {
        n00b_err_t code = n00b_result_get_err(sign_r);
        n00b_attest_signer_release(signer);
        return n00b_result_err(n00b_buffer_t *, code);
    }

    // 6. Serialize to canonical wire JSON (D-024: pretty = false,
    // satisfied by `n00b_attest_envelope_serialize`'s body).
    auto ser_r = n00b_attest_envelope_serialize(env,
                                                .allocator = allocator);
    if (n00b_result_is_err(ser_r)) {
        n00b_err_t code = n00b_result_get_err(ser_r);
        n00b_attest_signer_release(signer);
        return n00b_result_err(n00b_buffer_t *, code);
    }

    // 7. Release the signer — wipes the private key material
    // (FR-SM-3 / file_release).
    n00b_attest_signer_release(signer);

    return ser_r;
}

n00b_result_t(bool)
n00b_attest_cli_verify(n00b_buffer_t *envelope_bytes,
                       n00b_string_t *key_uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    if (envelope_bytes == nullptr || envelope_bytes->byte_len == 0) {
        return n00b_result_err(bool, N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (key_uri == nullptr || key_uri->u8_bytes == 0) {
        // No verifier URI to dispatch on; this is the resolver's
        // "empty discovery chain" branch surfaced one frame up —
        // mirrors the signer-side _KEY_NOT_FOUND treatment on the
        // verifier-domain code namespace.
        return n00b_result_err(bool,
                               N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND);
    }

    // 1. Parse the envelope. Per Phase 1 (DF-006 closure) the
    // parser reconstructs `signatures[]` from the wire JSON so the
    // subsequent verify call sees the appended `{keyid, sig}`
    // pairs. The parsed envelope is GC-managed — no explicit
    // release needed on any return path.
    auto parse_r = n00b_attest_envelope_parse(envelope_bytes,
                                              .allocator = allocator);
    if (n00b_result_is_err(parse_r)) {
        return n00b_result_err(bool, n00b_result_get_err(parse_r));
    }
    n00b_attest_envelope_t *env = n00b_result_get(parse_r);

    // 2. Resolve the verifier. Per D-042 W-2 the verifier
    // remembers this allocator and inherits it forward into
    // subsequent `_envelope_verify` scratch.
    auto resolve_r = n00b_attest_verifier_resolve(.ref       = key_uri,
                                                  .allocator = allocator);
    if (n00b_result_is_err(resolve_r)) {
        // Verifier never constructed; nothing to release. The
        // parsed envelope is GC-managed.
        return n00b_result_err(bool, n00b_result_get_err(resolve_r));
    }
    n00b_attest_verifier_t *verifier = n00b_result_get(resolve_r);

    // 3. High-level verify: sigstore-style any-matching-keyid-
    // passes (D-041 high-level wrapper; D-044 Q3 disposition).
    // Returns the verdict on Ok and machinery failures on Err;
    // both shapes must propagate unchanged so Phase 4's 3-code
    // exit shape (D-044 OQ-1 (b)) sees the verdict/Err split.
    auto verify_r = n00b_attest_envelope_verify(env,
                                                verifier,
                                                .allocator = allocator);

    // 4. **Defensive release**: the verifier is live regardless of
    // whether `_envelope_verify` returned Ok(true), Ok(false), or
    // Err — mirror of the signer-side release-on-error pattern in
    // `n00b_attest_cli_sign`. Release BEFORE returning so no
    // return path leaks the verifier's cached buffers.
    n00b_attest_verifier_release(verifier);

    return verify_r;
}

/* ------------------------------------------------------------------
 * Push verb (WP-004 Phase 2).
 *
 * Composes:
 *   1. OCI URL parse (internal helper) → registry / name / digest.
 *   2. Auth resolve.
 *   3. Client construct.
 *   4. Envelope parse + keyid extraction from signatures[0].
 *   5. Envelope payload → Statement parse → predicate-type
 *      extraction (or use the override kwarg if set).
 *   6. Push orchestrator.
 *
 * Defensive release discipline: BOTH the client and auth handles
 * are released on every return path including Err (mirror of
 * verify-side release-on-error pattern).
 * ------------------------------------------------------------------ */

// Build the registry origin string from a registry hostname:
// prepend `https://`. Caller controls whether the input came from
// `_url_parse` or the override kwarg.
static n00b_string_t *
build_registry_origin(n00b_string_t *host, n00b_allocator_t *allocator)
{
    if (host == nullptr || host->u8_bytes == 0) {
        return nullptr;
    }
    static const char prefix[] = "https://";
    size_t pfx = sizeof(prefix) - 1;
    size_t total = pfx + host->u8_bytes;
    char *buf = n00b_alloc_array_with_opts(
        char,
        total + 1,
        &(n00b_alloc_opts_t){.allocator = allocator});
    memcpy(buf, prefix, pfx);
    memcpy(buf + pfx, host->data, host->u8_bytes);
    buf[total] = '\0';
    return n00b_string_from_raw(buf,
                                (int64_t)total,
                                .allocator = allocator);
}

n00b_result_t(n00b_string_t *)
n00b_attest_cli_push(n00b_buffer_t *envelope_bytes,
                     n00b_string_t *image_ref) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_string_t    *predicate_type    = nullptr;
    n00b_allocator_t *allocator         = nullptr;
}
{
    n00b_attest_oci_auth_t   *auth   = nullptr;
    n00b_attest_oci_client_t *client = nullptr;
    n00b_err_t                err_code = 0;

    if (envelope_bytes == nullptr || envelope_bytes->byte_len == 0) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    // 1. Parse the image ref.
    auto url_r = n00b_attest_oci_url_parse(image_ref,
                                            .allocator = allocator);
    if (n00b_result_is_err(url_r)) {
        return n00b_result_err(n00b_string_t *,
                               n00b_result_get_err(url_r));
    }
    n00b_attest_oci_image_ref_t *parsed = n00b_result_get(url_r);

    if (parsed->digest == nullptr || parsed->digest->u8_bytes == 0) {
        // Push requires a digest-pinned subject (the attestation
        // is *of* a specific manifest, not a moving tag).
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    // 2. Choose registry hostname: override wins; otherwise
    // parsed.registry; reject if neither.
    n00b_string_t *registry_host = registry_override != nullptr
                                        && registry_override->u8_bytes > 0
                                       ? registry_override
                                       : parsed->registry;
    if (registry_host == nullptr || registry_host->u8_bytes == 0) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *registry_url = build_registry_origin(registry_host,
                                                         allocator);
    if (registry_url == nullptr) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    // 3. Parse envelope (we need keyid; optionally predicate-type).
    auto env_r = n00b_attest_envelope_parse(envelope_bytes,
                                             .allocator = allocator);
    if (n00b_result_is_err(env_r)) {
        return n00b_result_err(n00b_string_t *,
                               n00b_result_get_err(env_r));
    }
    n00b_attest_envelope_t *env = n00b_result_get(env_r);

    // Walk signatures[0].keyid for the manifest annotation. WP-003
    // P1 surface (do not re-implement).
    if (n00b_attest_envelope_signature_count(env) == 0) {
        return n00b_result_err(n00b_string_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }
    auto kid_r = n00b_attest_envelope_get_signature_keyid(env, 0);
    if (n00b_result_is_err(kid_r)) {
        return n00b_result_err(n00b_string_t *,
                               n00b_result_get_err(kid_r));
    }
    n00b_string_t *signer_keyid = n00b_result_get(kid_r);

    // 4. Determine predicate-type. Kwarg wins; else walk
    // envelope.payload → Statement.predicate_type. WP-001 surface.
    n00b_string_t *use_predicate_type = nullptr;
    if (predicate_type != nullptr && predicate_type->u8_bytes > 0) {
        use_predicate_type = predicate_type;
    }
    else {
        auto pl_r = n00b_attest_envelope_get_payload(env);
        if (n00b_result_is_err(pl_r)) {
            return n00b_result_err(n00b_string_t *,
                                   n00b_result_get_err(pl_r));
        }
        n00b_buffer_t *payload = n00b_result_get(pl_r);
        auto st_r = n00b_attest_statement_parse(payload,
                                                 .allocator = allocator);
        if (n00b_result_is_err(st_r)) {
            return n00b_result_err(n00b_string_t *,
                                   n00b_result_get_err(st_r));
        }
        n00b_attest_statement_t *st = n00b_result_get(st_r);
        use_predicate_type = n00b_attest_statement_get_predicate_type(st);
        if (use_predicate_type == nullptr
            || use_predicate_type->u8_bytes == 0) {
            return n00b_result_err(n00b_string_t *,
                                   N00B_ATTEST_ERR_STMT_MISSING_FIELD);
        }
    }

    // 5. Resolve auth (default chain — caller / registries.json /
    // anonymous), bound to the chosen registry.
    auto auth_r = n00b_attest_oci_auth_resolve(.registry  = registry_host,
                                                .allocator = allocator);
    if (n00b_result_is_err(auth_r)) {
        return n00b_result_err(n00b_string_t *,
                               n00b_result_get_err(auth_r));
    }
    auth = n00b_result_get(auth_r);

    // 6. Construct OCI client.
    auto client_r = n00b_attest_oci_client_new(registry_url,
                                                .auth      = auth,
                                                .allocator = allocator);
    if (n00b_result_is_err(client_r)) {
        err_code = n00b_result_get_err(client_r);
        goto release_auth_only;
    }
    client = n00b_result_get(client_r);

    // 7. Dispatch push orchestrator.
    auto push_r = n00b_attest_oci_push_attestation(
        client,
        parsed->name,
        parsed->digest,
        envelope_bytes,
        .predicate_type = use_predicate_type,
        .signer_keyid   = signer_keyid,
        .allocator      = allocator);

    // Defensive release: BOTH handles, on every code path
    // (mirror of verify-side release-on-error). The release
    // primitives are null-safe.
    n00b_attest_oci_client_release(client);
    n00b_attest_oci_auth_release(auth);

    if (n00b_result_is_err(push_r)) {
        return n00b_result_err(n00b_string_t *,
                               n00b_result_get_err(push_r));
    }
    return push_r;

release_auth_only:
    n00b_attest_oci_auth_release(auth);
    return n00b_result_err(n00b_string_t *, err_code);
}

/* ------------------------------------------------------------------
 * Discover verb (WP-004 Phase 3).
 *
 * Composes:
 *   1. OCI URL parse → registry / name / digest.
 *   2. Auth resolve (default chain: caller / registries.json /
 *      anonymous).
 *   3. Client construct.
 *   4. List referrers (with optional server-side artifact-type
 *      filter; D-051 OQ-3 BOTH, server-side half).
 *
 * Defensive release discipline: BOTH the client and auth handles
 * are released on every return path including Err.
 * ------------------------------------------------------------------ */

n00b_result_t(n00b_list_t(n00b_attest_oci_referrer_t *) *)
n00b_attest_cli_discover(n00b_string_t *image_ref) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_string_t    *artifact_type     = nullptr;
    n00b_allocator_t *allocator         = nullptr;
}
{
    n00b_attest_oci_auth_t   *auth   = nullptr;
    n00b_attest_oci_client_t *client = nullptr;
    n00b_err_t                err_code = 0;

    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    auto url_r = n00b_attest_oci_url_parse(image_ref,
                                            .allocator = allocator);
    if (n00b_result_is_err(url_r)) {
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            n00b_result_get_err(url_r));
    }
    n00b_attest_oci_image_ref_t *parsed = n00b_result_get(url_r);

    if (parsed->digest == nullptr || parsed->digest->u8_bytes == 0) {
        // Discover requires a digest-pinned subject (attestations
        // are *of* a specific manifest).
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *registry_host = registry_override != nullptr
                                        && registry_override->u8_bytes > 0
                                       ? registry_override
                                       : parsed->registry;
    if (registry_host == nullptr || registry_host->u8_bytes == 0) {
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *registry_url = build_registry_origin(registry_host,
                                                         allocator);
    if (registry_url == nullptr) {
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    auto auth_r = n00b_attest_oci_auth_resolve(.registry  = registry_host,
                                                .allocator = allocator);
    if (n00b_result_is_err(auth_r)) {
        return n00b_result_err(
            n00b_list_t(n00b_attest_oci_referrer_t *) *,
            n00b_result_get_err(auth_r));
    }
    auth = n00b_result_get(auth_r);

    auto client_r = n00b_attest_oci_client_new(registry_url,
                                                .auth      = auth,
                                                .allocator = allocator);
    if (n00b_result_is_err(client_r)) {
        err_code = n00b_result_get_err(client_r);
        goto discover_release_auth_only;
    }
    client = n00b_result_get(client_r);

    auto list_r = n00b_attest_oci_list_referrers(
        client,
        parsed->name,
        parsed->digest,
        .artifact_type = artifact_type,
        .allocator     = allocator);

    n00b_attest_oci_client_release(client);
    n00b_attest_oci_auth_release(auth);
    return list_r;

discover_release_auth_only:
    n00b_attest_oci_auth_release(auth);
    return n00b_result_err(n00b_list_t(n00b_attest_oci_referrer_t *) *,
                           err_code);
}

/* ------------------------------------------------------------------
 * Pull verb (WP-004 Phase 3).
 *
 * Composes:
 *   1. OCI URL parse → registry / name / digest.
 *   2. Auth resolve.
 *   3. Client construct.
 *   4. List referrers with server-side artifact-type filter on
 *      `application/vnd.in-toto+dsse` (D-051 OQ-3 BOTH,
 *      server-side half).
 *   5. Client-side post-filter on predicate-type annotation
 *      (D-051 OQ-3 BOTH, client-side half — defense-in-depth).
 *   6. Pick the FIRST matching entry (multi-attestation with the
 *      same predicate-type is allowed but pull returns one; the
 *      "error on multiple" path is flagged for orchestrator).
 *   7. Pull envelope bytes.
 *
 * Defensive release discipline: BOTH the client and auth handles
 * are released on every return path including Err.
 * ------------------------------------------------------------------ */

n00b_result_t(n00b_buffer_t *)
n00b_attest_cli_pull(n00b_string_t *image_ref,
                     n00b_string_t *predicate_type) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_allocator_t *allocator         = nullptr;
}
{
    n00b_attest_oci_auth_t   *auth   = nullptr;
    n00b_attest_oci_client_t *client = nullptr;
    n00b_err_t                err_code = 0;

    if (image_ref == nullptr || image_ref->u8_bytes == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }
    if (predicate_type == nullptr || predicate_type->u8_bytes == 0) {
        // The plan forces predicate-type explicit to avoid ambiguity
        // (multi-attestation per image is allowed); a null narrows
        // to no entry and we surface bad-input over the empty-list
        // pull-side `_OCI_NO_MATCHING_REFERRER` because the call was
        // structurally invalid, not "search yielded zero."
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_STMT_BAD_INPUT);
    }

    auto url_r = n00b_attest_oci_url_parse(image_ref,
                                            .allocator = allocator);
    if (n00b_result_is_err(url_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(url_r));
    }
    n00b_attest_oci_image_ref_t *parsed = n00b_result_get(url_r);

    if (parsed->digest == nullptr || parsed->digest->u8_bytes == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *registry_host = registry_override != nullptr
                                        && registry_override->u8_bytes > 0
                                       ? registry_override
                                       : parsed->registry;
    if (registry_host == nullptr || registry_host->u8_bytes == 0) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    n00b_string_t *registry_url = build_registry_origin(registry_host,
                                                         allocator);
    if (registry_url == nullptr) {
        return n00b_result_err(n00b_buffer_t *,
                               N00B_ATTEST_ERR_OCI_BAD_URL);
    }

    auto auth_r = n00b_attest_oci_auth_resolve(.registry  = registry_host,
                                                .allocator = allocator);
    if (n00b_result_is_err(auth_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(auth_r));
    }
    auth = n00b_result_get(auth_r);

    auto client_r = n00b_attest_oci_client_new(registry_url,
                                                .auth      = auth,
                                                .allocator = allocator);
    if (n00b_result_is_err(client_r)) {
        err_code = n00b_result_get_err(client_r);
        goto pull_release_auth_only_err;
    }
    client = n00b_result_get(client_r);

    auto list_r = n00b_attest_oci_list_referrers(
        client,
        parsed->name,
        parsed->digest,
        .artifact_type = r"application/vnd.in-toto+dsse",
        .allocator     = allocator);
    if (n00b_result_is_err(list_r)) {
        err_code = n00b_result_get_err(list_r);
        goto pull_release_both_err;
    }
    n00b_list_t(n00b_attest_oci_referrer_t *) *refs = n00b_result_get(list_r);

    // Client-side post-filter on predicate-type. Walk the list and
    // pick the first entry whose `predicate_type` annotation matches
    // the caller's `predicate_type` exactly.
    n00b_attest_oci_referrer_t *picked = nullptr;
    size_t nrefs = refs->len;
    for (size_t i = 0; i < nrefs; i++) {
        n00b_attest_oci_referrer_t *r = refs->data[i];
        if (r == nullptr || r->predicate_type == nullptr) {
            continue;
        }
        if (r->predicate_type->u8_bytes == predicate_type->u8_bytes
            && memcmp(r->predicate_type->data,
                       predicate_type->data,
                       predicate_type->u8_bytes) == 0) {
            picked = r;
            break;
        }
    }
    if (picked == nullptr) {
        err_code = N00B_ATTEST_ERR_OCI_NO_MATCHING_REFERRER;
        goto pull_release_both_err;
    }

    auto pull_r = n00b_attest_oci_pull_envelope(client,
                                                 parsed->name,
                                                 picked->manifest_digest,
                                                 .allocator = allocator);

    n00b_attest_oci_client_release(client);
    n00b_attest_oci_auth_release(auth);

    if (n00b_result_is_err(pull_r)) {
        return n00b_result_err(n00b_buffer_t *,
                               n00b_result_get_err(pull_r));
    }
    return pull_r;

pull_release_both_err:
    n00b_attest_oci_client_release(client);
    n00b_attest_oci_auth_release(auth);
    return n00b_result_err(n00b_buffer_t *, err_code);

pull_release_auth_only_err:
    n00b_attest_oci_auth_release(auth);
    return n00b_result_err(n00b_buffer_t *, err_code);
}

/* ------------------------------------------------------------------
 * Mark verb (WP-005 Phase 2).
 *
 * Composes:
 *   1. Walk envelope_bytes_list; for each blob run
 *      `n00b_attest_envelope_parse` to materialize an envelope
 *      handle.
 *   2. Dispatch through `n00b_attest_mark_artifact`, forwarding the
 *      `bundled`, `registry_hint`, and `allocator` kwargs verbatim.
 *
 * No long-lived handles to release (no OCI client, no signer, no
 * verifier). The parsed envelopes are GC-managed by the per-call
 * allocator.
 * ------------------------------------------------------------------ */

n00b_result_t(n00b_attest_mark_result_t *)
n00b_attest_cli_mark(n00b_string_t                *artifact_path,
                     n00b_list_t(n00b_buffer_t *) *envelope_bytes_list) _kargs
{
    bool                          bundled         = true;
    n00b_string_t                *registry_hint   = nullptr;
    n00b_allocator_t             *allocator       = nullptr;
    n00b_chalk_signer_identity_t *signer_identity = nullptr;
}
{
    if (artifact_path == nullptr || artifact_path->u8_bytes == 0
        || envelope_bytes_list == nullptr) {
        return n00b_result_err(n00b_attest_mark_result_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }

    size_t n_blobs = (size_t)n00b_list_len(*envelope_bytes_list);
    if (n_blobs == 0) {
        return n00b_result_err(n00b_attest_mark_result_t *,
                               N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    }

    // Materialize the envelope-handle list from the wire-bytes
    // blobs. Per-blob parse errors propagate verbatim (DSSE_BAD_JSON
    // / _WRONG_TYPE / _BAD_BASE64); a null blob surfaces as
    // DSSE_BAD_INPUT (the caller's invariant violation).
    n00b_list_t(n00b_attest_envelope_t *) envs =
        n00b_list_new(n00b_attest_envelope_t *);
    for (size_t i = 0; i < n_blobs; i++) {
        n00b_buffer_t *blob = n00b_list_get(*envelope_bytes_list,
                                            (int64_t)i);
        if (blob == nullptr || blob->byte_len == 0) {
            return n00b_result_err(n00b_attest_mark_result_t *,
                                   N00B_ATTEST_ERR_DSSE_BAD_INPUT);
        }
        auto pr = n00b_attest_envelope_parse(blob,
                                              .allocator = allocator);
        if (n00b_result_is_err(pr)) {
            return n00b_result_err(n00b_attest_mark_result_t *,
                                   n00b_result_get_err(pr));
        }
        n00b_list_push(envs, n00b_result_get(pr));
    }

    return n00b_attest_mark_artifact(artifact_path,
                                      &envs,
                                      .bundled         = bundled,
                                      .registry_hint   = registry_hint,
                                      .allocator       = allocator,
                                      .signer_identity = signer_identity);
}

/* ------------------------------------------------------------------
 * Unmark verb (WP-005 Phase 2).
 *
 * Direct passthrough to `n00b_attest_unmark`. The wrapper exists to
 * preserve the library-API-first verb-core pattern (D-044) — the
 * shim in n00b-attest.c calls only the `_cli_*` family.
 * ------------------------------------------------------------------ */

n00b_result_t(bool)
n00b_attest_cli_unmark(n00b_string_t *artifact_path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return n00b_attest_unmark(artifact_path,
                               .allocator = allocator);
}

/* ------------------------------------------------------------------
 * Extract verb (WP-005 Phase 2).
 *
 * Direct passthrough to `n00b_attest_extract_from_artifact`. The
 * shim maps the four IC-5 Err codes onto the §10.1 exit-code split
 * (0 = mark + valid ATTESTATION, 1 = no usable verdict, 2 =
 * machinery failure).
 * ------------------------------------------------------------------ */

n00b_result_t(n00b_attest_extract_result_t *)
n00b_attest_cli_extract(n00b_string_t *artifact_path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
}
{
    return n00b_attest_extract_from_artifact(artifact_path,
                                              .allocator = allocator);
}
