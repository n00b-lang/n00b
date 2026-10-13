#include <stdio.h>
#include <string.h>

#include "zlib_writ.h"

static int
fail(const char *what)
{
    fprintf(stderr, "roundtrip: %s\n", what);
    return 1;
}

int
main(void)
{
    uint8_t original[4096];
    for (size_t i = 0; i < sizeof(original); i++) {
        original[i] = (uint8_t)("writ contracts "[i % 15]);
    }
    zlib_bytes_t packed = {0};
    int status = 0;
    if (zlib_compress2(original, sizeof(original), 9, &packed, &status) != zlib_compress2_ok) {
        return fail("compress2 failed");
    }
    if (packed.length >= sizeof(original)) {
        return fail("compress2 did not compress");
    }
    zlib_bytes_t unpacked = {0};
    if (zlib_uncompress(sizeof(original), packed.data, packed.length, &unpacked, &status) != zlib_uncompress_ok) {
        return fail("uncompress failed");
    }
    if (unpacked.length != sizeof(original) || memcmp(unpacked.data, original, sizeof(original))) {
        return fail("round trip changed the data");
    }
    zlib_bytes_t truncated = {0};
    if (zlib_uncompress(16, packed.data, packed.length, &truncated, &status) != zlib_uncompress_error) {
        return fail("a too-small capacity was not reported");
    }
    if (status != -5 || truncated.data) {
        return fail("the error outcome did not carry Z_BUF_ERROR");
    }
    zlib_bytes_t empty = {0};
    if (zlib_compress2(NULL, 0, 6, &empty, &status) != zlib_compress2_ok || !empty.length) {
        return fail("compressing nothing failed");
    }
    if (zlib_compressBound(1000) < 1000) {
        return fail("compressBound returned less than its input");
    }
    zlib_bytes_free(&packed);
    zlib_bytes_free(&unpacked);
    zlib_bytes_free(&empty);
    printf("roundtrip ok\n");
    return 0;
}
