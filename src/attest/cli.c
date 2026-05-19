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
