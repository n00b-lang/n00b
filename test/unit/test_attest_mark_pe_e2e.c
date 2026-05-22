/** @file test/unit/test_attest_mark_pe_e2e.c — WP-005 Phase 6
 *  end-to-end mark + resign regression on the PE code path
 *  (cross-platform Authenticode signing).
 *
 *  Coverage:
 *
 *    [A] Build a synthetic minimal PE32+ binary in-memory, write
 *        to a temp file, resolve a signer identity from the
 *        committed PEM fixtures, mark with the identity, extract
 *        the envelope, verify the IC-4 invariant (pre-mark hash
 *        equals subject digest), and confirm structural
 *        properties of the re-signed binary (parses cleanly,
 *        exactly one certificate, non-zero cert blob).
 *
 *  # IC-4 invariant
 *
 *  Envelope's `subject.digest.sha256` is the SHA-256 of the bytes
 *  BEFORE mark insertion. Recorded BEFORE the mark; cross-checked
 *  after extraction against the embedded Statement.
 *
 *  # Fixture choice (per dispatch)
 *
 *  Synthetic in-test PE32+ via `n00b_pe_binary_new` (the P4
 *  precedent). The synthetic shape gives us a byte-stable golden
 *  for deterministic signing (sign twice → byte-identical
 *  output), which the P4 unit test already covers; this E2E test
 *  focuses on the mark+resign dispatch wiring through
 *  `n00b_attest_mark_artifact`. The committed Sysinternals
 *  `sigcheck64.exe` is downloaded by a meson `custom_target` into
 *  the build dir, NOT committed to the repo; the synthetic path
 *  removes that fixture dependency.
 *
 *  # Test-file carve-out (D-030)
 *
 *  libc I/O for tmpfile setup + stdout logging is acceptable per
 *  the established test-file precedent.
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "core/sha256.h"
#include "core/file.h"
#include "compiler/objfile/pe.h"
#include "compiler/objfile/pe_build.h"
#include "compiler/objfile/pe_types.h"
#include "compiler/objfile/bstream.h"
#include "attest/n00b_attest.h"

#include "chalk/n00b_chalk.h"
#include "chalk/n00b_chalk_resign.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                (int)n00b_result_get_err(r)); \
        assert(0); } } while (0)

static const char k_signer_keyid[] =
    "06e3fd8fda29bb60ab59557de61edb0aecdb231134be30e75b455f8e1b792fa9";

static uint8_t k_signature[64];

static void
sha256_of(const uint8_t *bytes, size_t len, uint8_t out[32])
{
    n00b_sha256_digest_t digest;
    n00b_sha256_hash(bytes, len, digest);
    for (int i = 0; i < 8; i++) {
        uint32_t w = digest[i];
        out[i * 4 + 0] = (uint8_t)((w >> 24) & 0xff);
        out[i * 4 + 1] = (uint8_t)((w >> 16) & 0xff);
        out[i * 4 + 2] = (uint8_t)((w >> 8) & 0xff);
        out[i * 4 + 3] = (uint8_t)(w & 0xff);
    }
}

static n00b_attest_envelope_t *
build_envelope_for_hash(const uint8_t pre_mark_hash[32])
{
    static const char hex[] = "0123456789abcdef";
    char digest_hex[65];
    for (int i = 0; i < 32; i++) {
        digest_hex[i * 2 + 0] = hex[(pre_mark_hash[i] >> 4) & 0xf];
        digest_hex[i * 2 + 1] = hex[pre_mark_hash[i] & 0xf];
    }
    digest_hex[64] = '\0';

    char stmt_json[1024];
    int n = snprintf(stmt_json, sizeof(stmt_json),
        "{\"_type\":\"https://in-toto.io/Statement/v1\","
        "\"subject\":[{\"name\":\"hello.exe\","
        "\"digest\":{\"sha256\":\"%s\"}}],"
        "\"predicateType\":\"https://slsa.dev/provenance/v1\","
        "\"predicate\":{\"builder\":{\"id\":\"test\"},\"buildType\":\"test\"}}",
        digest_hex);
    assert(n > 0 && (size_t)n < sizeof(stmt_json));

    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    n00b_buffer_t          *pay = n00b_buffer_from_bytes(stmt_json,
                                                          (int64_t)n);
    auto sp = n00b_attest_envelope_set_payload(env, pay);
    ASSERT_OK(sp);
    n00b_string_t *kid = n00b_string_from_cstr(k_signer_keyid);
    n00b_buffer_t *sig = n00b_buffer_from_bytes((char *)k_signature, 64);
    auto add = n00b_attest_envelope_add_signature(env, kid, sig);
    ASSERT_OK(add);
    return env;
}

// Build a synthetic minimal PE32+ binary in memory and write it
// to a temp file. Mirrors `write_synthetic_pe` in
// test_chalk_pe_resign.c. Returns the temp path (n00b_string_t *).
static n00b_string_t *
write_synthetic_pe(const char *suffix)
{
    n00b_pe_binary_t *bin = n00b_pe_binary_new(
        N00B_PE_MACHINE_AMD64,
        N00B_PE_SUBSYSTEM_WINDOWS_CUI);
    bin->entry_point = 0x1000;

    n00b_pe_section_t *text = n00b_pe_add_section(
        bin, ".text",
        N00B_PE_SCN_CNT_CODE | N00B_PE_SCN_MEM_EXECUTE | N00B_PE_SCN_MEM_READ);
    uint8_t code[] = { 0xCC, 0x90, 0xC3 };
    text->content = n00b_buffer_from_bytes((char *)code, 3);

    auto br = n00b_pe_build(bin);
    assert(n00b_result_is_ok(br));
    n00b_buffer_t *built = n00b_result_get(br);
    assert(built != nullptr);
    assert(built->byte_len > 0);

    char tmpl[64];
    snprintf(tmpl, sizeof(tmpl), "synthetic_pe_e2e_%s_XXXXXX.exe", suffix);
    int fd = mkstemps(tmpl, 4);
    assert(fd >= 0);
    ssize_t w = write(fd, built->data, built->byte_len);
    assert(w == (ssize_t)built->byte_len);
    close(fd);

    return n00b_string_from_cstr(tmpl);
}

static n00b_buffer_t *
slurp_path(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f != nullptr);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz);
    size_t nr = fread(buf, 1, (size_t)sz, f);
    assert(nr == (size_t)sz);
    fclose(f);
    n00b_buffer_t *out = n00b_buffer_from_bytes(buf, (int64_t)sz);
    free(buf);
    return out;
}

static void
test_pe_round_trip_with_resign(void)
{
    fprintf(stderr, "[pe-e2e] round-trip with resign (Authenticode)\n");

    const char *src_root = getenv("MESON_SOURCE_ROOT");
    if (src_root == NULL) {
        fprintf(stderr, "  [SKIP] MESON_SOURCE_ROOT not set\n");
        return;
    }

    char uri_buf[1024];
    int uri_len = snprintf(uri_buf, sizeof(uri_buf),
        "file://%s/test/unit/data/pkcs7_fixture_cert.pem,"
        "file://%s/test/unit/data/pkcs7_fixture_key.pem",
        src_root, src_root);
    assert(uri_len > 0 && (size_t)uri_len < sizeof(uri_buf));

    // Resolve the signer identity from the PEM fixtures.
    n00b_string_t *uri = n00b_string_from_cstr(uri_buf);
    auto ir = n00b_chalk_signer_identity_resolve(uri);
    ASSERT_OK(ir);
    n00b_chalk_signer_identity_t *identity = n00b_result_get(ir);
    assert(identity != nullptr);

    // Build a synthetic PE and write it to a temp file.
    n00b_string_t *path_str = write_synthetic_pe("mark");
    const char *path_c = path_str->data;

    // (1) Pre-mark hash of the temp file.
    n00b_buffer_t *pre_bytes = slurp_path(path_c);
    uint8_t pre_mark_hash[32];
    sha256_of((const uint8_t *)pre_bytes->data,
              (size_t)pre_bytes->byte_len,
              pre_mark_hash);

    // (2) Envelope built against that pre-mark hash.
    n00b_attest_envelope_t *env = build_envelope_for_hash(pre_mark_hash);
    n00b_list_t(n00b_attest_envelope_t *) envs =
        n00b_list_new(n00b_attest_envelope_t *);
    n00b_list_push(envs, env);

    // (3) Mark with the identity → triggers post-insert
    // Authenticode resign. The PE codec is in-band; the resign
    // computes the Authenticode hash over the marked bytes and
    // embeds a PKCS#7 SignedData blob in the cert table.
    auto mr = n00b_attest_mark_artifact(path_str,
                                         &envs,
                                         .signer_identity = identity);
    ASSERT_OK(mr);
    n00b_attest_mark_result_t *row = n00b_result_get(mr);
    assert(row != nullptr);
    assert(row->kind == N00B_CHALK_OUT_IN_BAND);

    // (4) Extract + IC-4 cross-check.
    auto er = n00b_attest_extract_from_artifact(path_str);
    ASSERT_OK(er);
    n00b_attest_extract_result_t *exrow = n00b_result_get(er);
    assert(exrow != nullptr);
    assert(exrow->envelopes != nullptr);
    assert(exrow->envelopes->len == 1);

    n00b_attest_envelope_t *out_env = exrow->envelopes->data[0];
    auto pr = n00b_attest_envelope_get_payload(out_env);
    ASSERT_OK(pr);
    n00b_buffer_t *pay = n00b_result_get(pr);
    auto sr = n00b_attest_statement_parse(pay);
    ASSERT_OK(sr);
    n00b_attest_statement_t *parsed_stmt = n00b_result_get(sr);

    // DF-028 closure: typed accessor replaces textual JSON scan.
    n00b_string_t *got = n00b_attest_subject_get_digest_sha256(parsed_stmt, 0);
    assert(got != nullptr);
    assert(got->u8_bytes == 64);

    static const char hex[] = "0123456789abcdef";
    char expected_hex[65];
    for (int i = 0; i < 32; i++) {
        expected_hex[i * 2 + 0] = hex[(pre_mark_hash[i] >> 4) & 0xf];
        expected_hex[i * 2 + 1] = hex[pre_mark_hash[i] & 0xf];
    }
    expected_hex[64] = '\0';
    int cmp = memcmp(got->data, expected_hex, 64);
    if (cmp != 0) {
        fprintf(stderr, "  digest mismatch:\n    extracted: %.64s\n"
                        "    pre-mark : %s\n",
                got->data, expected_hex);
        assert(0);
    }

    // (5) Structural assertion on the re-signed binary: parses
    // cleanly + exactly one cert + non-zero cert blob.
    n00b_buffer_t *post_bytes = slurp_path(path_c);
    n00b_bstream_t *bs = n00b_bstream_new(post_bytes);
    auto pe_pr = n00b_pe_parse(bs);
    ASSERT_OK(pe_pr);
    n00b_pe_binary_t *pe_bin = n00b_result_get(pe_pr);
    assert(pe_bin->num_certificates == 1);
    assert(pe_bin->certificates[0].raw_data != nullptr);
    assert(pe_bin->certificates[0].raw_data->byte_len > 0);
    assert(pe_bin->certificates[0].revision == 0x0200);
    assert(pe_bin->certificates[0].certificate_type == 0x0002);
    fprintf(stderr,
            "  re-signed PE: %d cert(s), cert blob=%zu bytes\n",
            pe_bin->num_certificates,
            pe_bin->certificates[0].raw_data->byte_len);

    n00b_chalk_signer_identity_release(identity);
    unlink(path_c);
    fprintf(stderr,
            "  [PASS] pe_round_trip_with_resign "
            "(pre-mark digest verified end-to-end)\n");
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    fprintf(stderr, "test_attest_mark_pe_e2e:\n");
    test_pe_round_trip_with_resign();
    fprintf(stderr, "All attest mark PE E2E tests passed.\n");
    return 0;
}
