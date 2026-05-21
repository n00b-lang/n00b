#pragma once

/**
 * @file n00b_attest_cli.h
 * @brief Library-shaped cores of the `n00b-attest` CLI verbs.
 *
 * The `n00b-attest` tool binary (`src/tools/n00b-attest.c`) is a
 * thin stdin/stdout/file binding shim that dispatches to the
 * library-shaped verb cores declared here. Each core consumes
 * already-bound in-memory inputs (`n00b_buffer_t *`,
 * `n00b_string_t *`) and returns an already-bound in-memory
 * output via @ref n00b_result_t — the binary's argv parsing
 * (slay/commander) and stdin/stdout/file binding live in the
 * shim, NOT here.
 *
 * Library-API-first (WP-002 plan §727): regression-test-shaped
 * artifacts (the tests under `test/unit/test_attest_cli_sign.c`
 * and `test/unit/test_attest_cli_verify.c`) drive these cores
 * directly via in-memory buffers; the verb cores never read from
 * `stdin`, never write to `stdout`, and never `open()` a file
 * path. Path-binding belongs to the shim.
 *
 * # Scope
 *
 * WP-002 wired the `sign` verb; WP-003 Phase 4 wires the `verify`
 * verb. Other verbs (`inspect`, `push`, `pull`, ...) are
 * registered with commander as not-yet-implemented stubs in the
 * binary so `--help` discovery lists them (DF-004 disposition (a)
 * per user direction 2026-05-18); their library-shaped cores
 * arrive in later WPs.
 *
 * # Allocator discipline
 *
 * Each core accepts `.allocator = nullptr` and threads it into
 * every downstream call (Statement parse, envelope construction,
 * signer resolve, envelope sign, envelope serialize on the sign
 * side; envelope parse, verifier resolve, envelope verify on the
 * verify side). Threading an arena through here keeps every byte
 * the verb produces in that arena (FR-21 / FR-22). Per D-042 part
 * W-2 the signer's / verifier's resolve-time allocator inherits
 * forward into subsequent calls against the resolved handle, so
 * the CLI sees arena-attributed signatures / verify-scratch
 * without re-threading every downstream call site.
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_statement.h>
#include <attest/n00b_attest_dsse.h>
#include <attest/n00b_attest_signer.h>
#include <attest/n00b_attest_verifier.h>
#include <attest/n00b_attest_oci.h>
#include <attest/n00b_attest_mark.h>

/**
 * @brief Run the `sign` verb's library-shaped core: parse a
 *        Statement, wrap it in a DSSE envelope, resolve the
 *        signer, sign, and serialize to envelope JSON bytes.
 *
 * The body is the substrate the `n00b-attest sign` shim sits on.
 * Inputs are already-bound buffers / strings; outputs are an
 * already-bound buffer carrying the canonical-wire envelope JSON
 * (D-024: `n00b_json_encode(.pretty = false)`). The shim handles
 * stdin/stdout/file binding around this core.
 *
 * ## Statement canonicalization
 *
 * The input bytes are parsed as a Statement and re-serialized
 * via libn00b's canonical encoder before being placed in the
 * envelope's payload field. The signature is therefore over the
 * **canonical form**, not the verbatim input. This makes
 * signatures reproducible regardless of input whitespace,
 * member ordering, or other JSON-equivalent variations —
 * matching JOSE / XML-DSIG precedent. Callers who want their
 * pre-computed signature bytes preserved must canonicalize
 * before passing input. This behavior is project policy per
 * D-043 (logged at WP-002 closeout per user direction
 * 2026-05-18).
 *
 * The core composes the high-level building blocks landed in
 * WP-001 + WP-002 Phase 3:
 *   1. @ref n00b_attest_statement_parse — Statement bytes →
 *      Statement handle.
 *   2. Re-serialize the parsed Statement so the envelope's
 *      payload is the canonical (compact, parser-conformant)
 *      form (see "Statement canonicalization" above).
 *   3. @ref n00b_attest_envelope_new + @ref
 *      n00b_attest_envelope_set_payload — wrap.
 *   4. @ref n00b_attest_signer_resolve — load the signer from
 *      the URI. Per D-042 W-2 the signer remembers the
 *      resolve-time allocator; subsequent sign calls inherit it.
 *   5. @ref n00b_attest_envelope_sign — PAE → sign → keyid →
 *      add_signature, all in one call (D-041 high-level
 *      wrapper).
 *   6. @ref n00b_attest_envelope_serialize — render to JSON
 *      bytes (canonical wire form).
 *   7. Release the signer.
 *
 * @param statement_bytes  In-memory Statement JSON bytes
 *                         (typically the stdin contents or the
 *                         contents of `--statement <path>`).
 * @param key_uri          Signer URI (e.g.,
 *                         `file:///etc/n00b-attest/ci.pem`).
 *                         Per FR-SM-1 the file backend is the
 *                         only in-scope scheme in WP-002;
 *                         unknown schemes propagate the resolver's
 *                         @ref N00B_ATTEST_ERR_UNSUPPORTED_SCHEME.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns every byte the verb produces
 *                (intermediate Statement / envelope handles,
 *                signer state, PAE bytes, signature bytes, the
 *                returned envelope JSON buffer).
 *
 * @return `n00b_result_ok(n00b_buffer_t *, envelope_json)` on
 *         success; `n00b_result_err(...)` propagating the first
 *         failing step's error code:
 *         - @ref N00B_ATTEST_ERR_STMT_BAD_INPUT /
 *           @ref N00B_ATTEST_ERR_STMT_BAD_JSON /
 *           @ref N00B_ATTEST_ERR_STMT_WRONG_TYPE /
 *           @ref N00B_ATTEST_ERR_STMT_MISSING_FIELD — Statement
 *           parse / re-serialize failures.
 *         - @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT /
 *           @ref N00B_ATTEST_ERR_DSSE_NO_PAYLOAD — envelope
 *           construction failures.
 *         - @ref N00B_ATTEST_ERR_UNSUPPORTED_SCHEME /
 *           @ref N00B_ATTEST_ERR_KEY_NOT_FOUND /
 *           @ref N00B_ATTEST_ERR_PEM_PARSE_FAILED /
 *           @ref N00B_ATTEST_ERR_DER_PARSE_FAILED /
 *           @ref N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM —
 *           signer-resolve failures.
 *         - @ref N00B_ATTEST_ERR_SIGN_FAILED — sign-path failure.
 *
 * @pre @ref n00b_attest_module_init has been called (the host
 *      tool binary or test must register the in-tree backends
 *      before invoking this core).
 * @post On success the signer handle is released; the returned
 *       buffer is owned by the caller's allocator (or the
 *       runtime default if `.allocator` was null).
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_cli_sign(n00b_buffer_t *statement_bytes,
                     n00b_string_t *key_uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Run the `verify` verb's library-shaped core: parse a DSSE
 *        envelope, resolve the verifier, run the any-passes
 *        verification, release the verifier, and propagate the
 *        verdict.
 *
 * The body is the substrate the `n00b-attest verify` shim sits on.
 * Inputs are already-bound buffers / strings; the output is a
 * verdict-encoding @ref n00b_result_t(bool). The shim handles
 * stdin/file binding around this core; it also translates the
 * return into Phase 4's three-code exit shape (D-044 OQ-1 (b)):
 * `Ok(true)` → exit 0, `Ok(false)` → exit 1, `Err(...)` → exit 2.
 *
 * The core composes the high-level building blocks landed in
 * WP-003 Phase 1 + Phase 2 + Phase 3:
 *   1. @ref n00b_attest_envelope_parse — Envelope JSON bytes →
 *      envelope handle. Phase 1's parser reconstructs
 *      `signatures[]` from the wire JSON (DF-006 closure) so the
 *      subsequent verify call sees the appended `{keyid, sig}`
 *      pairs.
 *   2. @ref n00b_attest_verifier_resolve — load the verifier from
 *      the URI. Per D-042 W-2 the verifier remembers the
 *      resolve-time allocator; subsequent verify calls inherit it.
 *   3. @ref n00b_attest_envelope_verify — sigstore-style
 *      any-matching-keyid-passes verification (D-041 high-level
 *      wrapper, dual of `n00b_attest_envelope_sign`).
 *   4. @ref n00b_attest_verifier_release — symmetric lifetime
 *      cleanup; runs even on early-Err paths (defensive-release
 *      mirror of the signer side).
 *
 * ## Verdict pass-through
 *
 * The core does NOT collapse `Ok(false)` into `Err`: the verify
 * surface deliberately keeps the verdict on the `Ok` channel and
 * the machinery-failure on the `Err` channel (D-044 OQ-1 (b),
 * @ref n00b_attest_envelope_verify). Phase 4's 3-code exit shape
 * depends on this distinction; callers MUST preserve it.
 *
 * @param envelope_bytes  In-memory DSSE envelope JSON bytes
 *                        (typically the stdin contents or the
 *                        contents of `--envelope <path>`).
 * @param key_uri         Verifier URI (e.g.,
 *                        `file:///etc/n00b-attest/ci.pub.pem`).
 *                        Per FR-VM-1 the file backend is the only
 *                        in-scope scheme in WP-003; unknown
 *                        schemes propagate the resolver's
 *                        @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME.
 *
 * @kw allocator  Optional allocator (defaults to the runtime
 *                allocator); owns every byte the verb produces
 *                (parsed envelope handle, verifier state, PAE
 *                bytes, any check-path scratch).
 *
 * @return Verdict-encoding `n00b_result_t(bool)`:
 *         - `n00b_result_ok(bool, true)` — at least one entry in
 *           the envelope's `signatures[]` whose `keyid` matched
 *           the verifier's keyid verified under the verifier's
 *           public key.
 *         - `n00b_result_ok(bool, false)` — no matching-keyid
 *           entry verified (either no entry has a matching keyid,
 *           or every matching-keyid entry returned `Ok(false)`,
 *           or `signatures[]` is empty, or the envelope's payload
 *           bytes are not the bytes that were signed — tampered
 *           payload). **Verdict, NOT Err.**
 *         - `n00b_result_err(...)` propagating the first failing
 *           step's machinery error code:
 *           - @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT for null /
 *             empty `envelope_bytes`.
 *           - @ref N00B_ATTEST_ERR_DSSE_BAD_JSON /
 *             @ref N00B_ATTEST_ERR_DSSE_WRONG_TYPE /
 *             @ref N00B_ATTEST_ERR_DSSE_BAD_BASE64 — envelope-parse
 *             failures.
 *           - @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME /
 *             @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND /
 *             @ref N00B_ATTEST_ERR_VERIFIER_PEM_PARSE_FAILED /
 *             @ref N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED /
 *             @ref N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM
 *             — verifier-resolve failures.
 *           - @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT /
 *             @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH —
 *             check-path machinery failures from the per-entry
 *             dispatch.
 *
 * @pre @ref n00b_attest_module_init has been called (the host
 *      tool binary or test must register the in-tree backends
 *      before invoking this core).
 * @post On every return path (success, verdict-false, or Err) the
 *       internally-resolved verifier handle has been released. The
 *       caller owns nothing produced inside this entry point.
 *
 * @note Defensive release: if `_envelope_parse` succeeds but
 *       `_verifier_resolve` fails, the parsed envelope is
 *       GC-managed and needs no explicit release; the verifier
 *       handle was never constructed. If `_verifier_resolve`
 *       succeeds but `_envelope_verify` returns `Err`, the
 *       verifier IS live and IS released before this entry point
 *       returns — mirror of the signer side's release-on-error
 *       pattern in @ref n00b_attest_cli_sign.
 */
extern n00b_result_t(bool)
n00b_attest_cli_verify(n00b_buffer_t *envelope_bytes,
                       n00b_string_t *key_uri) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Run the `push` verb's library-shaped core: parse an OCI
 *        image reference, resolve OCI auth, construct an OCI
 *        client, derive the signer keyid + predicate type from
 *        the envelope, and push the envelope as a referrer
 *        manifest.
 *
 * The body is the substrate the `n00b-attest push` shim sits on.
 * Inputs are already-bound buffers / strings; the output is the
 * canonical manifest digest (`sha256:<hex>`) the registry stored
 * the referrer under. The shim handles `--envelope <path>` and
 * `--image foo/bar@sha256:...` flag parsing around this core.
 *
 * ## Composition
 *
 * The core composes the high-level building blocks landed in
 * WP-001, WP-002, WP-003, and WP-004 Phase 1:
 *   1. @ref n00b_attest_oci_url_parse (internal) — parse the
 *      `[<registry>/]<name>@sha256:<digest>` shape.
 *   2. @ref n00b_attest_oci_auth_resolve — walk the auth-source
 *      chain (caller, registries.json, anonymous).
 *   3. @ref n00b_attest_oci_client_new — bind the registry origin
 *      + auth + per-request defaults.
 *   4. @ref n00b_attest_envelope_parse + @ref
 *      n00b_attest_envelope_get_signature_keyid — extract the
 *      signer's keyid for the manifest annotation. The annotation
 *      captures `signatures[0].keyid` only (a fast-path filter;
 *      the envelope's `signatures[]` is authoritative for
 *      multi-signer envelopes).
 *   5. @ref n00b_attest_envelope_get_payload + @ref
 *      n00b_attest_statement_parse + @ref
 *      n00b_attest_statement_get_predicate_type — extract the
 *      predicate type URI for the manifest annotation (skipped
 *      when @c predicate_type is supplied by the caller).
 *   6. @ref n00b_attest_oci_push_attestation — orchestrate the
 *      HEAD subject + POST/PUT blob + PUT manifest sequence.
 *
 * ## Defensive release discipline
 *
 * The push verb core holds TWO handles in flight: the OCI client
 * handle + the OCI auth handle. **Both are released on every
 * return path, including Err.** Mirror of the verify-side
 * release-on-error pattern (WP-003 Phase 4 precedent in @ref
 * n00b_attest_cli_verify).
 *
 * @param envelope_bytes  In-memory DSSE envelope JSON bytes
 *                        (typically the stdin contents or the
 *                        contents of `--envelope <path>`).
 *                        Required.
 * @param image_ref       OCI image reference. Either
 *                        `<host>/<name>@sha256:<digest>` (with
 *                        registry prefix) or `<name>@sha256:
 *                        <digest>` (delegating registry to the
 *                        kwarg / default). Tag-form refs are
 *                        currently NOT supported — the subject
 *                        identity must be digest-pinned at push
 *                        time. Required.
 *
 * @kw registry_override  Optional registry hostname override
 *                        (e.g. `r"ghcr.io"`). When set the
 *                        override replaces the registry parsed
 *                        from @p image_ref; when null the parsed
 *                        registry is used. If neither is
 *                        available the verb returns
 *                        @ref N00B_ATTEST_ERR_OCI_BAD_URL.
 * @kw predicate_type     Optional predicate-type URI override. When
 *                        null the verb derives the value from the
 *                        envelope payload's Statement via
 *                        @ref n00b_attest_statement_parse. Supply
 *                        when the caller already has the URI in
 *                        hand and wants to skip the parse step.
 * @kw allocator          Optional allocator (defaults to the
 *                        runtime allocator); owns every byte the
 *                        verb produces.
 *
 * @return `n00b_result_ok(n00b_string_t *, manifest_digest)` on
 *         success — the canonical `sha256:<hex>` digest of the
 *         uploaded artifact manifest, as confirmed by the
 *         registry (or computed locally when the registry omitted
 *         `Docker-Content-Digest`). Err legs:
 *         - @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT — null /
 *           empty `envelope_bytes`, or envelope-walk failed to
 *           extract a signer keyid.
 *         - @ref N00B_ATTEST_ERR_OCI_BAD_URL — null
 *           `image_ref`, malformed image reference (per
 *           `_url_parse`'s structural rules), tag-form ref
 *           without `@sha256:` pinning, or no registry available
 *           after applying the override.
 *         - @ref N00B_ATTEST_ERR_OCI_HTTP_ERROR — transport or
 *           non-success status on any of the HEAD / POST / PUT
 *           sub-steps inside `_push_attestation`.
 *         - @ref N00B_ATTEST_ERR_OCI_BEARER_TOKEN_FAILED — bearer
 *           token-exchange flow failed on any sub-step.
 *         - @ref N00B_ATTEST_ERR_OCI_AUTH_SOURCE_NOT_FOUND — no
 *           auth source yielded credentials when the auth chain
 *           was explicitly restricted to non-anonymous sources.
 *         - @ref N00B_ATTEST_ERR_OCI_MANIFEST_DIGEST_MISMATCH —
 *           registry-reported manifest digest disagreed with the
 *           locally-computed digest (integrity concern).
 *         - Statement / envelope parse errors propagated from
 *           the envelope-walk step (the WP-001 / WP-002
 *           namespaces).
 *
 *         Phase 2 ships bare-code Err legs per D-053; richer Err
 *         payloads are deferred to the libn00b typed-Err-payload
 *         future lift (DF-011).
 *
 * @pre @ref n00b_attest_module_init has been called (the host
 *      tool binary or test must register the in-tree backends
 *      before invoking this core).
 * @post On every return path (success or Err) the internally-
 *       constructed OCI client and auth handles have been
 *       released. The caller owns nothing produced inside this
 *       entry point besides the returned manifest digest.
 */
extern n00b_result_t(n00b_string_t *)
n00b_attest_cli_push(n00b_buffer_t *envelope_bytes,
                     n00b_string_t *image_ref) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_string_t    *predicate_type    = nullptr;
    n00b_allocator_t *allocator         = nullptr;
};

/**
 * @brief Run the `discover` verb's library-shaped core: list
 *        attestations recorded for an OCI image digest.
 *
 * Composes:
 *   1. @ref n00b_attest_oci_url_parse — parse the image ref.
 *   2. @ref n00b_attest_oci_auth_resolve — resolve auth.
 *   3. @ref n00b_attest_oci_client_new — construct client.
 *   4. @ref n00b_attest_oci_list_referrers — fetch referrers, with
 *      optional server-side @c artifact_type filter (D-051 OQ-3
 *      BOTH).
 *
 * # Defensive release discipline
 *
 * Mirror of @ref n00b_attest_cli_push: BOTH the OCI client handle
 * and the OCI auth handle are released on every return path
 * including Err.
 *
 * # Empty referrers list is not an error
 *
 * Per OCI distribution-spec § 4.5, an image with no recorded
 * referrers may return 200 OK + empty `manifests[]`, OR 404. Both
 * shapes surface as `Ok([])` from this entry point; the binary-side
 * shim emits "no attestations found" + exit 0 in either case.
 *
 * @param image_ref  OCI image reference. Either
 *                   `<host>/<name>@sha256:<digest>` (with registry
 *                   prefix) or `<name>@sha256:<digest>` (delegating
 *                   registry to the kwarg). Tag-form refs are NOT
 *                   supported — discover requires digest-pinning.
 *                   Required.
 *
 * @kw registry_override  Optional registry hostname override.
 * @kw artifact_type      Optional server-side artifact-type filter
 *                        (e.g. `r"application/vnd.in-toto+dsse"`).
 *                        When null, no filter is applied at the
 *                        server.
 * @kw allocator          Optional allocator.
 *
 * @return `n00b_result_ok(n00b_list_t(n00b_attest_oci_referrer_t *)
 *         *, list)` on success — the list may be empty. Err legs
 *         route through the OCI / DSSE / Statement domain codes
 *         propagated from sub-step calls (per @ref
 *         n00b_attest_oci_list_referrers).
 *
 * @pre @ref n00b_attest_module_init has been called.
 * @post Both the internally-constructed OCI client and auth handles
 *       have been released. The returned list is owned by the
 *       caller's allocator.
 */
extern n00b_result_t(n00b_list_t(n00b_attest_oci_referrer_t *) *)
n00b_attest_cli_discover(n00b_string_t *image_ref) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_string_t    *artifact_type     = nullptr;
    n00b_allocator_t *allocator         = nullptr;
};

/**
 * @brief Run the `pull` verb's library-shaped core: fetch a DSSE
 *        envelope from an OCI 1.1 registry by predicate-type
 *        narrowing.
 *
 * Composes:
 *   1. @ref n00b_attest_oci_url_parse — parse the image ref.
 *   2. @ref n00b_attest_oci_auth_resolve — resolve auth.
 *   3. @ref n00b_attest_oci_client_new — construct client.
 *   4. @ref n00b_attest_oci_list_referrers with
 *      `.artifact_type = r"application/vnd.in-toto+dsse"` —
 *      server-side narrowing to in-toto+dsse envelopes (D-051 OQ-3
 *      BOTH, server-side half).
 *   5. Client-side post-filter on the referrer's `predicate_type`
 *      annotation (D-051 OQ-3 BOTH, client-side half — defense-in-
 *      depth in case the registry ignores the `?artifactType=`
 *      query).
 *   6. Pick the FIRST matching entry — multi-attestation with the
 *      same predicate-type is allowed but `pull` returns one. (See
 *      flagged_for_orchestrator on the choice point.)
 *   7. @ref n00b_attest_oci_pull_envelope — fetch the envelope
 *      bytes.
 *
 * # Defensive release discipline
 *
 * Mirror of @ref n00b_attest_cli_push: BOTH the OCI client handle
 * and the OCI auth handle are released on every return path
 * including Err.
 *
 * # Pull does NOT verify
 *
 * The returned envelope is byte-equal to what was pushed; no
 * signature check is performed. Callers who want verification
 * chain @ref n00b_attest_cli_verify on the returned bytes.
 *
 * @param image_ref       OCI image reference (must be digest-pinned).
 *                        Required.
 * @param predicate_type  Required predicate-type URI to filter on
 *                        (e.g.
 *                        `r"https://slsa.dev/provenance/v1"`). Per
 *                        the plan's choice "force the flag explicit
 *                        to avoid ambiguity"; null surfaces as
 *                        @ref N00B_ATTEST_ERR_STMT_BAD_INPUT.
 *                        Required.
 *
 * @kw registry_override  Optional registry hostname override.
 * @kw allocator          Optional allocator.
 *
 * @return `n00b_result_ok(n00b_buffer_t *, envelope_bytes)` on
 *         success — byte-identical to the envelope pushed via
 *         @ref n00b_attest_cli_push. Err legs:
 *         - @ref N00B_ATTEST_ERR_OCI_NO_MATCHING_REFERRER — the
 *           post-filter found no entry matching the requested
 *           predicate-type.
 *         - @ref N00B_ATTEST_ERR_OCI_BLOB_DIGEST_MISMATCH — fetched
 *           envelope bytes hash to a different sha256 than the
 *           manifest declared.
 *         - @ref N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE — envelope
 *           exceeded the 1 MiB size cap (NFR-5).
 *         - @ref N00B_ATTEST_ERR_OCI_BAD_REFERRER_INDEX — referrer
 *           manifest shape did not match spec §8.2.
 *         - Other OCI / DSSE / Statement domain codes propagated
 *           from sub-step calls.
 *
 * @pre @ref n00b_attest_module_init has been called.
 * @post Both the internally-constructed OCI client and auth handles
 *       have been released. The returned buffer is owned by the
 *       caller's allocator.
 */
extern n00b_result_t(n00b_buffer_t *)
n00b_attest_cli_pull(n00b_string_t *image_ref,
                     n00b_string_t *predicate_type) _kargs
{
    n00b_string_t    *registry_override = nullptr;
    n00b_allocator_t *allocator         = nullptr;
};

/**
 * @brief Run the `mark` verb's library-shaped core: parse a list of
 *        DSSE envelope wire-byte buffers, dispatch through
 *        @ref n00b_attest_mark_artifact, and return the mark result
 *        carrying the IC-4 unchalked SHA-256.
 *
 * The body is the substrate the `n00b-attest mark` shim sits on.
 * Inputs are already-bound: an artifact path string plus a list of
 * `n00b_buffer_t *` envelope-bytes blobs (each blob is the
 * canonical wire JSON of one DSSE envelope, as produced by
 * @ref n00b_attest_envelope_serialize or @ref n00b_attest_cli_sign).
 * The shim handles `--artifact`, repeated `--envelope <path>` flags
 * (or stdin fallback), `--lazy`, and `--registry-hint` argv binding
 * around this core.
 *
 * ## Composition
 *
 *   1. Walk @p envelope_bytes_list; for each entry call
 *      @ref n00b_attest_envelope_parse to materialize an envelope
 *      handle.
 *   2. Dispatch through @ref n00b_attest_mark_artifact with the
 *      reconstructed envelope list and the caller-supplied kwargs.
 *
 * ## No defensive release ceremony
 *
 * The verb holds no long-lived handles (no OCI client, no signer,
 * no verifier). Standard `n00b_buffer_t *` / `n00b_attest_envelope_t *`
 * lifecycle applies — every byte the verb produces is owned by the
 * caller's allocator.
 *
 * @param artifact_path        Filesystem path to the artifact to
 *                             mark. Required; null/empty surfaces
 *                             as @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT.
 * @param envelope_bytes_list  List of `n00b_buffer_t *` envelope
 *                             wire-byte blobs (each entry is one
 *                             DSSE envelope JSON). The CR-13
 *                             multi-envelope use case (a build
 *                             attestation paired with an SBOM
 *                             attestation) is the primary motivator
 *                             for accepting a list rather than a
 *                             single envelope. Required; null/empty
 *                             surfaces as
 *                             @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT.
 *
 * @kw bundled        Bundled-vs-lazy ATTESTATION JSON shape;
 *                    defaults to `true` (envelopes' wire bytes
 *                    folded into the mark's `envelopes[]` array).
 *                    `false` records only digest + predicate type
 *                    per envelope; the verifier fetches envelopes
 *                    out-of-band (typically via @c registry_hint +
 *                    WP-004's `_pull_envelope`). Forwarded verbatim
 *                    to @ref n00b_attest_mark_artifact.
 * @kw registry_hint  Optional full OCI image reference (e.g.
 *                    `r"ghcr.io/myorg/myrepo:tag"`) recorded in the
 *                    mark's `registry_hint` field. Validated at
 *                    mark time via @c n00b_attest_oci_url_parse;
 *                    invalid refs surface as
 *                    @ref N00B_ATTEST_ERR_CHALK_BAD_REGISTRY_HINT
 *                    BEFORE the artifact bytes are touched.
 *                    Forwarded verbatim to
 *                    @ref n00b_attest_mark_artifact.
 * @kw allocator      Optional allocator (defaults to runtime).
 *                    Threaded through every downstream call.
 *
 * @return `n00b_result_ok(n00b_attest_mark_result_t *, row)` on
 *         success. Err legs propagate the first failing step's
 *         error code:
 *         - @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT — null/empty
 *           @p artifact_path, null/empty @p envelope_bytes_list,
 *           a null entry inside the list, or a null entry's bytes
 *           failed envelope-parse.
 *         - Envelope-parse errors (@ref
 *           N00B_ATTEST_ERR_DSSE_BAD_JSON / @ref
 *           N00B_ATTEST_ERR_DSSE_WRONG_TYPE / @ref
 *           N00B_ATTEST_ERR_DSSE_BAD_BASE64) propagated from
 *           @ref n00b_attest_envelope_parse.
 *         - @c _CHALK_BAD_REGISTRY_HINT / @c _CHALK_BAD_ENVELOPE /
 *           @c _CHALK_INSERT_FAILED propagated from
 *           @ref n00b_attest_mark_artifact.
 *
 * @pre @ref n00b_attest_module_init has been called.
 * @post On Ok, the artifact's bytes have been rewritten in place
 *       (in-band codecs) or a sidecar file has been written
 *       (sidecar codecs). On Err the artifact's bytes are not
 *       touched (validation runs before any byte-mutating op).
 */
extern n00b_result_t(n00b_attest_mark_result_t *)
n00b_attest_cli_mark(n00b_string_t                *artifact_path,
                     n00b_list_t(n00b_buffer_t *) *envelope_bytes_list) _kargs
{
    bool              bundled       = true;
    n00b_string_t    *registry_hint = nullptr;
    n00b_allocator_t *allocator     = nullptr;
};

/**
 * @brief Run the `unmark` verb's library-shaped core: direct
 *        passthrough to @ref n00b_attest_unmark.
 *
 * The body is the substrate the `n00b-attest unmark` shim sits on.
 * Inputs are already-bound; the shim binds the positional
 * `<artifact>` argument around this core.
 *
 * @param artifact_path  Filesystem path to the artifact. Required;
 *                       null/empty surfaces as
 *                       @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT.
 *
 * @kw allocator  Optional allocator (defaults to runtime). Threaded
 *                through libchalk's read/unmark/write dispatch.
 *
 * @return `n00b_result_ok(bool, true)` on success;
 *         `n00b_result_err(bool, ...)` propagating the
 *         @ref n00b_attest_unmark error code on failure.
 *
 * @pre @ref n00b_attest_module_init has been called.
 */
extern n00b_result_t(bool)
n00b_attest_cli_unmark(n00b_string_t *artifact_path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};

/**
 * @brief Run the `extract` verb's library-shaped core: direct
 *        passthrough to @ref n00b_attest_extract_from_artifact.
 *
 * The body is the substrate the `n00b-attest extract` shim sits on.
 * Inputs are already-bound; the shim binds the positional
 * `<artifact>` argument plus `--json` formatting decisions around
 * this core. The IC-5 sentinel discrimination (four Err legs) is
 * preserved unchanged from @ref n00b_attest_extract_from_artifact;
 * the shim maps the four codes onto the §10.1 exit-code split.
 *
 * @param artifact_path  Filesystem path to the artifact. Required;
 *                       null/empty surfaces as
 *                       @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT.
 *
 * @kw allocator  Optional allocator (defaults to runtime). Threaded
 *                through libchalk's read/extract dispatch plus the
 *                ATTESTATION JSON parse + base64 decode.
 *
 * @return `n00b_result_ok(n00b_attest_extract_result_t *, row)` on
 *         success; Err legs propagate the IC-5 sentinels (@ref
 *         N00B_ATTEST_ERR_CHALK_NO_MARK / @ref
 *         N00B_ATTEST_ERR_CHALK_NO_ATTESTATION / @ref
 *         N00B_ATTEST_ERR_CHALK_MALFORMED_ATTESTATION / @ref
 *         N00B_ATTEST_ERR_CHALK_CODEC_LOOKUP_FAILED) plus
 *         @ref N00B_ATTEST_ERR_CHALK_EXTRACT_FAILED and
 *         @ref N00B_ATTEST_ERR_DSSE_BAD_INPUT.
 *
 * @pre @ref n00b_attest_module_init has been called.
 * @post The artifact's bytes are NOT modified.
 */
extern n00b_result_t(n00b_attest_extract_result_t *)
n00b_attest_cli_extract(n00b_string_t *artifact_path) _kargs
{
    n00b_allocator_t *allocator = nullptr;
};
