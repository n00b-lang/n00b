/* src/attest/error.c — n00b_attest module-domain error-code accessor.
 *
 * Implements the surface declared in include/attest/n00b_attest_error.h:
 *   - n00b_attest_err_str  (pure lookup from int code to rich-literal
 *                           n00b_string_t *)
 *
 * Per D-031 A-1 closure (carried forward from WP-001) and the
 * WP-002 Phase 3 cleanup pass (`n00b-code-auditor` W-3), the
 * accessor covers every `N00B_ATTEST_ERR_*` code defined in the
 * module — all sixteen codes live in n00b_attest_error.h:
 *   - Statement codes  (-1001 .. -1004)
 *   - DSSE codes       (-2001 .. -2005)
 *   - Signer codes     (-4001 .. -4007)
 *
 * WP-003 Phase 2 extends the namespace with five verifier codes
 * (-5001 .. -5005) per D-046 (supersedes D-044 OQ-2's original
 * Phase 3 phase assignment). WP-003 Phase 3 (D-047 W-1) adds two
 * runtime-check-path codes (-5006, -5007) under the `_VERIFY_*`
 * prefix and migrates three placeholder callsites that Phase 2
 * temporarily routed through @ref N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND.
 * Total now twenty-three codes.
 *
 * Unknown codes return a documented fallback string (per the
 * api-guidelines § 5 contract that domain-specific `*_err_str`
 * accessors cover the full namespace).
 *
 * Implementation note: we use a `switch` statement against the
 * macro values rather than an array because the codes are non-
 * contiguous integers and a switch lets the compiler emit a
 * jump-table for the dense ranges while still handling sparsely-
 * defined codes cleanly. The bodies are rich-string literals
 * (`r"..."`) — process-lifetime storage; the accessor is pure and
 * allocation-free.
 */

#include <attest/n00b_attest.h>

#include "core/string.h"

n00b_string_t *
n00b_attest_err_str(n00b_err_t err)
{
    switch (err) {
    // Statement domain (-1001 .. -1004).
    case N00B_ATTEST_ERR_STMT_BAD_INPUT:
        return r"statement: bad or missing input argument";
    case N00B_ATTEST_ERR_STMT_MISSING_FIELD:
        return r"statement: required field missing";
    case N00B_ATTEST_ERR_STMT_BAD_JSON:
        return r"statement: JSON parse failed";
    case N00B_ATTEST_ERR_STMT_WRONG_TYPE:
        return r"statement: unexpected _type value";

    // DSSE-envelope domain (-2001 .. -2005).
    case N00B_ATTEST_ERR_DSSE_BAD_INPUT:
        return r"dsse: bad or missing input argument";
    case N00B_ATTEST_ERR_DSSE_NO_PAYLOAD:
        return r"dsse: payload not attached";
    case N00B_ATTEST_ERR_DSSE_BAD_JSON:
        return r"dsse: JSON parse failed";
    case N00B_ATTEST_ERR_DSSE_WRONG_TYPE:
        return r"dsse: unexpected payloadType";
    case N00B_ATTEST_ERR_DSSE_BAD_BASE64:
        return r"dsse: payload base64 decode failed";

    // Signer domain (-4001 .. -4007).
    case N00B_ATTEST_ERR_UNSUPPORTED_SCHEME:
        return r"signer: URI scheme has no registered backend";
    case N00B_ATTEST_ERR_KEY_NOT_FOUND:
        return r"signer: key not found at the referenced URI";
    case N00B_ATTEST_ERR_PEM_PARSE_FAILED:
        return r"signer: PEM container parse failed";
    case N00B_ATTEST_ERR_DER_PARSE_FAILED:
        return r"signer: DER structure parse failed";
    case N00B_ATTEST_ERR_UNSUPPORTED_ALGORITHM:
        return r"signer: key algorithm not supported";
    case N00B_ATTEST_ERR_SIGN_FAILED:
        return r"signer: signing primitive failed";
    case N00B_ATTEST_ERR_NOT_IMPLEMENTED:
        return r"signer: code path not yet implemented";

    // Verifier domain (-5001 .. -5005), WP-003 Phase 2 per D-046.
    case N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_SCHEME:
        return r"verifier: URI scheme has no registered backend";
    case N00B_ATTEST_ERR_VERIFIER_KEY_NOT_FOUND:
        return r"verifier: key not found at the referenced URI";
    case N00B_ATTEST_ERR_VERIFIER_PEM_PARSE_FAILED:
        return r"verifier: PEM container parse failed";
    case N00B_ATTEST_ERR_VERIFIER_DER_PARSE_FAILED:
        return r"verifier: DER structure parse failed";
    case N00B_ATTEST_ERR_VERIFIER_UNSUPPORTED_ALGORITHM:
        return r"verifier: key algorithm not supported";

    // Verify check-path domain (-5006 .. -5007), WP-003 Phase 3
    // per D-047 W-1. `_VERIFY_*` prefix flags runtime-check-path
    // errors; `_VERIFIER_*` (above) flags resolver-edge errors.
    case N00B_ATTEST_ERR_VERIFY_BAD_SIG_LENGTH:
        return r"verify: signature length does not match algorithm";
    case N00B_ATTEST_ERR_VERIFY_BAD_INPUT:
        return r"verify: bad or missing input argument";

    default:
        return r"unknown attest error code";
    }
}
