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
