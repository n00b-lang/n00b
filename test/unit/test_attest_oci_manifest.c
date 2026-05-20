/** @file test/unit/test_attest_oci_manifest.c — OCI 1.1 artifact-
 *  manifest serializer + SHA-256 buffer-digest regression test
 *  (WP-004 Phase 2).
 *
 *  Exercises the producer-side substrate against in-memory
 *  fixtures (no network). Sub-cases:
 *
 *    [1] `_digest_of_buffer` against RFC 6234 SHA-256 test
 *        vectors #1 ("abc") and #2 (multi-block).
 *    [2] `_manifest_build` with fixed inputs produces a
 *        byte-stable canonical output across runs.
 *    [3] `_manifest_build` output matches an expected-golden
 *        literal in spec §8.2 shape.
 *    [4] `_manifest_build` field-order regression: parses the
 *        produced JSON and asserts fields appear in spec §8.2
 *        order (NOT alphabetical, NOT hashmap-iteration order).
 *
 *  Test-file carve-out (D-030) applies — libc I/O for stdout
 *  logging is acceptable per the established test-file precedent.
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
#include "internal/attest/oci/registry.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                (int)n00b_result_get_err(r)); \
        assert(0); } } while (0)

// RFC 6234 SHA-256 test vectors:
//   #1: input "abc" → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
//   #2: input "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
//        → 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
static const char k_vec1_input[]  = "abc";
static const char k_vec1_digest[] =
    "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static const char k_vec2_input[] =
    "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
static const char k_vec2_digest[] =
    "sha256:248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

static void
assert_strings_equal_bytes(const char *ctx,
                           n00b_string_t *got,
                           const char    *expected)
{
    size_t exp_len = strlen(expected);
    if (got == nullptr) {
        fprintf(stderr, "FAIL: %s: got null string\n", ctx);
        assert(0);
    }
    if (got->u8_bytes != exp_len
        || memcmp(got->data, expected, exp_len) != 0) {
        fprintf(stderr,
                "FAIL: %s:\n  got:      '%.*s' (%zu bytes)\n"
                "  expected: '%s' (%zu bytes)\n",
                ctx,
                (int)got->u8_bytes, got->data, (size_t)got->u8_bytes,
                expected, exp_len);
        assert(0);
    }
}

static void
test_digest_of_buffer_rfc6234_vector_1(void)
{
    n00b_buffer_t *buf = n00b_buffer_from_bytes(
        (char *)k_vec1_input,
        (int64_t)(sizeof(k_vec1_input) - 1));
    auto r = n00b_attest_oci_digest_of_buffer(buf);
    ASSERT_OK(r);
    assert_strings_equal_bytes("rfc6234-vec1", n00b_result_get(r),
                                k_vec1_digest);
    printf("  [PASS] digest_of_buffer_rfc6234_vector_1\n");
}

static void
test_digest_of_buffer_rfc6234_vector_2(void)
{
    n00b_buffer_t *buf = n00b_buffer_from_bytes(
        (char *)k_vec2_input,
        (int64_t)(sizeof(k_vec2_input) - 1));
    auto r = n00b_attest_oci_digest_of_buffer(buf);
    ASSERT_OK(r);
    assert_strings_equal_bytes("rfc6234-vec2", n00b_result_get(r),
                                k_vec2_digest);
    printf("  [PASS] digest_of_buffer_rfc6234_vector_2\n");
}

static void
test_digest_of_buffer_empty(void)
{
    // SHA-256("") =
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    n00b_buffer_t *buf = n00b_buffer_empty();
    auto r = n00b_attest_oci_digest_of_buffer(buf);
    ASSERT_OK(r);
    assert_strings_equal_bytes("digest-of-empty",
        n00b_result_get(r),
        "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    printf("  [PASS] digest_of_buffer_empty\n");
}

static void
test_manifest_build_golden(void)
{
    // Fixed inputs.
    n00b_string_t *image_digest =
        r"sha256:1111111111111111111111111111111111111111111111111111111111111111";
    uint64_t       image_size = 527;
    n00b_string_t *envelope_digest =
        r"sha256:2222222222222222222222222222222222222222222222222222222222222222";
    uint64_t       envelope_size = 4096;
    n00b_string_t *predicate_type =
        r"https://slsa.dev/provenance/v1";
    n00b_string_t *signer_keyid =
        r"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

    auto r = n00b_attest_oci_manifest_build(image_digest, image_size,
                                             envelope_digest, envelope_size,
                                             predicate_type, signer_keyid);
    ASSERT_OK(r);
    n00b_buffer_t *out = n00b_result_get(r);

    // sha256("{}") = 44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a
    const char *expected =
        "{\"schemaVersion\":2,"
        "\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
        "\"artifactType\":\"application/vnd.in-toto+dsse\","
        "\"config\":{"
        "\"mediaType\":\"application/vnd.oci.empty.v1+json\","
        "\"digest\":\"sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a\","
        "\"size\":2"
        "},"
        "\"layers\":[{"
        "\"mediaType\":\"application/vnd.in-toto+json\","
        "\"digest\":\"sha256:2222222222222222222222222222222222222222222222222222222222222222\","
        "\"size\":4096"
        "}],"
        "\"subject\":{"
        "\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
        "\"digest\":\"sha256:1111111111111111111111111111111111111111111111111111111111111111\","
        "\"size\":527"
        "},"
        "\"annotations\":{"
        "\"com.crashoverride.attestation.predicate-type\":"
        "\"https://slsa.dev/provenance/v1\","
        "\"com.crashoverride.attestation.signer-keyid\":"
        "\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\""
        "}}";

    size_t expected_len = strlen(expected);
    if ((size_t)out->byte_len != expected_len
        || memcmp(out->data, expected, expected_len) != 0) {
        fprintf(stderr,
                "FAIL: manifest_build golden mismatch\n"
                "  got      (%zd bytes): %.*s\n"
                "  expected (%zu bytes): %s\n",
                (ssize_t)out->byte_len, (int)out->byte_len, out->data,
                expected_len, expected);
        assert(0);
    }
    printf("  [PASS] manifest_build_golden\n");
}

static void
test_manifest_build_byte_stability(void)
{
    n00b_string_t *image_digest =
        r"sha256:1111111111111111111111111111111111111111111111111111111111111111";
    uint64_t       image_size = 527;
    n00b_string_t *envelope_digest =
        r"sha256:2222222222222222222222222222222222222222222222222222222222222222";
    uint64_t       envelope_size = 4096;
    n00b_string_t *predicate_type = r"https://slsa.dev/provenance/v1";
    n00b_string_t *signer_keyid =
        r"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

    auto r1 = n00b_attest_oci_manifest_build(image_digest, image_size,
                                              envelope_digest, envelope_size,
                                              predicate_type, signer_keyid);
    ASSERT_OK(r1);
    auto r2 = n00b_attest_oci_manifest_build(image_digest, image_size,
                                              envelope_digest, envelope_size,
                                              predicate_type, signer_keyid);
    ASSERT_OK(r2);

    n00b_buffer_t *b1 = n00b_result_get(r1);
    n00b_buffer_t *b2 = n00b_result_get(r2);
    if (b1->byte_len != b2->byte_len
        || memcmp(b1->data, b2->data, b1->byte_len) != 0) {
        fprintf(stderr, "FAIL: manifest_build not byte-stable across runs\n");
        assert(0);
    }
    printf("  [PASS] manifest_build_byte_stability\n");
}

static void
test_manifest_build_field_order(void)
{
    // Same inputs as golden. We assert the literal byte positions
    // of the top-level field names — finding "schemaVersion" then
    // "mediaType" then "artifactType" etc., each AFTER the previous
    // one's offset. If the implementation ever switches to dict-
    // iter encoding the order will drift and this test fails.
    n00b_string_t *image_digest =
        r"sha256:1111111111111111111111111111111111111111111111111111111111111111";
    n00b_string_t *envelope_digest =
        r"sha256:2222222222222222222222222222222222222222222222222222222222222222";
    n00b_string_t *predicate_type = r"https://slsa.dev/provenance/v1";
    n00b_string_t *signer_keyid =
        r"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

    auto r = n00b_attest_oci_manifest_build(image_digest, 100,
                                             envelope_digest, 200,
                                             predicate_type, signer_keyid);
    ASSERT_OK(r);
    n00b_buffer_t *buf = n00b_result_get(r);

    // Make a NUL-terminated copy so we can use strstr.
    char *cstr = (char *)malloc((size_t)buf->byte_len + 1);
    memcpy(cstr, buf->data, (size_t)buf->byte_len);
    cstr[buf->byte_len] = '\0';

    const char *order[] = {
        "\"schemaVersion\"",
        "\"mediaType\"",
        "\"artifactType\"",
        "\"config\"",
        "\"layers\"",
        "\"subject\"",
        "\"annotations\"",
        nullptr,
    };

    size_t prev_offset = 0;
    for (int i = 0; order[i] != nullptr; i++) {
        const char *p = strstr(cstr + prev_offset, order[i]);
        if (p == nullptr) {
            fprintf(stderr,
                    "FAIL: manifest_build_field_order: '%s' not found "
                    "after offset %zu\n",
                    order[i], prev_offset);
            free(cstr);
            assert(0);
        }
        prev_offset = (size_t)(p - cstr) + 1;
    }
    free(cstr);
    printf("  [PASS] manifest_build_field_order\n");
}

static void
test_manifest_build_rejects_nulls(void)
{
    n00b_string_t *valid_digest =
        r"sha256:1111111111111111111111111111111111111111111111111111111111111111";

    // Null image_digest.
    auto r1 = n00b_attest_oci_manifest_build(nullptr, 10,
                                              valid_digest, 10,
                                              r"x", r"y");
    assert(n00b_result_is_err(r1));
    // Null envelope_digest.
    auto r2 = n00b_attest_oci_manifest_build(valid_digest, 10,
                                              nullptr, 10,
                                              r"x", r"y");
    assert(n00b_result_is_err(r2));
    // Null predicate_type.
    auto r3 = n00b_attest_oci_manifest_build(valid_digest, 10,
                                              valid_digest, 10,
                                              nullptr, r"y");
    assert(n00b_result_is_err(r3));
    // Null signer_keyid.
    auto r4 = n00b_attest_oci_manifest_build(valid_digest, 10,
                                              valid_digest, 10,
                                              r"x", nullptr);
    assert(n00b_result_is_err(r4));
    printf("  [PASS] manifest_build_rejects_nulls\n");
}

int
main(int argc, char *argv[])
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    n00b_attest_module_init();

    printf("== test_attest_oci_manifest ==\n");
    test_digest_of_buffer_rfc6234_vector_1();
    test_digest_of_buffer_rfc6234_vector_2();
    test_digest_of_buffer_empty();
    test_manifest_build_golden();
    test_manifest_build_byte_stability();
    test_manifest_build_field_order();
    test_manifest_build_rejects_nulls();
    printf("All test_attest_oci_manifest tests passed.\n");

    n00b_shutdown();
    return 0;
}
