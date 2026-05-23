/** @file test/unit/test_attest_statement_roundtrip.c — Statement v1
 *  round-trip + determinism regression.
 *
 *  Phase-2 regression test (WP-001). Verifies:
 *    (a) `_serialize` is deterministic on the same builder (two runs
 *        produce byte-equal output).
 *    (b) The pipeline build → serialize → parse → serialize yields the
 *        same bytes as the first serialization (round-trip closure
 *        through the canonical paths).
 *
 *  Fixture: a Statement with two named subjects (each with a 32-byte
 *  sha256 digest), the SLSA v1 predicateType
 *  (`https://slsa.dev/provenance/v1`, per FR-3), and a small predicate
 *  blob exercising the canonicalization re-emit at serialize time.
 */

#include <assert.h>
#include <stdio.h>
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

static n00b_attest_statement_t *
build_fixture_statement(void)
{
    n00b_attest_statement_t *st = n00b_attest_statement_new();

    // Two subjects with distinguishable sha256 digests.
    uint8_t d1[32];
    uint8_t d2[32];
    for (int i = 0; i < 32; i++) {
        d1[i] = (uint8_t)i;
        d2[i] = (uint8_t)(0xff - i);
    }
    n00b_buffer_t *digest1 = n00b_buffer_from_bytes((char *)d1, 32);
    n00b_buffer_t *digest2 = n00b_buffer_from_bytes((char *)d2, 32);

    auto ar1 = n00b_attest_statement_add_subject(
        st,
        .name   = n00b_string_from_cstr("artifact-one"),
        .digest = digest1);
    ASSERT_OK(ar1);

    auto ar2 = n00b_attest_statement_add_subject(
        st,
        .name   = n00b_string_from_cstr("artifact-two"),
        .digest = digest2);
    ASSERT_OK(ar2);

    auto tr = n00b_attest_statement_set_predicate_type(
        st,
        n00b_string_from_cstr("https://slsa.dev/provenance/v1"));
    ASSERT_OK(tr);

    // Minimal predicate blob — JSON the encoder will re-emit verbatim.
    static const char k_predicate[] =
        "{\"buildType\":\"https://example.com/build/v1\","
        "\"builder\":{\"id\":\"https://example.com/builder\"}}";
    n00b_buffer_t *pred = n00b_buffer_from_bytes((char *)k_predicate,
                                                 (int64_t)(sizeof(k_predicate) - 1));
    auto pr = n00b_attest_statement_set_predicate_json(st, pred);
    ASSERT_OK(pr);

    return st;
}

static void
test_serialize_deterministic(void)
{
    n00b_attest_statement_t *st = build_fixture_statement();

    auto r1 = n00b_attest_statement_serialize(st);
    ASSERT_OK(r1);
    auto r2 = n00b_attest_statement_serialize(st);
    ASSERT_OK(r2);

    n00b_buffer_t *b1 = n00b_result_get(r1);
    n00b_buffer_t *b2 = n00b_result_get(r2);

    assert(b1->byte_len == b2->byte_len);
    assert(memcmp(b1->data, b2->data, b1->byte_len) == 0);
    printf("  [PASS] statement_serialize_deterministic\n");
}

static void
test_serialize_parse_roundtrip(void)
{
    n00b_attest_statement_t *st1 = build_fixture_statement();
    auto sr1 = n00b_attest_statement_serialize(st1);
    ASSERT_OK(sr1);
    n00b_buffer_t *serialized1 = n00b_result_get(sr1);

    auto pr = n00b_attest_statement_parse(serialized1);
    ASSERT_OK(pr);
    n00b_attest_statement_t *st2 = n00b_result_get(pr);

    auto sr2 = n00b_attest_statement_serialize(st2);
    ASSERT_OK(sr2);
    n00b_buffer_t *serialized2 = n00b_result_get(sr2);

    assert(serialized1->byte_len == serialized2->byte_len);
    assert(memcmp(serialized1->data,
                  serialized2->data,
                  serialized1->byte_len) == 0);

    printf("  [PASS] statement_serialize_parse_roundtrip\n");
}

static void
test_serialize_contains_expected_keys(void)
{
    n00b_attest_statement_t *st = build_fixture_statement();
    auto r = n00b_attest_statement_serialize(st);
    ASSERT_OK(r);
    n00b_buffer_t *bytes = n00b_result_get(r);

    // The _type URI, the SLSA predicateType, and "subject" must appear
    // somewhere in the encoded form — minimal sanity that the canonical
    // shape didn't silently drop fields.
    const char *needles[] = {
        "https://in-toto.io/Statement/v1",
        "https://slsa.dev/provenance/v1",
        "subject",
        "predicateType",
        "predicate",
        "sha256",
    };
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        const char *n = needles[i];
        size_t nl = strlen(n);
        bool found = false;
        for (size_t j = 0; j + nl <= bytes->byte_len; j++) {
            if (memcmp(bytes->data + j, n, nl) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "FAIL: needle '%s' missing from serialized statement\n",
                    n);
            assert(0);
        }
    }
    printf("  [PASS] statement_contains_expected_keys\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== n00b_attest statement round-trip ==\n");
    test_serialize_deterministic();
    test_serialize_parse_roundtrip();
    test_serialize_contains_expected_keys();

    printf("All n00b_attest statement round-trip tests passed.\n");
    return 0;
}
