/** @file test/unit/test_attest_dsse_roundtrip.c — DSSE envelope
 *  round-trip + PAE-byte regression.
 *
 *  Phase-2 regression test (WP-001). Verifies:
 *    (a) Serialize Statement → wrap in envelope → emit envelope JSON →
 *        re-parse → call _get_payload → assert bytes byte-equal to the
 *        original Statement bytes.
 *    (b) `_pae_bytes` produces the canonical DSSE v1 byte string
 *        (`DSSEv1 SP <type-len> SP <type> SP <payload-len> SP
 *        <payload>` with single-space separators, decimal-ASCII
 *        lengths, and the **unencoded** Statement bytes in the
 *        payload position).
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "attest/n00b_attest.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                n00b_result_get_err(r)); \
        assert(0); } } while (0)

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
    n00b_buffer_t *pred = n00b_buffer_from_bytes((char *)k_pred,
                                                 (int64_t)(sizeof(k_pred) - 1));
    auto pr = n00b_attest_statement_set_predicate_json(st, pred);
    ASSERT_OK(pr);

    auto sr = n00b_attest_statement_serialize(st);
    ASSERT_OK(sr);
    return n00b_result_get(sr);
}

static void
test_envelope_roundtrip(void)
{
    n00b_buffer_t *stmt_bytes = build_statement_bytes();

    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    auto pr = n00b_attest_envelope_set_payload(env, stmt_bytes);
    ASSERT_OK(pr);

    auto sr = n00b_attest_envelope_serialize(env);
    ASSERT_OK(sr);
    n00b_buffer_t *env_bytes = n00b_result_get(sr);

    // The emitted envelope JSON must contain "signatures":[] per
    // WP-001's no-signing contract (D-016 framing-only).
    const char *sigs_needle = "\"signatures\"";
    bool sigs_found = false;
    for (size_t j = 0; j + strlen(sigs_needle) <= env_bytes->byte_len; j++) {
        if (memcmp(env_bytes->data + j,
                   sigs_needle,
                   strlen(sigs_needle)) == 0) {
            sigs_found = true;
            break;
        }
    }
    assert(sigs_found);

    // Re-parse the envelope JSON.
    auto pr2 = n00b_attest_envelope_parse(env_bytes);
    ASSERT_OK(pr2);
    n00b_attest_envelope_t *env2 = n00b_result_get(pr2);

    // Recover the (base64-decoded) Statement bytes.
    auto gr = n00b_attest_envelope_get_payload(env2);
    ASSERT_OK(gr);
    n00b_buffer_t *recovered = n00b_result_get(gr);

    assert(recovered->byte_len == stmt_bytes->byte_len);
    assert(memcmp(recovered->data,
                  stmt_bytes->data,
                  stmt_bytes->byte_len) == 0);
    printf("  [PASS] dsse_envelope_roundtrip\n");
}

static void
test_pae_byte_shape(void)
{
    // Use a deterministic, small payload to make the expected PAE
    // string trivial to assert against.
    static const char k_payload[] = "hello-world";
    n00b_buffer_t *pl = n00b_buffer_from_bytes((char *)k_payload,
                                               (int64_t)(sizeof(k_payload) - 1));

    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    auto pr = n00b_attest_envelope_set_payload(env, pl);
    ASSERT_OK(pr);

    auto br = n00b_attest_envelope_pae_bytes(env);
    ASSERT_OK(br);
    n00b_buffer_t *pae = n00b_result_get(br);

    // Expected PAE per the DSSE v1 spec:
    //   "DSSEv1 28 application/vnd.in-toto+json 11 hello-world"
    static const char k_expected[] =
        "DSSEv1 28 application/vnd.in-toto+json 11 hello-world";
    size_t exp_len = sizeof(k_expected) - 1;
    if (pae->byte_len != exp_len
        || memcmp(pae->data, k_expected, exp_len) != 0) {
        fprintf(stderr, "FAIL: PAE byte shape\n");
        fprintf(stderr, "  got (%zu): ", pae->byte_len);
        for (size_t i = 0; i < pae->byte_len && i < 200; i++) {
            unsigned char c = (unsigned char)pae->data[i];
            if (c >= 32 && c < 127) fputc(c, stderr);
            else fprintf(stderr, "\\x%02x", c);
        }
        fprintf(stderr, "\n  exp (%zu): %s\n", exp_len, k_expected);
        assert(0);
    }
    printf("  [PASS] dsse_pae_byte_shape\n");
}

static void
test_pae_payload_is_unencoded(void)
{
    // The PAE byte string must carry the *unencoded* payload bytes,
    // never the base64 form embedded in the JSON envelope. Use binary
    // payload with bytes outside the base64 alphabet to make a
    // base64-confusion bug impossible to miss.
    uint8_t raw[5] = {0x00, 0xff, 0x10, 0x80, 0x7f};
    n00b_buffer_t *pl = n00b_buffer_from_bytes((char *)raw, 5);

    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    auto pr = n00b_attest_envelope_set_payload(env, pl);
    ASSERT_OK(pr);

    auto br = n00b_attest_envelope_pae_bytes(env);
    ASSERT_OK(br);
    n00b_buffer_t *pae = n00b_result_get(br);

    // The last 5 bytes of the PAE must be the raw payload bytes
    // verbatim — no base64, no escaping.
    assert(pae->byte_len >= 5);
    assert(memcmp(pae->data + pae->byte_len - 5, raw, 5) == 0);
    printf("  [PASS] dsse_pae_payload_unencoded\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== n00b_attest DSSE envelope round-trip ==\n");
    test_envelope_roundtrip();
    test_pae_byte_shape();
    test_pae_payload_is_unencoded();

    printf("All n00b_attest DSSE envelope round-trip tests passed.\n");
    return 0;
}
