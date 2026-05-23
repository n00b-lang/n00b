#pragma once

/**
 * @file n00b_attest_error.h
 * @brief n00b_attest module-domain error codes + human-readable
 *        accessor.
 *
 * Houses every `N00B_ATTEST_ERR_*` macro for the module — the
 * Statement, DSSE-envelope, and signer code spaces are all
 * declared here — plus the lookup function @ref n00b_attest_err_str
 * that turns a code into a short human-readable `n00b_string_t *`.
 *
 * # Why a focused error header
 *
 * WP-001 originally defined the Statement / DSSE error codes as
 * file-scope `#define`s inside `statement.c` and `dsse.c`. Phase 3
 * of WP-002 migrates them here so the codes are the source of
 * truth for a single header (D-031 A-1 closure + V-2 fix). The
 * Phase 3 cleanup pass also pulls the signer codes here (the
 * `n00b-code-auditor` W-3 finding) so the full 16-code namespace
 * lives in one place; `n00b_attest_signer.h` retains @c \@ref
 * pointers but no longer declares any codes itself.
 *
 * # Code-space layout
 *
 *   -1001 … -1099  Statement builder / parser
 *   -2001 … -2099  DSSE envelope encode / decode
 *   -4001 … -4099  Signer abstraction
 *   -5001 … -5099  Verifier abstraction
 *   -6001 … -6099  OCI integration (registry client + auth)
 *   -7001 … -7099  Chalk integration (mark / unmark / extract)
 *
 * All codes are negative integers to avoid `errno` collision per
 * the libn00b convention (api-guidelines § 5.1).
 *
 * # Allocator discipline
 *
 * @ref n00b_attest_err_str is a pure lookup — no `_kargs`, no
 * allocator threading. The returned string is a rich-string
 * literal (`r"..."`) with process-lifetime storage; callers must
 * not free it.
 */

#include <n00b.h>
#include "adt/result.h"

/**
 * @brief Module-domain error: a Statement builder/parser entry
 *        point received a null or otherwise structurally-invalid
 *        argument.
 *
 * @see n00b_attest_statement_new
 * @see n00b_attest_statement_add_subject
 */
#define N00B_ATTEST_ERR_STMT_BAD_INPUT     (-1001)

/**
 * @brief Module-domain error: a Statement document is missing a
 *        required field (subject, predicateType, predicate).
 */
#define N00B_ATTEST_ERR_STMT_MISSING_FIELD (-1002)

/**
 * @brief Module-domain error: a Statement byte stream did not
 *        parse as JSON, or its embedded predicate did not.
 */
#define N00B_ATTEST_ERR_STMT_BAD_JSON      (-1003)

/**
 * @brief Module-domain error: a parsed Statement carries a
 *        `_type` value other than `https://in-toto.io/Statement/v1`.
 */
#define N00B_ATTEST_ERR_STMT_WRONG_TYPE    (-1004)

/**
 * @brief Module-domain error: a DSSE envelope entry point
 *        received a null or empty argument.
 */
#define N00B_ATTEST_ERR_DSSE_BAD_INPUT     (-2001)

/**
 * @brief Module-domain error: a DSSE envelope operation that
 *        requires a payload was invoked before the payload was
 *        attached.
 */
#define N00B_ATTEST_ERR_DSSE_NO_PAYLOAD    (-2002)

/**
 * @brief Module-domain error: a DSSE envelope byte stream did
 *        not parse as JSON.
 */
#define N00B_ATTEST_ERR_DSSE_BAD_JSON      (-2003)

/**
 * @brief Module-domain error: a DSSE envelope carries a
 *        `payloadType` other than `application/vnd.in-toto+json`.
 */
#define N00B_ATTEST_ERR_DSSE_WRONG_TYPE    (-2004)

/**
 * @brief Module-domain error: a DSSE envelope's payload field
 *        did not decode as base64.
 */
#define N00B_ATTEST_ERR_DSSE_BAD_BASE64    (-2005)

/**
 * @brief Module-domain error: the resolver's URI scheme is not
 *        served by any registered backend.
 *
 * Surfaces on @ref n00b_attest_signer_resolve when the `ref`
 * URI's scheme (`file`, `keychain`, `op`, etc.) does not match
 * any backend registered with the resolver. WP-002 ships only
 * the `file://` backend; later WPs lift this to keychain / op /
 * vault / cloud-secret-manager schemes.
 */
#define N00B_ATTEST_ERR_UNSUPPORTED_SCHEME    (-4001)

/**
 * @brief Module-domain error: a backend could not find the
 *        referenced key.
 *
 * For the file backend this surfaces when the file path resolved
 * from the URI does not exist or is not readable. Later backends
 * (keychain, vault, cloud secret manager) reuse this code for
 * "key not present" outcomes.
 */
#define N00B_ATTEST_ERR_KEY_NOT_FOUND         (-4002)

/**
 * @brief Module-domain error: a PEM container failed to parse.
 *
 * Surfaces when the file backend reads a PKCS#8 PEM file whose
 * armor lines are missing or malformed, or whose base64 body is
 * not decodable. Distinct from @ref N00B_ATTEST_ERR_DER_PARSE_FAILED
 * (which surfaces after PEM decoding when the inner DER does not
 * walk to a well-formed PrivateKeyInfo).
 */
#define N00B_ATTEST_ERR_PEM_PARSE_FAILED      (-4003)

/**
 * @brief Module-domain error: a DER-encoded structure failed to
 *        parse against the expected shape.
 *
 * Surfaces from the file backend's RFC 8410 walk when the inner
 * PrivateKeyInfo DER does not match the expected
 * `SEQUENCE { INTEGER, SEQUENCE { OID }, OCTET STRING { OCTET STRING } }`
 * shape or when the inner OCTET STRING length is not the expected
 * 32 bytes for Ed25519.
 */
#define N00B_ATTEST_ERR_DER_PARSE_FAILED      (-4004)

/**
 * @brief Module-domain error: the loaded key's algorithm is not
 *        supported by the resolver.
 *
 * The signer abstraction is algorithm-agnostic at the public
 * surface (D-016) but each backend's load path identifies the
 * concrete algorithm at parse time. The WP-002 file backend
 * supports only id-Ed25519 (OID `1.3.101.112`); any other
 * AlgorithmIdentifier surfaces this error. A later WP adds
 * ECDSA P-256 (id-ecPublicKey + secp256r1) without breaking the
 * surface.
 */
#define N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM (-4005)

/**
 * @brief Module-domain error: a signing primitive failed.
 *
 * Reserved for Phase 3 sign-path failures; declared in Phase 2
 * so the resolver edge can route the stub backend's
 * `NOT_IMPLEMENTED` outcome through a stable error namespace.
 */
#define N00B_ATTEST_ERR_SIGN_FAILED           (-4006)

/**
 * @brief Module-domain error: a code path that is declared in
 *        the public header but whose body lands in a later phase
 *        was reached.
 *
 * In Phase 2 the file backend's `sign` vtable entry is a stub
 * that returns this error; Phase 3's sign-body lift removes the
 * stub. The error code remains in the namespace for any future
 * declared-before-implemented surface (e.g., the verifier WP's
 * not-yet-wired pubkey-resolver entry points).
 */
#define N00B_ATTEST_ERR_NOT_IMPLEMENTED       (-4007)

// ===========================================================================
// Verifier abstraction (-5001 … -5099).
//
// Established by D-044 OQ-2 as the verifier-domain namespace; the
// first five codes land in WP-003 Phase 2 per D-046 (which
// supersedes D-044 OQ-2's original Phase 3 phase assignment).
// Phase 3 of WP-003 adds the runtime-check-path codes
// @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH (-5006) and
// @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT (-5007) under the
// `_VERIFY_*` prefix (D-047 W-1); see the "Verifier check-path
// errors" sub-section header for the prefix-split rationale.
// ===========================================================================

/**
 * @brief Module-domain error: the verifier resolver's URI scheme
 *        is not served by any registered verifier backend.
 *
 * Surfaces on @ref n00b_attest_verifier_resolve when the `ref`
 * URI's scheme (`file`, `keychain`, `oci`, etc.) does not match
 * any verifier backend registered with the resolver. WP-003
 * ships only the `file://` verifier backend; later WPs lift this
 * to additional schemes. Symmetric with @ref
 * N00B_ATTEST_ERR_UNSUPPORTED_SCHEME on the signer side.
 */
#define N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME    (-5001)

/**
 * @brief Module-domain error: a verifier backend could not find
 *        the referenced public key.
 *
 * For the file verifier backend this surfaces when the file path
 * resolved from the URI does not exist or is not readable.
 *
 * @note Phase 2 of WP-003 used this code as a placeholder for
 * three machinery-failure paths on the check / registration
 * edges. Phase 3 (D-047 W-1) migrated all three callsites to the
 * dedicated @ref N00B_ATTEST_ERR_VERIFY_BAD_INPUT (-5007) and
 * @ref N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH (-5006) codes, so
 * post-Phase-3 this code only surfaces on the resolver edge for
 * its original intent ("backend looked, key isn't there").
 */
#define N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND         (-5002)

/**
 * @brief Module-domain error: a verifier-backend PEM container
 *        failed to parse.
 *
 * Surfaces when the file verifier backend reads an SPKI PEM
 * whose armor lines are missing or carry the wrong label
 * (D-044 OQ-3: strict `-----BEGIN PUBLIC KEY-----` only), or
 * whose base64 body is not decodable. Distinct from @ref
 * N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED (which surfaces
 * after PEM decoding when the inner DER does not walk to a
 * well-formed SubjectPublicKeyInfo). Symmetric with @ref
 * N00B_ATTEST_ERR_PEM_PARSE_FAILED on the signer side.
 */
#define N00B_ATTEST_ERR_VERIFIER_PEM_PARSE_FAILED      (-5003)

/**
 * @brief Module-domain error: a verifier-backend DER-encoded
 *        structure failed to parse against the expected shape.
 *
 * Surfaces from the file verifier backend's SPKI walk when the
 * inner SubjectPublicKeyInfo DER does not match the expected
 * `SEQUENCE { SEQUENCE { OID }, BIT STRING }` shape, or when
 * the BIT STRING's unused-bits indicator is non-zero, or when
 * the BIT STRING content length is not the expected 33 bytes
 * for an Ed25519 SPKI. Symmetric with @ref
 * N00B_ATTEST_ERR_DER_PARSE_FAILED on the signer side.
 */
#define N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED      (-5004)

/**
 * @brief Module-domain error: the loaded public key's algorithm
 *        is not supported by the resolver.
 *
 * The verifier abstraction is algorithm-agnostic at the public
 * surface (D-016) but each backend's load path identifies the
 * concrete algorithm at parse time. The WP-003 file verifier
 * backend supports only id-Ed25519 (OID `1.3.101.112`); any
 * other AlgorithmIdentifier surfaces this error. A later WP
 * adds ECDSA P-256 (id-ecPublicKey + secp256r1) without
 * breaking the surface. Symmetric with @ref
 * N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM on the signer side.
 */
#define N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM (-5005)

// ===========================================================================
// Verifier check-path errors (-5006 … -5099).
//
// **Prefix split (D-047).** The -5001..-5005 codes above use the
// `_VERIFIER_*` prefix and tag the resolver / load-path errors —
// abstraction-layer concerns surfaced at @ref
// n00b_attest_verifier_resolve time (URI scheme, file-not-found,
// PEM container parse, SPKI DER parse, AlgorithmIdentifier). The
// -5006/-5007 codes below use the `_VERIFY_*` prefix and tag the
// runtime check-path errors — action-layer concerns surfaced
// during @ref n00b_attest_verifier_check (Ed25519 signature length
// not 64 bytes, null pointer inputs, registration-list capacity
// exceeded).
//
// This split mirrors the existing namespace split between
// -1001..-1004 (`_STMT_*`) and -2001..-2005 (`_DSSE_*`): the prefix
// reflects which subsystem-action emits the code, so
// `n00b_attest_err_str` output and audit logs preserve the
// resolver-vs-check distinction without callers having to consult
// the numeric range.
//
// Phase 3 of WP-003 introduces both codes plus migrates the three
// placeholder callsites that Phase 2 left holding @ref
// N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND per D-046 (`Phase 3 adds
// any additional codes it needs`).
// ===========================================================================

/**
 * @brief Module-domain error: a signature buffer passed to the
 *        verifier check path is not exactly 64 bytes (the
 *        Ed25519 signature length).
 *
 * @details Surfaces from the file verifier backend's check path
 * (@ref n00b_attest_verifier_check) when the `sig` buffer's
 * `byte_len` is not exactly 64. The verify routine requires a
 * fixed-size input; a malformed-length sig is a machinery failure
 * (the caller handed the verifier a sig buffer that the algorithm
 * cannot consume), NOT a verdict — collapsing this into
 * @c Ok(false) would conflate "machinery cannot process the sig"
 * with "machinery processed the sig and reached a definitive no"
 * and break Phase 4's 3-code exit shape (D-044 OQ-1).
 *
 * A later WP that introduces ECDSA P-256 may extend this code's
 * semantics to cover both Ed25519 (64-byte) and ECDSA-P-256
 * (typically 64-byte raw or 70-72-byte DER) sig-length failures,
 * since the backend identifies the expected length from the
 * loaded key's algorithm. The code's name therefore intentionally
 * does NOT bake in the Ed25519 length — it documents the
 * "signature buffer length does not match the algorithm's
 * expected length" failure shape, which is algorithm-parametric.
 */
#define N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH          (-5006)

/**
 * @brief Module-domain error: a verifier check or registration
 *        entry point received a null pointer input or hit the
 *        registration-list capacity ceiling.
 *
 * @details A single code covers two distinct machinery-failure
 * conditions on the verifier subsystem's runtime-action surface,
 * enumerated here because §10.3 requires the full set of
 * triggering conditions to be in the doxygen:
 *
 *   - **Null pointer inputs to @ref n00b_attest_verifier_check.**
 *     A null `verifier`, `bytes`, or `sig` argument to the file
 *     verifier backend's `check` vtable entry surfaces this code.
 *     Phase 2 used @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND as
 *     a placeholder; Phase 3 (D-047 W-1) migrated the callsite to
 *     this dedicated code so audit logs can distinguish
 *     resolver-edge "key not found" from check-edge "caller
 *     handed me null."
 *
 *   - **Registration-list capacity exceeded in @ref
 *     n00b_attest_register_verifier_backend.** When the resolver's
 *     fixed-capacity registration list
 *     (`N00B_ATTEST_MAX_REGISTERED_VERIFIER_BACKENDS`) is full and
 *     another backend is registered, this code surfaces. Phase 2
 *     used @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND as a
 *     placeholder; Phase 3 migrated the callsite to this code.
 *
 * Both conditions are caller / wiring errors (the runtime can do
 * nothing useful in response) rather than verdicts, so routing
 * through @c Err is correct per D-044 OQ-1.
 *
 * A null `backend` or null `backend->scheme` argument to @ref
 * n00b_attest_register_verifier_backend also surfaces this code
 * (it falls under the "null pointer input" condition above) — the
 * registration entry point and the check entry point share both
 * the prefix (`_VERIFY_*` = runtime action) and the code value
 * because each is a runtime-action input-validation failure.
 */
#define N00B_ATTEST_ERR_VERIFY_BAD_INPUT               (-5007)

// ===========================================================================
// OCI integration (-6001 … -6099).
//
// Established by D-051 OQ-6 as the OCI-domain namespace; the
// `-3001..` block was intentionally skipped to maintain numeric
// spacing between subsystem ranges. The first four codes land in
// WP-004 Phase 1 (this WP's first implementation phase). WP-004
// Phase 2 reclaims -6002 as `_OCI_MANIFEST_DIGEST_MISMATCH` (per
// D-046's phase-introduces-codes-when-it-uses-them rule + the
// Phase 1 W-2 retirement of `_OCI_TLS_HANDSHAKE` at -6002 which
// left the slot open). The network-timeout distinction (-6006)
// remains reserved-but-not-declared until a verb shim needs it.
// The OCI client is algorithm-agnostic by D-016, so no
// algorithm-tag leakage appears in the code names.
// ===========================================================================

/**
 * @brief Module-domain error: a constructed registry URL or parsed
 *        image reference is malformed.
 *
 * @details Surfaces from @ref n00b_attest_oci_client_new when the
 * registry URL is null, empty, or does not start with `https://`
 * (the OCI client enforces HTTPS — plain HTTP at the underlying
 * libn00b layer is rejected). Also surfaces from
 * `n00b_attest_oci_url_parse` (internal helper) when the input
 * reference is null, empty, missing the required digest-or-tag
 * suffix, has an empty name, or is otherwise structurally invalid
 * per OCI distribution-spec § 4.1. Phase 2's push verb composes
 * on top of `n00b_attest_oci_url_parse`; the same code surfaces
 * from the push edge when the user-supplied `--image` ref does
 * not parse.
 */
#define N00B_ATTEST_ERR_OCI_BAD_URL                    (-6001)

/**
 * @brief Module-domain error: the registry's claimed manifest
 *        digest disagrees with the locally-computed digest.
 *
 * @details Surfaces from
 * @ref n00b_attest_oci_manifest_upload when the OCI registry's
 * `Docker-Content-Digest` response header is present AND its value
 * does not byte-equal the SHA-256 the client computed locally over
 * the same manifest bytes. This is a real integrity concern — the
 * registry transformed the manifest in transit (whitespace
 * normalization, key reordering, codec round-trip, or worse) — and
 * surfaces as a distinct code so callers can distinguish it from
 * generic transport / HTTP-layer failures.
 *
 * When the registry omits the `Docker-Content-Digest` header
 * entirely (non-strict mode) the client falls back to the
 * locally-computed digest without surfacing this code; the cross-
 * check fires only when both digests are present and disagree.
 *
 * Phase 2 of WP-004 introduces this code (D-046 — phase introduces
 * codes when it uses them). Slotted at -6002 (one of the two slots
 * vacated by D-053 W-2's retirement of the unused Phase-1 codes
 * `_OCI_TLS_HANDSHAKE` and `_OCI_NETWORK_TIMEOUT`).
 */
#define N00B_ATTEST_ERR_OCI_MANIFEST_DIGEST_MISMATCH   (-6002)

/**
 * @brief Module-domain error: an HTTP-layer failure during an OCI
 *        request that prevented a parseable response.
 *
 * @details Surfaces from @ref n00b_attest_oci_request when the
 * underlying libn00b HTTPS dispatcher returned a transport-level
 * error (no parseable HTTP response) or surfaced a transport
 * failure via a status-0 response. The OCI substrate ships a bare
 * error code at this WP — HTTP status code and response body are
 * NOT carried in the result type itself. DF-011 tracks the libn00b
 * typed-Err-payload future lift that will enable richer Err
 * context. Verb shims that need specific user-facing diagnostics
 * (4xx / 5xx status code, registry-side response body, headers)
 * inspect the libn00b @c n00b_http_response_t handle directly via
 * per-call state.
 *
 * At the substrate layer @ref n00b_attest_oci_request returns the
 * response on the Ok channel even for 4xx / 5xx; this code surfaces
 * only when no parseable response was obtained (or, in Phase 2+
 * verb shims, when a higher-level shim has decided the status is
 * unrecoverable at its scope and explicitly maps to this code).
 */
#define N00B_ATTEST_ERR_OCI_HTTP_ERROR                 (-6003)

/**
 * @brief Module-domain error: the OCI distribution-spec § 5
 *        bearer-token-exchange flow failed.
 *
 * @details Surfaces from `n00b_attest_oci_request` when:
 *
 *   - The original request received a 401 with a
 *     `WWW-Authenticate: Bearer` challenge.
 *   - The request helper performed a second un-authenticated GET
 *     against the realm URL (passing `service` + `scope` query
 *     params).
 *   - That second request returned 401, OR
 *   - That second request returned 200 but the JSON body did NOT
 *     parse, OR
 *   - The parsed JSON did NOT carry a `"token"` or `"access_token"`
 *     field of string type, OR
 *   - The retry of the original request with the obtained token
 *     ALSO returned 401.
 *
 * All four sub-conditions surface through this single code; the
 * `WWW-Authenticate` parse failure (challenge header missing
 * `realm=`) routes through this code as well — a registry that
 * sent a 401 without a parseable challenge has effectively
 * declined token-exchange.
 */
#define N00B_ATTEST_ERR_OCI_BEARER_TOKEN_FAILED        (-6004)

/**
 * @brief Module-domain error: no auth source yielded usable
 *        credentials.
 *
 * @details Surfaces from @ref n00b_attest_oci_auth_resolve under
 * two conditions:
 *
 *   - The caller restricted the source chain to a subset that
 *     did not include @ref N00B_ATTEST_OCI_AUTH_ANONYMOUS, and
 *     none of the listed sources produced a credential. (The
 *     default chain always terminates in @c ANONYMOUS, so this
 *     condition only surfaces when the caller passed an explicit
 *     non-trivial `.sources` list.)
 *
 *   - The caller listed one of the future-WP sources
 *     (@ref N00B_ATTEST_OCI_AUTH_CRED_HELPER or @ref
 *     N00B_ATTEST_OCI_AUTH_KEYCHAIN) and no earlier source in the
 *     chain produced a credential. Both future sources are declared
 *     NOW for forward-compat per D-051 OQ-1 but their resolver
 *     bodies surface this code until the Tier-2 follow-on WP
 *     ships them.
 *
 * Malformed `registries.json` (the @ref
 * N00B_ATTEST_OCI_AUTH_REGISTRIES_JSON source) ALSO routes
 * through this code rather than a dedicated BAD_JSON code: the
 * user's broader auth resolution may still succeed via another
 * source (anonymous fallback), so the resolver treats malformed
 * JSON as "this source did not yield credentials" rather than a
 * fatal parse failure.
 */
#define N00B_ATTEST_ERR_OCI_AUTH_SOURCE_NOT_FOUND      (-6005)

/**
 * @brief Module-domain error: no referrer in the OCI referrers index
 *        matched the caller's predicate-type filter.
 *
 * @details Surfaces from @ref n00b_attest_cli_pull when the
 * server-side `?artifactType=` query narrowed the referrers list to
 * the in-toto+dsse subset, and the verb-core's client-side post-
 * filter on the caller-supplied @c predicate_type rejected every
 * remaining entry. The empty-after-filter outcome is treated as Err
 * (not Ok with a null envelope) because pull's semantic is "return
 * the envelope" — no envelope means the verb cannot satisfy the
 * request. Discover (which has no predicate-type narrowing) treats
 * an empty referrers list as @c Ok([]).
 *
 * WP-004 Phase 3 introduces this code (D-046 — phase introduces
 * codes when it uses them). Slotted at -6006 (the slot vacated by
 * Phase 1 W-2's retirement of `_OCI_NETWORK_TIMEOUT`).
 */
#define N00B_ATTEST_ERR_OCI_NO_MATCHING_REFERRER       (-6006)

/**
 * @brief Module-domain error: a fetched blob exceeded the caller's
 *        size cap.
 *
 * @details Surfaces from @ref n00b_attest_oci_pull_envelope when the
 * envelope blob's body bytes exceed the configured `max_size` cap
 * (default 1 MiB per NFR-5; typical envelopes are <= 50 KB per
 * NFR-6, so the default is comfortable). The cap exists to bound
 * the discover/pull-side memory budget against malicious or
 * misconfigured referrer manifests pointing at outsized blobs.
 *
 * WP-004 Phase 3 introduces this code (D-046).
 */
#define N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE             (-6007)

/**
 * @brief Module-domain error: a fetched blob's locally-computed
 *        SHA-256 disagrees with the digest the caller requested.
 *
 * @details Surfaces from @ref n00b_attest_oci_pull_envelope when the
 * envelope blob's actual bytes hash to a different `sha256:<hex>`
 * than the digest passed in (or learned from the manifest's
 * `layers[0].digest`). Symmetric to push's
 * @ref N00B_ATTEST_ERR_OCI_MANIFEST_DIGEST_MISMATCH; together they
 * bracket the registry-round-trip integrity invariant.
 *
 * WP-004 Phase 3 introduces this code (D-046).
 */
#define N00B_ATTEST_ERR_OCI_BLOB_DIGEST_MISMATCH       (-6008)

/**
 * @brief Module-domain error: a referrer manifest or referrers
 *        index could not be interpreted as the expected OCI shape.
 *
 * @details Surfaces from @ref n00b_attest_oci_list_referrers when
 * the server returned 200 OK but the response body is not a
 * well-formed OCI image index (missing `manifests[]` array,
 * malformed JSON, etc.), and from @ref n00b_attest_oci_pull_envelope
 * when the referrer manifest's `layers[]` does not match the
 * spec §8.2 single-in-toto+json-layer shape.
 *
 * Distinct from @ref N00B_ATTEST_ERR_OCI_HTTP_ERROR because the
 * registry's transport layer behaved correctly — the failure mode
 * is application-level shape mismatch; this distinction makes it
 * easier to discriminate "registry is broken" from "we don't know
 * how to read this registry's index format" during audit.
 *
 * WP-004 Phase 3 introduces this code (D-046).
 */
#define N00B_ATTEST_ERR_OCI_BAD_REFERRER_INDEX         (-6009)

/**
 * @brief Module-domain error: a registry response body exceeded
 *        the per-call size cap.
 *
 * @details Surfaces from @ref n00b_attest_oci_list_referrers when
 * one page of the referrers-index pagination response exceeds the
 * Phase-4 hardening cap of 1 MiB per page (NFR-5). The cap exists
 * to bound the discover/pull-side memory budget against malicious
 * or misconfigured registries that emit oversized referrers
 * pages.
 *
 * Distinct from @ref N00B_ATTEST_ERR_OCI_BLOB_TOO_LARGE which is
 * emitted by the symmetric blob / manifest fetch path; the two
 * codes bracket the response-shape vs blob-content size-cap
 * surfaces respectively.
 *
 * WP-004 Phase 4 introduces this code (D-046 — phase introduces
 * codes when it uses them). The n00b-attest-side enforcement
 * mirrors the Phase-3 `generic_fetch` precedent because libn00b's
 * @ref n00b_http_request_sync does NOT (yet) carry a per-call
 * `max_body_size` kwarg; lifting the enforcement into libn00b is
 * tracked as DF-014.
 */
#define N00B_ATTEST_ERR_OCI_RESPONSE_TOO_LARGE         (-6010)

// ===========================================================================
// Chalk integration (-7001 … -7099).
//
// Established by WP-005 Phase 1 as the chalk-integration namespace
// for the `n00b_attest_mark_artifact` / `_unmark` /
// `_extract_from_artifact` library surface. The block carries:
//
//   - `_CHALK_BAD_REGISTRY_HINT` (-7001): the caller-supplied
//     `registry_hint` failed `n00b_attest_oci_url_parse`.
//   - IC-5 sentinel quartet (-7002 .. -7005): `_CHALK_NO_MARK`,
//     `_CHALK_NO_ATTESTATION`, `_CHALK_MALFORMED_ATTESTATION`,
//     `_CHALK_CODEC_LOOKUP_FAILED`. Per the IC-5 spec
//     (`docs/attest/01-requirements.md`) the four cases are
//     reported as four distinct Err codes rather than a
//     discriminated Ok-payload enum: the four cases all carry no
//     useful Ok value, so the Err shape is semantically correct
//     (and mirrors the `n00b_result_t(T) → Err(err_t)` precedent
//     across the rest of the n00b_attest module).
//   - libchalk-dispatch passthrough Err codes (-7006 .. -7008):
//     `_CHALK_INSERT_FAILED`, `_CHALK_EXTRACT_FAILED`,
//     `_CHALK_DELETE_FAILED` — surfaced when libchalk's
//     `n00b_chalk_insert_file` / `_extract_file` / `_delete_file`
//     return an Err for a reason that is NOT IC-5-(i..iv).
//   - `_CHALK_BAD_ENVELOPE` (-7009): an input envelope in the
//     caller-supplied `envelopes[]` list is malformed (failed
//     `n00b_attest_envelope_get_payload` or
//     `n00b_attest_statement_get_predicate_type`).
// ===========================================================================

/**
 * @brief Module-domain error: the caller-supplied @c registry_hint
 *        kwarg on @ref n00b_attest_mark_artifact failed to parse
 *        as an OCI image reference.
 *
 * @details Surfaces from @ref n00b_attest_mark_artifact when the
 * `.registry_hint` kwarg is non-null AND
 * `n00b_attest_oci_url_parse` returns an Err leg against the same
 * bytes. The mark is NOT inserted in this case — validation runs
 * BEFORE any libchalk dispatch so a malformed hint cannot land in
 * a binary's ATTESTATION JSON.
 *
 * Per WP-005 Phase 1 disposed scope the validated value is the
 * FULL image reference (e.g. `ghcr.io/myorg/myrepo:tag` or the
 * digest-pinned form). Hints that omit both digest and tag are
 * rejected (`n00b_attest_oci_url_parse` enforces explicit
 * pinning).
 */
#define N00B_ATTEST_ERR_CHALK_BAD_REGISTRY_HINT        (-7001)

/**
 * @brief Module-domain error: IC-5 case (i) — the artifact carries
 *        no chalk mark.
 *
 * @details Surfaces from @ref n00b_attest_extract_from_artifact
 * when the artifact's codec dispatch succeeded but
 * `n00b_chalk_extract_file` reports that no chalk mark is present
 * in the artifact bytes (e.g., an ELF binary that was never
 * marked, or a Mach-O whose `LC_NOTE` does not carry a chalk
 * owner). Per IC-5 in `docs/attest/01-requirements.md` this is
 * one of the four "no usable attestation" sentinels callers must
 * be able to distinguish.
 *
 * @note `_unmark` is also a possible producer in principle, but
 * the current `_unmark` body uses libchalk's `_delete_file` whose
 * Err code shape does not currently distinguish "no mark to
 * delete" from "delete failed for some other reason"; the unmark
 * path routes through @ref N00B_ATTEST_ERR_CHALK_DELETE_FAILED
 * instead. A future libchalk lift may split the unmark surface.
 */
#define N00B_ATTEST_ERR_CHALK_NO_MARK                  (-7002)

/**
 * @brief Module-domain error: IC-5 case (ii) — the artifact carries
 *        a chalk mark but the mark has no `ATTESTATION` field.
 *
 * @details Surfaces from @ref n00b_attest_extract_from_artifact
 * when `n00b_chalk_extract_file` returned a mark dict but the
 * dict does not contain the `ATTESTATION` key. Per the chalk
 * mark contract (`include/chalk/n00b_chalk_mark.h`) the
 * `ATTESTATION` key is optional; a marked binary that was chalked
 * by libchalk without any attest data attached produces this
 * sentinel.
 */
#define N00B_ATTEST_ERR_CHALK_NO_ATTESTATION           (-7003)

/**
 * @brief Module-domain error: IC-5 case (iii) — the artifact's
 *        mark has an `ATTESTATION` field but its content is not
 *        a well-formed ATTESTATION JSON tree.
 *
 * @details Surfaces from @ref n00b_attest_extract_from_artifact
 * when the mark's `ATTESTATION` slot is present but its contents
 * cannot be interpreted as the canonical ATTESTATION JSON shape
 * documented in `docs/attest/04-in-container-identity.md` §1.
 * Concretely, the slot is malformed when:
 *
 *   - The slot's JSON node is not an object.
 *   - A required field is missing or has the wrong JSON type
 *     (e.g., `envelope_digest` is not a string, `predicate_types`
 *     is not an array of strings).
 *   - The `envelopes[]` array (bundled mode) contains an entry
 *     whose `envelope_base64` does not base64-decode, or whose
 *     decoded bytes do not parse as a DSSE envelope.
 *
 * The IC-5 mapping deliberately separates "structurally malformed"
 * (this code) from "no ATTESTATION at all" (@ref
 * N00B_ATTEST_ERR_CHALK_NO_ATTESTATION) so consumers can
 * distinguish "the producer never attached attestation data" from
 * "the producer attached something, but it isn't valid."
 */
#define N00B_ATTEST_ERR_CHALK_MALFORMED_ATTESTATION    (-7004)

/**
 * @brief Module-domain error: IC-5 case (iv) — the artifact's
 *        bytes do not match any libchalk codec.
 *
 * @details Surfaces from @ref n00b_attest_extract_from_artifact
 * when `n00b_chalk_detect_file` returns
 * @c N00B_CHALK_CODEC_NONE. The path's content (read by libchalk)
 * does not match any of ELF / Mach-O / PE / GGUF / SafeTensors /
 * ZIP / PyC / source / sidecar.
 *
 * @note This is a machinery condition — the caller handed the
 * library bytes the library cannot reason about — distinct from
 * the verdict-shaped IC-5 cases (i..iii) where the library DID
 * understand the bytes but could not extract usable attestation.
 * The Phase-2 CLI verb shim (`extract`) maps this to exit code 2
 * (machinery failure) while (i..iii) collapse to exit 1
 * (no-verdict).
 */
#define N00B_ATTEST_ERR_CHALK_CODEC_LOOKUP_FAILED      (-7005)

/**
 * @brief Module-domain error: libchalk's `n00b_chalk_insert_file`
 *        returned an Err leg during @ref n00b_attest_mark_artifact.
 *
 * @details Surfaces from @ref n00b_attest_mark_artifact when the
 * pre-flight steps (registry_hint validation, ATTESTATION JSON
 * build, `n00b_chalk_mark_new`, `_mark_set_attestation`) all
 * succeeded but `n00b_chalk_insert_file` itself returned an Err
 * leg. Phase 1 ships a bare-code passthrough — the libchalk Err
 * code shape (`1` = read failed, `2` = codec-insert failed, `3` =
 * write failed per `n00b_chalk_file_insert_via` in
 * `src/chalk/file_io.c`) is opaque at this layer; richer
 * diagnostic distinction would require a libchalk public-Err lift
 * outside Phase 1's scope.
 */
#define N00B_ATTEST_ERR_CHALK_INSERT_FAILED            (-7006)

/**
 * @brief Module-domain error: libchalk's `n00b_chalk_extract_file`
 *        returned an Err leg for a reason that is NOT one of the
 *        IC-5 sentinel cases (i..iv).
 *
 * @details Surfaces from @ref n00b_attest_extract_from_artifact
 * when `n00b_chalk_extract_file` returned an Err leg AFTER the
 * codec was successfully detected (i.e., `_CHALK_CODEC_LOOKUP_
 * FAILED` would not apply). The libchalk Err shape today is
 * opaque (numeric codec-specific code per
 * `src/chalk/file_io.c:1`); the n00b-attest wrapper maps "no
 * chalk section / no mark found" to @ref
 * N00B_ATTEST_ERR_CHALK_NO_MARK (IC-5 (i)) and everything else
 * to this code. A future libchalk public-Err lift could split
 * this further (e.g., parse errors vs file-read errors).
 */
#define N00B_ATTEST_ERR_CHALK_EXTRACT_FAILED           (-7007)

/**
 * @brief Module-domain error: libchalk's `n00b_chalk_delete_file`
 *        returned an Err leg during @ref n00b_attest_unmark.
 *
 * @details Surfaces from @ref n00b_attest_unmark when libchalk's
 * `n00b_chalk_delete_file` returned an Err leg. As with `_INSERT_
 * FAILED` and `_EXTRACT_FAILED`, Phase 1 ships a bare-code
 * passthrough — the libchalk Err codes are not currently
 * differentiated at this layer.
 *
 * @note The "no mark to delete" condition also routes through
 * this code rather than `_CHALK_NO_MARK`. Splitting that out
 * requires a libchalk lift that exposes a distinct "nothing to
 * delete" Err code on the delete path.
 */
#define N00B_ATTEST_ERR_CHALK_DELETE_FAILED            (-7008)

/**
 * @brief Module-domain error: one of the input envelopes in the
 *        @c envelopes positional arg of @ref
 *        n00b_attest_mark_artifact is malformed.
 *
 * @details Surfaces from @ref n00b_attest_mark_artifact when the
 * ATTESTATION JSON builder cannot extract the required per-
 * envelope fields. Concretely, the builder needs each envelope's:
 *
 *   - Payload bytes (`n00b_attest_envelope_get_payload`) — used
 *     to parse the Statement and to compute the
 *     `envelope_digest` of envelope[0].
 *   - Statement predicate type
 *     (`n00b_attest_statement_get_predicate_type`) — used to
 *     populate the `predicate_types[]` array and the per-
 *     envelope `predicate_type` slot in `envelopes[]` (bundled
 *     mode).
 *
 * If any envelope's payload cannot be borrowed (e.g., the
 * envelope was constructed via @ref n00b_attest_envelope_new but
 * never had a payload attached) or its inner Statement cannot be
 * parsed / lacks a predicateType field, this Err surfaces.
 *
 * Mark insertion is NOT attempted in this case; the artifact's
 * bytes are not touched.
 */
#define N00B_ATTEST_ERR_CHALK_BAD_ENVELOPE             (-7009)

/**
 * @brief Module-domain error: libchalk's PE / Mach-O re-sign
 *        dispatch returned an Err leg during @ref
 *        n00b_attest_mark_artifact.
 *
 * @details Surfaces from @ref n00b_attest_mark_artifact when the
 * caller-supplied @c signer_identity kwarg is non-null AND the
 * artifact's codec is PE or Mach-O AND the post-insert
 * @c n00b_chalk_pe_resign / @c n00b_chalk_macho_resign call
 * returned @c N00B_CHALK_ERR_RESIGN_FAILED. The libchalk-side Err
 * code carries no further discrimination today (parse failure,
 * hash failure, build failure, write failure, codesign(1) bridge
 * failure, etc. all collapse to the single code); a future
 * libchalk lift may split it.
 *
 * @note When this code surfaces, the mark HAS been inserted —
 * the artifact's bytes are rewritten with the chalk mark in
 * place. Only the post-insert signature step failed. Callers
 * that need atomicity of mark + sign should snapshot the
 * pre-mark bytes themselves and restore on this Err.
 *
 * WP-005 Phase 6 introduces this code (D-046 — phase introduces
 * codes when it uses them).
 */
#define N00B_ATTEST_ERR_CHALK_RESIGN_FAILED            (-7010)

/**
 * @brief Look up a human-readable string for an n00b_attest
 *        module-domain error code.
 *
 * @param err  Any `N00B_ATTEST_ERR_*` code. The function covers
 *             every code defined in this header.
 *
 * @return A non-null `n00b_string_t *` containing a short
 *         description. The returned string is a rich-string
 *         literal with process-lifetime storage; the caller must
 *         NOT free it. Unknown codes return a documented
 *         fallback string of the form
 *         `r"unknown attest error code"` (the integer value
 *         itself is not formatted into the message — call sites
 *         that need the integer have it already).
 *
 * @details This accessor is a pure lookup over a hard-coded
 * table; it allocates nothing and never fails. Repeated calls
 * with the same input return string pointers whose `data` bytes
 * are byte-identical (the rich-literal storage is process-stable).
 */
extern n00b_string_t *
n00b_attest_err_str(n00b_err_t err);
