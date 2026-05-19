/** @file test/unit/test_attest_envelope_signatures_roundtrip.c
 *  — DSSE envelope signatures[] parse-side round-trip regression
 *  (WP-003 Phase 1; closes DF-006).
 *
 *  WP-002 Phase 3 shipped the envelope-emission side of
 *  `signatures[]` (the write side of `n00b_attest_envelope_add_signature`
 *  + `n00b_attest_envelope_sign`). The parser at
 *  `src/attest/dsse.c::n00b_attest_envelope_parse` did NOT reconstruct
 *  the array on the read side — the WP-001 round-trip test only
 *  exercised payload recovery, and WP-002's envelope-sign test only
 *  asserted the serialized JSON contained the right keyid / sig
 *  bytes (the parsed envelope had no signatures populated). WP-003
 *  Phase 1 closes that gap with three new public accessors
 *  (`_signature_count`, `_get_signature_keyid`, `_get_signature_sig`)
 *  and a parser extension that walks the wire `signatures[]` and
 *  appends each entry into the same private-copy + lazy-init
 *  parallel-list machinery the write side uses.
 *
 *  This test covers five sub-cases:
 *
 *    [1] Round-trip with two distinct {keyid, sig} pairs.
 *        Build an envelope, attach a payload, append two
 *        hand-baked {keyid, sig} pairs via `_add_signature`,
 *        serialize, then re-parse via `_envelope_parse`. Assert
 *        the parsed envelope's signature_count == 2 and each
 *        getter returns byte-identical keyid / sig bytes.
 *
 *    [2] Empty `signatures: []` case. Parse an envelope JSON
 *        with an empty signatures array and assert count == 0.
 *
 *    [3] No-array case. Parse an envelope JSON with NO
 *        `signatures` field at all (the WP-001 round-trip
 *        envelope shape — though the WP-001 emitter always
 *        writes `[]`, the parser must tolerate the field's
 *        absence per D-016's JSON-superset principle). Assert
 *        count == 0; NOT an error.
 *
 *    [4] Malformed-entry case (structural). Parse `{"signatures":
 *        [{"keyid": "x"}]}` (the `sig` field is missing). Assert
 *        the parser propagates `N00B_ATTEST_ERR_DSSE_BAD_JSON`.
 *        Structural-JSON failures (missing field, wrong type)
 *        propagate BAD_JSON.
 *
 *    [5] Non-base64 sig case (content). Parse `{"signatures":
 *        [{"keyid": "...", "sig": "###not-valid-base64###"}]}`.
 *        Assert the parser propagates
 *        `N00B_ATTEST_ERR_DSSE_BAD_BASE64`. Mirrors the top-level
 *        payload-base64 precedent so audit logs distinguish
 *        structural-JSON failures (sub-case [4]) from base64-
 *        content failures (this sub-case) regardless of nesting
 *        depth. The two sub-cases together exercise the
 *        BAD_JSON vs BAD_BASE64 distinction at per-entry depth.
 *
 *  Per D-030 the test-file carve-out applies — libc I/O for
 *  status logging via `printf` is fine. Per D-039 the keyid
 *  bytes are preserved verbatim; the test exercises that with
 *  the canonical RFC 8032 §7.1 vector #1 keyid as one fixture
 *  and a distinct 64-hex string as the other (the parser does
 *  NOT validate keyid length or hex-shape, so the second value
 *  is an arbitrary byte-equality oracle).
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "attest/n00b_attest.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                n00b_result_get_err(r)); \
        assert(0); } } while (0)

// Two hand-baked keyids. `keyid_a` is the canonical D-039 keyid for
// the RFC 8032 §7.1 vector #1 key (canonical-by-spec value, used
// here only as a unique 64-hex byte sequence). `keyid_b` is a
// distinct 64-hex string — NOT a real SHA-256 of any pubkey, but
// the parser does not validate keyid shape (D-039), so byte-equality
// round-trip is the only contract.
static const char k_keyid_a[]
    = "06e3fd8fda29bb60ab59557de61edb0aecdb231134be30e75b455f8e1b792fa9";
static const char k_keyid_b[]
    = "deadbeefcafef00ddeadbeefcafef00ddeadbeefcafef00ddeadbeefcafef00d";

// Two arbitrary 64-byte sig buffers. They need NOT verify under any
// pubkey — this is a serialize/parse round-trip test, not a sign
// verify test. The contract under test is "what the appender put in
// comes back out byte-identical."
static void
make_sig_buffer(uint8_t out[64], uint8_t seed)
{
    for (int i = 0; i < 64; i++) {
        out[i] = (uint8_t)((i * 31u + seed) & 0xff);
    }
}

static n00b_buffer_t *
build_statement_bytes(void)
{
    n00b_attest_statement_t *st = n00b_attest_statement_new();

    uint8_t d[32];
    for (int i = 0; i < 32; i++) d[i] = (uint8_t)(i * 7 + 3);
    n00b_buffer_t *digest = n00b_buffer_from_bytes((char *)d, 32);

    auto ar = n00b_attest_statement_add_subject(
        st,
        .name   = n00b_string_from_cstr("hello"),
        .digest = digest);
    ASSERT_OK(ar);

    auto tr = n00b_attest_statement_set_predicate_type(
        st,
        n00b_string_from_cstr("https://slsa.dev/provenance/v1"));
    ASSERT_OK(tr);

    static const char k_pred[] = "{\"foo\":42}";
    n00b_buffer_t *pred = n00b_buffer_from_bytes(
        (char *)k_pred,
        (int64_t)(sizeof(k_pred) - 1));
    auto pr = n00b_attest_statement_set_predicate_json(st, pred);
    ASSERT_OK(pr);

    auto sr = n00b_attest_statement_serialize(st);
    ASSERT_OK(sr);
    return n00b_result_get(sr);
}

// [1] — full round-trip with two appended {keyid, sig} pairs.
static void
test_signatures_roundtrip_two_entries(void)
{
    n00b_buffer_t *stmt_bytes = build_statement_bytes();

    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    auto spr = n00b_attest_envelope_set_payload(env, stmt_bytes);
    ASSERT_OK(spr);

    uint8_t sig_a_bytes[64];
    uint8_t sig_b_bytes[64];
    make_sig_buffer(sig_a_bytes, 0x11);
    make_sig_buffer(sig_b_bytes, 0xa3);

    n00b_string_t *kid_a = n00b_string_from_cstr(k_keyid_a);
    n00b_string_t *kid_b = n00b_string_from_cstr(k_keyid_b);
    n00b_buffer_t *sig_a = n00b_buffer_from_bytes((char *)sig_a_bytes, 64);
    n00b_buffer_t *sig_b = n00b_buffer_from_bytes((char *)sig_b_bytes, 64);

    auto ar1 = n00b_attest_envelope_add_signature(env, kid_a, sig_a);
    ASSERT_OK(ar1);
    auto ar2 = n00b_attest_envelope_add_signature(env, kid_b, sig_b);
    ASSERT_OK(ar2);

    // Cross-check on the build-side envelope too: the new accessors
    // see what add_signature appended.
    assert(n00b_attest_envelope_signature_count(env) == 2);

    auto ser = n00b_attest_envelope_serialize(env);
    ASSERT_OK(ser);
    n00b_buffer_t *env_json = n00b_result_get(ser);

    // Parse the serialized JSON back.
    auto pr2 = n00b_attest_envelope_parse(env_json);
    ASSERT_OK(pr2);
    n00b_attest_envelope_t *env2 = n00b_result_get(pr2);

    // Count must be 2.
    assert(n00b_attest_envelope_signature_count(env2) == 2);

    // Entry 0: keyid_a, sig_a_bytes.
    auto k0r = n00b_attest_envelope_get_signature_keyid(env2, 0);
    ASSERT_OK(k0r);
    n00b_string_t *k0 = n00b_result_get(k0r);
    assert(k0->u8_bytes == strlen(k_keyid_a));
    assert(memcmp(k0->data, k_keyid_a, k0->u8_bytes) == 0);

    auto s0r = n00b_attest_envelope_get_signature_sig(env2, 0);
    ASSERT_OK(s0r);
    n00b_buffer_t *s0 = n00b_result_get(s0r);
    assert(s0->byte_len == 64);
    assert(memcmp(s0->data, sig_a_bytes, 64) == 0);

    // Entry 1: keyid_b, sig_b_bytes.
    auto k1r = n00b_attest_envelope_get_signature_keyid(env2, 1);
    ASSERT_OK(k1r);
    n00b_string_t *k1 = n00b_result_get(k1r);
    assert(k1->u8_bytes == strlen(k_keyid_b));
    assert(memcmp(k1->data, k_keyid_b, k1->u8_bytes) == 0);

    auto s1r = n00b_attest_envelope_get_signature_sig(env2, 1);
    ASSERT_OK(s1r);
    n00b_buffer_t *s1 = n00b_result_get(s1r);
    assert(s1->byte_len == 64);
    assert(memcmp(s1->data, sig_b_bytes, 64) == 0);

    // OOB check: idx == count must surface BAD_INPUT.
    auto oob = n00b_attest_envelope_get_signature_keyid(env2, 2);
    assert(n00b_result_is_err(oob));
    assert(n00b_result_get_err(oob) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);
    auto oob_s = n00b_attest_envelope_get_signature_sig(env2, 2);
    assert(n00b_result_is_err(oob_s));
    assert(n00b_result_get_err(oob_s) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    printf("  [PASS] signatures_roundtrip_two_entries\n");
}

// [2] — empty array.
static void
test_signatures_empty_array(void)
{
    // Hand-baked envelope JSON with `"signatures": []`.
    // payload is the base64 of "x" so the parser accepts it.
    static const char k_env_json[]
        = "{\"payloadType\":\"application/vnd.in-toto+json\","
          "\"payload\":\"eA==\","
          "\"signatures\":[]}";

    n00b_buffer_t *env_bytes = n00b_buffer_from_bytes(
        (char *)k_env_json,
        (int64_t)(sizeof(k_env_json) - 1));

    auto pr = n00b_attest_envelope_parse(env_bytes);
    ASSERT_OK(pr);
    n00b_attest_envelope_t *env = n00b_result_get(pr);

    assert(n00b_attest_envelope_signature_count(env) == 0);

    auto oob = n00b_attest_envelope_get_signature_keyid(env, 0);
    assert(n00b_result_is_err(oob));
    assert(n00b_result_get_err(oob) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    printf("  [PASS] signatures_empty_array\n");
}

// [3] — no signatures field present.
static void
test_signatures_field_absent(void)
{
    // Hand-baked envelope JSON with NO `signatures` field. The
    // parser must tolerate this and yield an empty list (NOT an
    // error). payload again is base64 of "x".
    static const char k_env_json[]
        = "{\"payloadType\":\"application/vnd.in-toto+json\","
          "\"payload\":\"eA==\"}";

    n00b_buffer_t *env_bytes = n00b_buffer_from_bytes(
        (char *)k_env_json,
        (int64_t)(sizeof(k_env_json) - 1));

    auto pr = n00b_attest_envelope_parse(env_bytes);
    ASSERT_OK(pr);
    n00b_attest_envelope_t *env = n00b_result_get(pr);

    assert(n00b_attest_envelope_signature_count(env) == 0);

    printf("  [PASS] signatures_field_absent\n");
}

// [4] — malformed entry (missing `sig`).
static void
test_signatures_malformed_entry(void)
{
    static const char k_env_json[]
        = "{\"payloadType\":\"application/vnd.in-toto+json\","
          "\"payload\":\"eA==\","
          "\"signatures\":[{\"keyid\":\"x\"}]}";

    n00b_buffer_t *env_bytes = n00b_buffer_from_bytes(
        (char *)k_env_json,
        (int64_t)(sizeof(k_env_json) - 1));

    auto pr = n00b_attest_envelope_parse(env_bytes);
    assert(n00b_result_is_err(pr));
    assert(n00b_result_get_err(pr) == N00B_ATTEST_ERR_DSSE_BAD_JSON);

    printf("  [PASS] signatures_malformed_entry\n");
}

// [5] — per-entry sig field present but not valid base64. Per the
// W-2 cleanup-pass disposition, base64-content failures at per-entry
// depth propagate BAD_BASE64 (NOT BAD_JSON), mirroring the top-level
// payload-base64 precedent. Combined with [4], the two sub-cases
// validate the structural-vs-content error-code distinction.
//
// The sig string `###not-valid-base64###` contains `#` characters
// which are NOT in the RFC 4648 base64 alphabet — the decode util
// rejects it. The keyid is `x` (arbitrary; structurally valid as a
// JSON string per [4], so the structural check passes and the
// decode is reached).
static void
test_signatures_non_base64_sig(void)
{
    static const char k_env_json[]
        = "{\"payloadType\":\"application/vnd.in-toto+json\","
          "\"payload\":\"eA==\","
          "\"signatures\":[{\"keyid\":\"x\","
          "\"sig\":\"###not-valid-base64###\"}]}";

    n00b_buffer_t *env_bytes = n00b_buffer_from_bytes(
        (char *)k_env_json,
        (int64_t)(sizeof(k_env_json) - 1));

    auto pr = n00b_attest_envelope_parse(env_bytes);
    assert(n00b_result_is_err(pr));
    assert(n00b_result_get_err(pr) == N00B_ATTEST_ERR_DSSE_BAD_BASE64);

    printf("  [PASS] signatures_non_base64_sig\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest envelope signatures[] round-trip ==\n");
    test_signatures_roundtrip_two_entries();
    test_signatures_empty_array();
    test_signatures_field_absent();
    test_signatures_malformed_entry();
    test_signatures_non_base64_sig();
    printf("All n00b_attest envelope signatures[] tests passed.\n");
    return 0;
}
