#include <stdio.h>
#include <string.h>

#include "zlib_writ.h"

static int failures;

static void
expect(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static const char text[] = "writ streams this text through deflate and inflate, twice over: "
                           "writ streams this text through deflate and inflate, twice over.";

static void
checksums(void)
{
    unsigned long crc = 0;
    unsigned long adler = 0;
    expect(zlib_crc32(0, (const uint8_t *)"hello", 5, &crc) == zlib_crc32_ok && crc == 0x3610a686UL, "crc32 of hello");
    expect(zlib_adler32(1, (const uint8_t *)"hello", 5, &adler) == zlib_adler32_ok && adler == 0x062c0215UL,
           "adler32 of hello");
    expect(zlib_crc32(0, NULL, 0, &crc) == zlib_crc32_ok && crc == 0, "an empty buffer may be null");
    const char *version = NULL;
    expect(zlib_zlibVersion(&version) == zlib_zlibVersion_ok && version && version[0] == ZLIB_VERSION[0],
           "zlibVersion is static text");
}

static size_t
deflate_all(uint8_t *out, size_t capacity)
{
    z_stream stream;
    memset(&stream, 0, sizeof stream);
    int status = 0;
    expect(zlib_deflateInit(&stream, Z_BEST_COMPRESSION, &status) == zlib_deflateInit_ok, "deflateInit");
    stream.next_in = (unsigned char *)text;
    stream.avail_in = sizeof text;
    stream.next_out = out;
    stream.avail_out = (unsigned)capacity;
    int32_t outcome = zlib_deflate_progress;
    while (outcome == zlib_deflate_progress) {
        outcome = zlib_deflate(&stream, Z_FINISH, &status);
    }
    expect(outcome == zlib_deflate_done, "deflate finishes");
    size_t produced = stream.total_out;
    expect(zlib_deflateEnd(&stream, &status) == zlib_deflateEnd_ok, "deflateEnd");
    return produced;
}

static void
streaming(void)
{
    uint8_t packed[512];
    size_t length = deflate_all(packed, sizeof packed);
    expect(length > 0 && length < sizeof text, "deflate compresses");

    z_stream stream;
    memset(&stream, 0, sizeof stream);
    int status = 0;
    expect(zlib_inflateInit(&stream, &status) == zlib_inflateInit_ok, "inflateInit");
    uint8_t unpacked[sizeof text];
    stream.next_in = packed;
    stream.avail_in = (unsigned)length;
    size_t total = 0;
    int32_t outcome = zlib_inflate_progress;
    while (outcome == zlib_inflate_progress) {
        stream.next_out = unpacked + total;
        stream.avail_out = (unsigned)(sizeof unpacked - total < 16 ? sizeof unpacked - total : 16);
        char *detail = NULL;
        outcome = zlib_inflate(&stream, Z_NO_FLUSH, &detail, &status);
        total = stream.total_out;
    }
    expect(outcome == zlib_inflate_done, "inflate reaches the end of the stream in small steps");
    expect(total == sizeof text && !memcmp(unpacked, text, sizeof text), "inflate round-trips");
    expect(zlib_inflateEnd(&stream, &status) == zlib_inflateEnd_ok, "inflateEnd");

    memset(&stream, 0, sizeof stream);
    zlib_inflateInit(&stream, &status);
    uint8_t garbage[] = {0x78, 0x9c, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    stream.next_in = garbage;
    stream.avail_in = sizeof garbage;
    stream.next_out = unpacked;
    stream.avail_out = sizeof unpacked;
    char *detail = NULL;
    expect(zlib_inflate(&stream, Z_NO_FLUSH, &detail, &status) == zlib_inflate_corrupt, "corrupt input is classified");
    expect(detail && *detail, "the corrupt outcome carries zlib's message");
    zlib_inflateEnd(&stream, &status);

    memset(&stream, 0, sizeof stream);
    zlib_inflateInit(&stream, &status);
    stream.next_in = packed;
    stream.avail_in = 0;
    stream.next_out = unpacked;
    stream.avail_out = sizeof unpacked;
    expect(zlib_inflate(&stream, Z_NO_FLUSH, &detail, &status) == zlib_inflate_stalled, "no input stalls");
    zlib_inflateEnd(&stream, &status);
}

static void
one_shot(void)
{
    zlib_bytes_t packed = {0};
    zlib_bytes_t unpacked = {0};
    int status = 0;
    expect(zlib_compress2((const uint8_t *)text, sizeof text, 6, &packed, &status) == zlib_compress2_ok, "compress2");
    expect(zlib_uncompress(sizeof text, packed.data, packed.length, &unpacked, &status) == zlib_uncompress_ok,
           "uncompress");
    expect(unpacked.length == sizeof text && !memcmp(unpacked.data, text, sizeof text), "one-shot round trip");
    zlib_bytes_free(&packed);
    zlib_bytes_free(&unpacked);
}

int
main(void)
{
    checksums();
    streaming();
    one_shot();
    if (!failures) {
        printf("zlib ok\n");
    }
    return failures != 0;
}
