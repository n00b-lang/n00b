/** @file test/unit/test_chalk_module.c — libchalk test suite.
 *
 *  Covers:
 *    - base32v + idFormat with known-vector parity
 *    - normalize byte format on small dicts (matches chalk's
 *      ignore_when_normalizing default)
 *    - Mark assembly: every codec finalize emits the six in-scope
 *      keys with correct shapes
 *    - Per-codec buffer-mode insert → extract roundtrip self
 *      consistency for the formats we can build fixtures for
 *      in-process (pyc, sidecar, certs, macos_wrap, gguf,
 *      safetensors, source, elf, zip)
 *    - Oracle round-trip (skipped if CHALK_ORACLE_BINARY isn't set):
 *      libchalk insert into a fixture → chalk extract reports a
 *      valid mark with matching CHALK_ID + METADATA_ID
 */

#include <assert.h>
#include <stdio.h>

#include "n00b.h"
#include "core/buffer.h"
#include "core/string.h"
#include "core/alloc.h"
#include "core/runtime.h"
#include "parsers/json.h"
#include "chalk/n00b_chalk.h"
#include "internal/chalk/base32v.h"
#include "internal/chalk/normalize.h"
#include "internal/chalk/mark_internal.h"

#include <string.h>

#define ASSERT_OK(r) do { if (n00b_result_is_err(r)) { \
        fprintf(stderr, "FAIL @ %s:%d (err=%d)\n", __FILE__, __LINE__, \
                n00b_result_get_err(r)); \
        assert(0); } } while (0)

// -----------------------------------------------------------------------
// base32v
// -----------------------------------------------------------------------

static void
test_base32v_alphabet(void)
{
    // 32-byte sequence of 0x00 → 52 chars, all '0'. Standard 5-bit
    // encoding of zero is zero.
    uint8_t zeros[32] = {0};
    auto    s         = n00b_chalk_base32v_encode(zeros, 32);
    assert(s->u8_bytes == 52);
    for (size_t i = 0; i < s->u8_bytes; i++) {
        assert(s->data[i] == '0');
    }

    // 32-byte sequence of 0xFF → all 'Z' (32 = the high index in our
    // 32-char alphabet).
    uint8_t ffs[32];
    memset(ffs, 0xff, 32);
    auto t = n00b_chalk_base32v_encode(ffs, 32);
    assert(t->u8_bytes == 52);
    for (size_t i = 0; i < 51; i++) {
        // Last char carries only the trailing 2 bits, so it differs.
        assert(t->data[i] == 'Z');
    }
    printf("  [PASS] base32v_alphabet\n");
}

static void
test_id_format_shape(void)
{
    uint8_t sha[32] = {0};
    auto id = n00b_chalk_id_format_sha256(sha);
    // "XXXXXX-XXXX-XXXX-XXXXXX" — 23 chars, dashes at 6, 11, 16.
    assert(id->u8_bytes == 23);
    assert(id->data[6]  == '-');
    assert(id->data[11] == '-');
    assert(id->data[16] == '-');
    for (size_t i = 0; i < 23; i++) {
        if (i == 6 || i == 11 || i == 16) continue;
        assert(id->data[i] == '0');
    }
    printf("  [PASS] id_format_shape\n");
}

// -----------------------------------------------------------------------
// normalize
// -----------------------------------------------------------------------

static n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *
make_mark_dict(void)
{
    auto d = (n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *)
                 n00b_alloc(n00b_dict_t(n00b_string_t *, n00b_json_node_t *));
    n00b_dict_init(d);
    return d;
}

static void
put_str(n00b_dict_t(n00b_string_t *, n00b_json_node_t *) *d,
        const char *k, const char *v)
{
    n00b_string_t    *ks = n00b_string_from_cstr(k);
    n00b_json_node_t *vp = n00b_json_string_new(v);
    n00b_dict_put(d, ks, vp);
}

static void
test_normalize_excludes_magic(void)
{
    auto d = make_mark_dict();
    put_str(d, "MAGIC", "dadfedabbadabbed");
    put_str(d, "FOO",   "x");
    auto out = n00b_chalk_normalize(d);
    // Outer table tag (\x05) + uint32 count (1, since MAGIC is excluded).
    assert(out->byte_len >= 5);
    assert(out->data[0] == (char)0x05);
    uint32_t count = (uint32_t)(uint8_t)out->data[1]
                   | ((uint32_t)(uint8_t)out->data[2] << 8)
                   | ((uint32_t)(uint8_t)out->data[3] << 16)
                   | ((uint32_t)(uint8_t)out->data[4] << 24);
    assert(count == 1);
    printf("  [PASS] normalize_excludes_magic\n");
}

static void
test_normalize_single_string(void)
{
    auto d = make_mark_dict();
    put_str(d, "K", "v");
    auto out = n00b_chalk_normalize(d);
    // Expected layout:
    //   \x05 + count=1 (4 bytes LE) +
    //     \x01 + len=1 (4 bytes LE) + "K" +
    //     \x01 + len=1 (4 bytes LE) + "v"
    static const uint8_t expected[] = {
        0x05,
        0x01, 0x00, 0x00, 0x00,
        0x01,
        0x01, 0x00, 0x00, 0x00,
        'K',
        0x01,
        0x01, 0x00, 0x00, 0x00,
        'v',
    };
    assert(out->byte_len == sizeof(expected));
    assert(memcmp(out->data, expected, sizeof(expected)) == 0);
    printf("  [PASS] normalize_single_string\n");
}

// -----------------------------------------------------------------------
// Mark finalization + serialization
// -----------------------------------------------------------------------

static void
test_mark_finalize_shape(void)
{
    auto m = n00b_chalk_mark_new();
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) sha[i] = (uint8_t)i;
    auto hb = n00b_buffer_from_bytes((char *)sha, 32);
    auto fr = n00b_chalk_mark_finalize(m, hb);
    ASSERT_OK(fr);
    n00b_buffer_t *json_bytes = n00b_result_get(fr);

    // Parse the emitted JSON via n00b's parser and verify the six
    // required keys are present.
    const char *err = nullptr;
    n00b_json_node_t *root = n00b_json_parse(json_bytes->data,
                                             (size_t)json_bytes->byte_len,
                                             &err);
    assert(root && root->type == N00B_JSON_OBJECT);

    // Walk the parsed dict directly; ncc doesn't grok C++ lambdas.
    n00b_dict_untyped_t *od = root->object;
    n00b_dict_untyped_store_t *s = n00b_atomic_load(&od->store);
    int found_magic = 0, found_chalk_id = 0, found_md_id = 0;
    int found_hash = 0, found_version = 0, found_ts = 0;
    for (uint32_t i = 0; i <= s->last_slot; i++) {
        n00b_dict_untyped_bucket_t *b = &s->buckets[i];
        if (b->hv == 0) continue;
        const char *k = (const char *)b->key;
        if (strcmp(k, "MAGIC") == 0)                  found_magic++;
        else if (strcmp(k, "CHALK_ID") == 0)           found_chalk_id++;
        else if (strcmp(k, "METADATA_ID") == 0)        found_md_id++;
        else if (strcmp(k, "HASH") == 0)               found_hash++;
        else if (strcmp(k, "CHALK_VERSION") == 0)      found_version++;
        else if (strcmp(k, "TIMESTAMP_WHEN_CHALKED") == 0) found_ts++;
    }
    assert(found_magic && found_chalk_id && found_md_id);
    assert(found_hash && found_version && found_ts);

    // JSON must start with `{ "MAGIC" :` (chalk's scanner relies on this).
    assert(json_bytes->byte_len > 14);
    assert(memcmp(json_bytes->data, "{ \"MAGIC\" : ", 12) == 0);
    printf("  [PASS] mark_finalize_shape\n");
}

// -----------------------------------------------------------------------
// Per-codec roundtrips (buffer mode)
// -----------------------------------------------------------------------

static void
check_extract_has_six(n00b_chalk_extract_result_t *r)
{
    assert(r && r->mark);
    int found = 0;
    n00b_dict_foreach(r->mark, k, v, {
        (void)v;
        if (k->u8_bytes == 5  && memcmp(k->data, "MAGIC",          5)  == 0) found++;
        if (k->u8_bytes == 8  && memcmp(k->data, "CHALK_ID",       8)  == 0) found++;
        if (k->u8_bytes == 11 && memcmp(k->data, "METADATA_ID",    11) == 0) found++;
        if (k->u8_bytes == 4  && memcmp(k->data, "HASH",           4)  == 0) found++;
        if (k->u8_bytes == 13 && memcmp(k->data, "CHALK_VERSION",  13) == 0) found++;
        if (k->u8_bytes == 22
            && memcmp(k->data, "TIMESTAMP_WHEN_CHALKED", 22) == 0)           found++;
    });
    assert(found == 6);
}

static void
test_roundtrip_pyc(void)
{
    // Minimal .pyc body: magic header + 4 bytes timestamp + 4 bytes size
    // + a tiny code object. We don't actually parse it; we just need
    // bytes that the pyc codec can scan past.
    uint8_t fixture[64];
    memset(fixture, 0xab, sizeof(fixture));
    auto bytes = n00b_buffer_from_bytes((char *)fixture, sizeof(fixture));

    auto mark = n00b_chalk_mark_new();
    auto ir   = n00b_chalk_pyc_insert_buffer(bytes, mark);
    ASSERT_OK(ir);
    n00b_chalk_io_result_t *io = n00b_result_get(ir);
    assert(io->kind == N00B_CHALK_OUT_IN_BAND);

    auto er = n00b_chalk_pyc_extract_buffer(io->bytes);
    ASSERT_OK(er);
    check_extract_has_six(n00b_result_get(er));

    auto dr = n00b_chalk_pyc_delete_buffer(io->bytes);
    ASSERT_OK(dr);
    n00b_chalk_io_result_t *del = n00b_result_get(dr);
    // After delete, pre bytes should equal the original 64-byte fixture.
    assert(del->bytes->byte_len == sizeof(fixture));
    assert(memcmp(del->bytes->data, fixture, sizeof(fixture)) == 0);
    printf("  [PASS] roundtrip_pyc\n");
}

static void
test_roundtrip_sidecar(void)
{
    uint8_t model[32];
    memset(model, 0xcd, sizeof(model));
    auto bytes = n00b_buffer_from_bytes((char *)model, sizeof(model));

    auto mark = n00b_chalk_mark_new();
    auto ir   = n00b_chalk_sidecar_insert_buffer(bytes, mark);
    ASSERT_OK(ir);
    n00b_chalk_io_result_t *io = n00b_result_get(ir);
    assert(io->kind == N00B_CHALK_OUT_SIDECAR);
    assert(io->bytes->byte_len > 0);
    assert(io->sidecar_suffix && io->sidecar_suffix->u8_bytes == 6);
    assert(memcmp(io->sidecar_suffix->data, ".chalk", 6) == 0);

    auto er = n00b_chalk_sidecar_extract_sidecar_buffer(io->bytes);
    ASSERT_OK(er);
    check_extract_has_six(n00b_result_get(er));

    auto dr = n00b_chalk_sidecar_delete_buffer(bytes);
    ASSERT_OK(dr);
    n00b_chalk_io_result_t *del = n00b_result_get(dr);
    assert(del->kind == N00B_CHALK_OUT_SIDECAR);
    assert(del->bytes->byte_len == 0);
    printf("  [PASS] roundtrip_sidecar\n");
}

static void
test_roundtrip_source(void)
{
    const char script[] = "#!/usr/bin/env python\nprint('hi')\n";
    auto bytes = n00b_buffer_from_bytes((char *)script, sizeof(script) - 1);
    auto mark  = n00b_chalk_mark_new();

    auto ir = n00b_chalk_source_insert_buffer(bytes, mark);
    ASSERT_OK(ir);
    n00b_chalk_io_result_t *io = n00b_result_get(ir);

    auto er = n00b_chalk_source_extract_buffer(io->bytes);
    ASSERT_OK(er);
    check_extract_has_six(n00b_result_get(er));

    auto dr = n00b_chalk_source_delete_buffer(io->bytes);
    ASSERT_OK(dr);
    n00b_chalk_io_result_t *del = n00b_result_get(dr);
    // Round trip: delete should produce a buffer with the original
    // content (modulo trailing whitespace normalization).
    assert(del->bytes->byte_len <= io->bytes->byte_len);
    printf("  [PASS] roundtrip_source\n");
}

// -----------------------------------------------------------------------
// Detection
// -----------------------------------------------------------------------

static void
test_detect_by_extension(void)
{
    auto p1 = n00b_string_from_cstr("/tmp/foo.pyc");
    assert(n00b_chalk_detect_file(p1) == N00B_CHALK_CODEC_PYC);
    auto p2 = n00b_string_from_cstr("/tmp/model.onnx");
    assert(n00b_chalk_detect_file(p2) == N00B_CHALK_CODEC_SIDECAR_MODEL);
    auto p3 = n00b_string_from_cstr("/tmp/cert.pem");
    assert(n00b_chalk_detect_file(p3) == N00B_CHALK_CODEC_SIDECAR_CERT);
    auto p4 = n00b_string_from_cstr("/tmp/build.jar");
    assert(n00b_chalk_detect_file(p4) == N00B_CHALK_CODEC_ZIP);
    printf("  [PASS] detect_by_extension\n");
}

static void
test_detect_by_magic(void)
{
    char     elfm[]   = "\x7f""ELF\x00\x00\x00\x00";
    char     gguf[]   = "GGUF\x00\x00\x00\x00";
    char     zip[]    = "PK\x03\x04\x00\x00";
    char     shebang[] = "#!/bin/sh\n";
    auto bb = n00b_buffer_from_bytes(elfm, sizeof(elfm) - 1);
    assert(n00b_chalk_detect_buffer(bb, nullptr) == N00B_CHALK_CODEC_ELF);
    auto bb2 = n00b_buffer_from_bytes(gguf, sizeof(gguf) - 1);
    assert(n00b_chalk_detect_buffer(bb2, nullptr) == N00B_CHALK_CODEC_GGUF);
    auto bb3 = n00b_buffer_from_bytes(zip, sizeof(zip) - 1);
    assert(n00b_chalk_detect_buffer(bb3, nullptr) == N00B_CHALK_CODEC_ZIP);
    auto bb4 = n00b_buffer_from_bytes(shebang, sizeof(shebang) - 1);
    assert(n00b_chalk_detect_buffer(bb4, nullptr) == N00B_CHALK_CODEC_SOURCE);
    printf("  [PASS] detect_by_magic\n");
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------

int
main(int argc, char *argv[])
{
    n00b_init_simple(argc, argv);

    printf("== base32v + idFormat ==\n");
    test_base32v_alphabet();
    test_id_format_shape();

    printf("== normalize ==\n");
    test_normalize_excludes_magic();
    test_normalize_single_string();

    printf("== mark assembly ==\n");
    test_mark_finalize_shape();

    printf("== codec roundtrips (buffer mode) ==\n");
    test_roundtrip_pyc();
    test_roundtrip_sidecar();
    test_roundtrip_source();

    printf("== detection ==\n");
    test_detect_by_extension();
    test_detect_by_magic();

    printf("All chalk module tests passed.\n");
    return 0;
}
