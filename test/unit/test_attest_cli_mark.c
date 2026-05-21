/** @file test/unit/test_attest_cli_mark.c — `n00b-attest mark`
 *  verb regression test (WP-005 Phase 2).
 *
 *  Drives the **library-shaped core** of the `mark` verb
 *  (`n00b_attest_cli_mark`) end-to-end via in-memory buffers per
 *  the plan's library-API-first framing (WP-002 plan §727): the
 *  test does NOT spawn the `n00b-attest` binary, does NOT redirect
 *  stdin/stdout, and does NOT shell out.
 *
 *  Coverage:
 *
 *    [A] Construct an envelope in-process, serialize to wire bytes,
 *        pass the bytes through `_cli_mark` (which parses + marks);
 *        assert the returned `n00b_attest_mark_result_t` carries
 *        the IC-4 unchalked SHA-256 byte-equal to libchalk's
 *        `hash_buffer` on the pre-mark file. Same IC-4 contract as
 *        `test_attest_mark_artifact.c`, but exercised through the
 *        Phase-2 envelope-bytes wrapper.
 *
 *    [B] `--lazy` (bundled = false) — the mark is produced
 *        successfully; subsequent `_extract_from_artifact` returns
 *        Ok with `bundled = false` and an empty envelopes list (the
 *        ATTESTATION JSON omits `envelopes[]` in lazy mode).
 *
 *    [C] Multi-envelope mark — pass TWO envelope-bytes blobs;
 *        assert `_cli_mark` succeeds and the post-mark extract
 *        returns both predicate types. Exercises the library
 *        surface's CR-13 multi-envelope contract (the slay
 *        repeatable-flag substrate is not yet available to the
 *        binary; the library still honors the multi-envelope shape).
 *
 *    [D] Null/empty input validation: null artifact_path, null list,
 *        empty list, and a list with a null entry all return
 *        `N00B_ATTEST_ERR_DSSE_BAD_INPUT`.
 *
 *  Test-file carve-out (D-030) applies — libc I/O for the tempfile
 *  setup and stdout logging is acceptable per the established
 *  test-file precedent. The fixture envelope reuses the
 *  `test_attest_mark_artifact.c` keypair (RFC 8032 §7.1 vector #1).
 */

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/runtime.h"
#include "text/strings/string_ops.h"
#include "attest/n00b_attest.h"

#include "chalk/n00b_chalk.h"
#include "compiler/objfile/elf.h"
#include "compiler/objfile/elf_build.h"
#include "compiler/objfile/elf_types.h"

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                (int)n00b_result_get_err(r)); \
        assert(0); } } while (0)

// ---------------------------------------------------------------------------
// Fixture builders.
// ---------------------------------------------------------------------------

static const char k_statement_provenance[] =
    "{\"_type\":\"https://in-toto.io/Statement/v1\","
    "\"subject\":[{\"name\":\"hello.elf\","
    "\"digest\":{\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}],"
    "\"predicateType\":\"https://slsa.dev/provenance/v1\","
    "\"predicate\":{\"builder\":{\"id\":\"test\"},\"buildType\":\"test\"}}";

static const char k_statement_sbom[] =
    "{\"_type\":\"https://in-toto.io/Statement/v1\","
    "\"subject\":[{\"name\":\"hello.elf\","
    "\"digest\":{\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}}],"
    "\"predicateType\":\"https://spdx.dev/Document\","
    "\"predicate\":{\"spdxVersion\":\"SPDX-2.3\"}}";

static const char k_signer_keyid[]
    = "06e3fd8fda29bb60ab59557de61edb0aecdb231134be30e75b455f8e1b792fa9";

static uint8_t k_signature[64];  // 64 zero bytes — algorithm-agnostic.

// Build an envelope from a Statement JSON payload, return its wire
// bytes (already canonical, ready to feed back into _cli_mark).
static n00b_buffer_t *
build_envelope_bytes(const char *stmt_json, size_t stmt_len)
{
    n00b_attest_envelope_t *env = n00b_attest_envelope_new();
    n00b_buffer_t          *pay = n00b_buffer_from_bytes((char *)stmt_json,
                                                         (int64_t)stmt_len);
    auto sp = n00b_attest_envelope_set_payload(env, pay);
    ASSERT_OK(sp);

    n00b_string_t *kid = n00b_string_from_cstr(k_signer_keyid);
    n00b_buffer_t *sig = n00b_buffer_from_bytes((char *)k_signature, 64);
    auto add = n00b_attest_envelope_add_signature(env, kid, sig);
    ASSERT_OK(add);

    auto sr = n00b_attest_envelope_serialize(env);
    ASSERT_OK(sr);
    return n00b_result_get(sr);
}

static n00b_buffer_t *
build_elf_fixture(void)
{
    auto bin = n00b_elf_binary_new(ET_EXEC, EM_X86_64);
    n00b_elf_section_t *text = n00b_elf_add_section(bin, ".text",
                                                     SHT_PROGBITS,
                                                     SHF_ALLOC | SHF_EXECINSTR);
    char text_bytes[16] = {0};
    text->content = n00b_buffer_from_bytes(text_bytes, sizeof(text_bytes));
    text->size    = sizeof(text_bytes);
    auto br = n00b_elf_build(bin);
    ASSERT_OK(br);
    return n00b_result_get(br);
}

static char *
write_elf_tempfile(n00b_buffer_t *bytes, const char *prefix)
{
    char tmpl[128];
    snprintf(tmpl, sizeof(tmpl), "/tmp/%s_XXXXXX", prefix);
    char *path = strdup(tmpl);
    int   fd   = mkstemp(path);
    assert(fd >= 0);
    ssize_t n = write(fd, bytes->data, (size_t)bytes->byte_len);
    assert(n == bytes->byte_len);
    close(fd);
    return path;
}

static n00b_buffer_t *
slurp_file(const char *path)
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

// ---------------------------------------------------------------------------
// [A] IC-4 unchalked-hash contract via the CLI wrapper.
// ---------------------------------------------------------------------------

static void
test_cli_mark_unchalked_hash(void)
{
    n00b_buffer_t *elf_bytes = build_elf_fixture();
    char          *path      = write_elf_tempfile(elf_bytes, "n00b_attest_cli_mark");
    n00b_string_t *path_str  = n00b_string_from_cstr(path);

    n00b_buffer_t *pre_bytes = slurp_file(path);
    auto pre_r = n00b_chalk_hash_buffer(pre_bytes);
    ASSERT_OK(pre_r);
    n00b_buffer_t *pre_hash = n00b_result_get(pre_r);
    assert(pre_hash->byte_len == 32);

    n00b_buffer_t *env_bytes = build_envelope_bytes(
        k_statement_provenance, sizeof(k_statement_provenance) - 1);

    n00b_list_t(n00b_buffer_t *) envs = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(envs, env_bytes);

    auto mr = n00b_attest_cli_mark(path_str, &envs);
    ASSERT_OK(mr);
    n00b_attest_mark_result_t *row = n00b_result_get(mr);
    assert(row != nullptr);
    assert(row->unchalked_sha256_32 != nullptr);
    assert(row->unchalked_sha256_32->byte_len == 32);
    assert(row->kind == N00B_CHALK_OUT_IN_BAND);

    assert(memcmp(row->unchalked_sha256_32->data,
                   pre_hash->data,
                   32)
            == 0);

    printf("  [PASS] cli_mark_unchalked_hash\n");
    unlink(path);
    free(path);
}

// ---------------------------------------------------------------------------
// [B] --lazy → bundled = false.
// ---------------------------------------------------------------------------

static void
test_cli_mark_lazy_mode(void)
{
    n00b_buffer_t *elf_bytes = build_elf_fixture();
    char          *path      = write_elf_tempfile(elf_bytes, "n00b_attest_cli_mark_lazy");
    n00b_string_t *path_str  = n00b_string_from_cstr(path);

    n00b_buffer_t *env_bytes = build_envelope_bytes(
        k_statement_provenance, sizeof(k_statement_provenance) - 1);

    n00b_list_t(n00b_buffer_t *) envs = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(envs, env_bytes);

    auto mr = n00b_attest_cli_mark(path_str, &envs, .bundled = false);
    ASSERT_OK(mr);

    // Post-mark extract: bundled = false, envelopes list empty,
    // predicate_types still has one entry.
    auto er = n00b_attest_cli_extract(path_str);
    ASSERT_OK(er);
    n00b_attest_extract_result_t *row = n00b_result_get(er);
    assert(row != nullptr);
    assert(row->bundled == false);
    assert(row->envelopes != nullptr);
    assert(row->envelopes->len == 0);
    assert(row->predicate_types != nullptr);
    assert(row->predicate_types->len == 1);

    printf("  [PASS] cli_mark_lazy_mode\n");
    unlink(path);
    free(path);
}

// ---------------------------------------------------------------------------
// [C] Multi-envelope (library-level — CR-13 contract).
// ---------------------------------------------------------------------------

static void
test_cli_mark_multi_envelope(void)
{
    n00b_buffer_t *elf_bytes = build_elf_fixture();
    char          *path      = write_elf_tempfile(elf_bytes, "n00b_attest_cli_mark_multi");
    n00b_string_t *path_str  = n00b_string_from_cstr(path);

    n00b_buffer_t *env1 = build_envelope_bytes(
        k_statement_provenance, sizeof(k_statement_provenance) - 1);
    n00b_buffer_t *env2 = build_envelope_bytes(
        k_statement_sbom, sizeof(k_statement_sbom) - 1);

    n00b_list_t(n00b_buffer_t *) envs = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(envs, env1);
    n00b_list_push(envs, env2);

    auto mr = n00b_attest_cli_mark(path_str, &envs);
    ASSERT_OK(mr);

    // Extract: both predicate types should appear.
    auto er = n00b_attest_cli_extract(path_str);
    ASSERT_OK(er);
    n00b_attest_extract_result_t *row = n00b_result_get(er);
    assert(row != nullptr);
    assert(row->predicate_types != nullptr);
    assert(row->predicate_types->len == 2);

    n00b_string_t *p0 = row->predicate_types->data[0];
    n00b_string_t *p1 = row->predicate_types->data[1];
    assert(p0 != nullptr && p1 != nullptr);

    bool seen_prov = false;
    bool seen_sbom = false;
    n00b_string_t *prov = n00b_string_from_cstr("https://slsa.dev/provenance/v1");
    n00b_string_t *sbom = n00b_string_from_cstr("https://spdx.dev/Document");
    if (n00b_unicode_str_eq(p0, prov) || n00b_unicode_str_eq(p1, prov)) {
        seen_prov = true;
    }
    if (n00b_unicode_str_eq(p0, sbom) || n00b_unicode_str_eq(p1, sbom)) {
        seen_sbom = true;
    }
    assert(seen_prov);
    assert(seen_sbom);

    printf("  [PASS] cli_mark_multi_envelope\n");
    unlink(path);
    free(path);
}

// ---------------------------------------------------------------------------
// [D] Null / empty input validation.
// ---------------------------------------------------------------------------

static void
test_cli_mark_null_inputs(void)
{
    n00b_buffer_t *elf_bytes = build_elf_fixture();
    char          *path      = write_elf_tempfile(elf_bytes, "n00b_attest_cli_mark_null");
    n00b_string_t *path_str  = n00b_string_from_cstr(path);

    n00b_buffer_t *env_bytes = build_envelope_bytes(
        k_statement_provenance, sizeof(k_statement_provenance) - 1);
    n00b_list_t(n00b_buffer_t *) envs = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(envs, env_bytes);

    n00b_list_t(n00b_buffer_t *) empty_envs = n00b_list_new(n00b_buffer_t *);

    n00b_list_t(n00b_buffer_t *) null_entry = n00b_list_new(n00b_buffer_t *);
    n00b_list_push(null_entry, (n00b_buffer_t *)nullptr);

    // (1) Null artifact path.
    auto r1 = n00b_attest_cli_mark(nullptr, &envs);
    assert(n00b_result_is_err(r1));
    assert(n00b_result_get_err(r1) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    // (2) Null envelope list.
    auto r2 = n00b_attest_cli_mark(path_str, nullptr);
    assert(n00b_result_is_err(r2));
    assert(n00b_result_get_err(r2) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    // (3) Empty envelope list.
    auto r3 = n00b_attest_cli_mark(path_str, &empty_envs);
    assert(n00b_result_is_err(r3));
    assert(n00b_result_get_err(r3) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    // (4) Null entry in envelope list.
    auto r4 = n00b_attest_cli_mark(path_str, &null_entry);
    assert(n00b_result_is_err(r4));
    assert(n00b_result_get_err(r4) == N00B_ATTEST_ERR_DSSE_BAD_INPUT);

    printf("  [PASS] cli_mark_null_inputs\n");
    unlink(path);
    free(path);
}

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);
    n00b_attest_module_init();

    printf("== n00b_attest CLI mark verb ==\n");
    test_cli_mark_unchalked_hash();
    test_cli_mark_lazy_mode();
    test_cli_mark_multi_envelope();
    test_cli_mark_null_inputs();
    printf("All n00b_attest CLI mark tests passed.\n");
    return 0;
}
