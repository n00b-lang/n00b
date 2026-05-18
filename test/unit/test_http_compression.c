/*
 * test_http_compression.c — Phase 6 chunk 8 unit tests.
 *
 * Coverage:
 *   - identity / nullptr encoding pass-through
 *   - gzip + deflate round-trip via libz
 *   - max_size cap defends against zip-bombs
 *   - unknown encoding rejected
 *   - Accept-Encoding header always includes gzip + deflate;
 *     includes br / zstd opportunistically
 *   - brotli + zstd: assert decode error when codec absent
 *     (cannot easily round-trip without the encoder side)
 */

#define N00B_USE_INTERNAL_API
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <zlib.h>

#include "n00b.h"
#include "core/runtime.h"
#include "core/buffer.h"
#include "core/string.h"
#include "adt/result.h"
#include "internal/net/http/http_compression.h"

static n00b_buffer_t *
B(const char *bytes, size_t len)
{
    return n00b_buffer_from_bytes((char *)bytes, (int64_t)len);
}

static n00b_buffer_t *
gzip_encode(const char *plain, size_t len)
{
    /* Use libz directly to compress so we have valid wire bytes
     * to feed to the decoder. */
    z_stream zs = {0};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return nullptr;
    }
    size_t   cap = (len < 256 ? 256 : len * 2);
    uint8_t *out = malloc(cap);
    zs.next_in   = (Bytef *)plain;
    zs.avail_in  = (uInt)len;
    size_t off = 0;
    int rc;
    do {
        if (off >= cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        zs.next_out  = out + off;
        zs.avail_out = (uInt)(cap - off);
        rc = deflate(&zs, Z_FINISH);
        off += (cap - off) - zs.avail_out;
    } while (rc == Z_OK);
    deflateEnd(&zs);
    n00b_buffer_t *b = n00b_buffer_from_bytes((char *)out, (int64_t)off);
    free(out);
    return b;
}

static n00b_buffer_t *
deflate_encode(const char *plain, size_t len)
{
    /* Standard zlib-wrapped deflate. */
    z_stream zs = {0};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return nullptr;
    }
    size_t   cap = (len < 256 ? 256 : len * 2);
    uint8_t *out = malloc(cap);
    zs.next_in   = (Bytef *)plain;
    zs.avail_in  = (uInt)len;
    size_t off = 0;
    int rc;
    do {
        if (off >= cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        zs.next_out  = out + off;
        zs.avail_out = (uInt)(cap - off);
        rc = deflate(&zs, Z_FINISH);
        off += (cap - off) - zs.avail_out;
    } while (rc == Z_OK);
    deflateEnd(&zs);
    n00b_buffer_t *b = n00b_buffer_from_bytes((char *)out, (int64_t)off);
    free(out);
    return b;
}

/* ---- Tests ---- */

static void
test_identity(void)
{
    n00b_buffer_t *src = B("hello world", 11);
    auto r = n00b_http_decompress(src, nullptr);
    assert(n00b_result_is_ok(r));
    n00b_buffer_t *out = n00b_result_get(r);
    assert(out->byte_len == 11);
    assert(memcmp(out->data, "hello world", 11) == 0);

    auto r2 = n00b_http_decompress(src, "");
    assert(n00b_result_is_ok(r2));

    auto r3 = n00b_http_decompress(src, "identity");
    assert(n00b_result_is_ok(r3));
    n00b_buffer_t *out3 = n00b_result_get(r3);
    assert(out3->byte_len == 11);
    printf("  [PASS] identity / empty / nullptr encoding pass-through\n");
}

static void
test_gzip_round_trip(void)
{
    static const char plain[] =
        "The quick brown fox jumps over the lazy dog. "
        "The quick brown fox jumps over the lazy dog.";
    size_t plen = sizeof(plain) - 1;

    n00b_buffer_t *enc = gzip_encode(plain, plen);
    assert(enc);
    /* Compressed payload should be smaller than original for repetitive
     * input. */
    assert((size_t)enc->byte_len < plen);

    auto r = n00b_http_decompress(enc, "gzip");
    assert(n00b_result_is_ok(r));
    n00b_buffer_t *dec = n00b_result_get(r);
    assert((size_t)dec->byte_len == plen);
    assert(memcmp(dec->data, plain, plen) == 0);

    /* Case-insensitive. */
    auto r2 = n00b_http_decompress(enc, "GZIP");
    assert(n00b_result_is_ok(r2));
    /* x-gzip alias. */
    auto r3 = n00b_http_decompress(enc, "x-gzip");
    assert(n00b_result_is_ok(r3));
    printf("  [PASS] gzip round-trip (case-insensitive + x-gzip alias)\n");
}

static void
test_deflate_round_trip(void)
{
    static const char plain[] =
        "Hello hello hello hello — repeated for repetition";
    size_t plen = sizeof(plain) - 1;

    n00b_buffer_t *enc = deflate_encode(plain, plen);
    assert(enc);

    auto r = n00b_http_decompress(enc, "deflate");
    assert(n00b_result_is_ok(r));
    n00b_buffer_t *dec = n00b_result_get(r);
    assert((size_t)dec->byte_len == plen);
    assert(memcmp(dec->data, plain, plen) == 0);
    printf("  [PASS] deflate (zlib-wrapped) round-trip\n");
}

static void
test_max_size_cap(void)
{
    /* Compress a large repetitive payload. */
    const size_t big = 256 * 1024;
    char *plain = malloc(big);
    memset(plain, 'A', big);
    n00b_buffer_t *enc = gzip_encode(plain, big);
    free(plain);
    assert(enc);

    /* Decompress with max_size = 1 KiB → expect failure. */
    auto r = n00b_http_decompress(enc, "gzip", .max_size = 1024);
    assert(n00b_result_is_err(r));
    printf("  [PASS] max_size cap rejects oversized decompression\n");
}

static void
test_unknown_encoding(void)
{
    n00b_buffer_t *src = B("data", 4);
    auto r = n00b_http_decompress(src, "snappy");
    assert(n00b_result_is_err(r));
    printf("  [PASS] unknown Content-Encoding rejected\n");
}

static void
test_corrupt_gzip(void)
{
    n00b_buffer_t *bad = B("\x1f\x8b not really gzip", 18);
    auto r = n00b_http_decompress(bad, "gzip");
    assert(n00b_result_is_err(r));
    printf("  [PASS] corrupt gzip rejected\n");
}

static void
test_accept_encoding_header(void)
{
    n00b_string_t *h = n00b_http_accept_encoding_header();
    assert(h);
    /* gzip + deflate are always present. */
    assert(strstr(h->data, "gzip"));
    assert(strstr(h->data, "deflate"));
    /* br / zstd may or may not be present depending on host;
     * if present, must follow gzip. */
    if (n00b_http_have_brotli()) {
        assert(strstr(h->data, "br"));
    }
    if (n00b_http_have_zstd()) {
        assert(strstr(h->data, "zstd"));
    }
    printf("  [PASS] Accept-Encoding always includes gzip+deflate "
           "(brotli=%d, zstd=%d)\n",
           (int)n00b_http_have_brotli(), (int)n00b_http_have_zstd());
}

static void
test_brotli_codec_absence(void)
{
    if (n00b_http_have_brotli()) {
        printf("  [SKIP] brotli is present on this host (cannot exercise "
               "absence path)\n");
        return;
    }
    n00b_buffer_t *bytes = B("\x00\x01\x02\x03", 4);
    auto r = n00b_http_decompress(bytes, "br");
    assert(n00b_result_is_err(r));
    printf("  [PASS] br decode errors when libbrotlidec absent\n");
}

static void
test_zstd_codec_absence(void)
{
    if (n00b_http_have_zstd()) {
        printf("  [SKIP] zstd is present on this host (cannot exercise "
               "absence path)\n");
        return;
    }
    n00b_buffer_t *bytes = B("\x28\xb5\x2f\xfd", 4);
    auto r = n00b_http_decompress(bytes, "zstd");
    assert(n00b_result_is_err(r));
    printf("  [PASS] zstd decode errors when libzstd absent\n");
}

/* ---- Body compression (request side) ---- */

static void
test_compress_round_trip_gzip(void)
{
    static const char plain[] =
        "Repeated text repeated text repeated text repeated text. "
        "Repeated text repeated text repeated text repeated text. ";
    size_t plen = sizeof(plain) - 1;

    n00b_buffer_t *src = B(plain, plen);
    auto cr = n00b_http_compress(src, "gzip");
    assert(n00b_result_is_ok(cr));
    n00b_buffer_t *enc = n00b_result_get(cr);
    assert((size_t)enc->byte_len < plen);   /* did real work */

    /* Round-trip via the decompressor. */
    auto dr = n00b_http_decompress(enc, "gzip");
    assert(n00b_result_is_ok(dr));
    n00b_buffer_t *dec = n00b_result_get(dr);
    assert((size_t)dec->byte_len == plen);
    assert(memcmp(dec->data, plain, plen) == 0);
    printf("  [PASS] body-side gzip compress + round-trip\n");
}

static void
test_compress_round_trip_deflate(void)
{
    static const char plain[] = "deflate me — short payload";
    size_t plen = sizeof(plain) - 1;

    n00b_buffer_t *src = B(plain, plen);
    auto cr = n00b_http_compress(src, "deflate");
    assert(n00b_result_is_ok(cr));

    auto dr = n00b_http_decompress(n00b_result_get(cr), "deflate");
    assert(n00b_result_is_ok(dr));
    n00b_buffer_t *dec = n00b_result_get(dr);
    assert((size_t)dec->byte_len == plen);
    assert(memcmp(dec->data, plain, plen) == 0);
    printf("  [PASS] body-side deflate compress + round-trip\n");
}

static void
test_compress_unsupported_encoding(void)
{
    n00b_buffer_t *src = B("hello", 5);
    auto cr = n00b_http_compress(src, "br");   /* not yet on the request side */
    assert(n00b_result_is_err(cr));
    cr = n00b_http_compress(src, "snappy");
    assert(n00b_result_is_err(cr));
    printf("  [PASS] body-side compress rejects unsupported codecs\n");
}

static void
test_compress_identity_passthrough(void)
{
    n00b_buffer_t *src = B("identity wire", 13);
    auto cr = n00b_http_compress(src, nullptr);
    assert(n00b_result_is_ok(cr));
    n00b_buffer_t *out = n00b_result_get(cr);
    assert(out->byte_len == 13);
    assert(memcmp(out->data, "identity wire", 13) == 0);

    auto cr2 = n00b_http_compress(src, "identity");
    assert(n00b_result_is_ok(cr2));
    printf("  [PASS] body-side identity / nullptr is pass-through\n");
}

int
main(int argc, char **argv)
{
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);

    printf("test_http_compression:\n");
    test_identity();
    test_gzip_round_trip();
    test_deflate_round_trip();
    test_max_size_cap();
    test_unknown_encoding();
    test_corrupt_gzip();
    test_accept_encoding_header();
    test_brotli_codec_absence();
    test_zstd_codec_absence();
    test_compress_round_trip_gzip();
    test_compress_round_trip_deflate();
    test_compress_unsupported_encoding();
    test_compress_identity_passthrough();
    printf("All test_http_compression tests passed.\n");

    n00b_shutdown();
    return 0;
}
