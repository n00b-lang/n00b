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
