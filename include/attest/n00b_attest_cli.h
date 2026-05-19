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
