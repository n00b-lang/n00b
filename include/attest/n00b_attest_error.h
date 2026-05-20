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
