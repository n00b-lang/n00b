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
 * artifacts (the test under `test/unit/test_attest_cli_sign.c`)
 * drive these cores directly via in-memory buffers; the verb
 * cores never read from `stdin`, never write to `stdout`, and
 * never `open()` a file path. Path-binding belongs to the shim.
 *
 * # Scope
 *
 * Only the `sign` verb is wired in WP-002. Other verbs (`verify`,
 * `inspect`, `push`, `pull`, ...) are registered with commander
 * as not-yet-implemented stubs in the binary so `--help`
 * discovery lists them (DF-004 disposition (a) per user direction
 * 2026-05-18); their library-shaped cores arrive in later WPs.
 *
 * # Allocator discipline
 *
 * Each core accepts `.allocator = nullptr` and threads it into
 * every downstream call (Statement parse, envelope construction,
 * signer resolve, envelope sign, envelope serialize). Threading
 * an arena through here keeps every byte the verb produces in
 * that arena (FR-21 / FR-22). Per D-042 part W-2 the signer's
 * resolve-time allocator inherits forward into `_sign`, so the
 * CLI sees arena-attributed signatures without re-threading the
 * sign call site itself.
 */

#include <n00b.h>
#include "adt/result.h"
#include <attest/n00b_attest_statement.h>
#include <attest/n00b_attest_dsse.h>
#include <attest/n00b_attest_signer.h>

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
